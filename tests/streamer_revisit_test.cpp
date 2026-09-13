#include "framework.hpp"

#include "core/entity/mob_spawner.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/frustum.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/sign_store.hpp"
#include "core/world/tile_entity.hpp"
#include "version_slots.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>

using namespace mc;
using render::ChunkRenderer;
using render::ChunkRendererConfig;
using render::SectionField;
using render::VboAllocator;
using render::VboTier;
using render::WorldStreamer;
using mc::ClipRange;
using mc::Frustum;
using mc::Mat4;

namespace {

constexpr i64 kNow = 1284768000000LL;

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_revisit_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }

    std::string world(const char* name) const { return std::string(path) + "/" + name; }
};

class TestAllocator : public VboAllocator {
public:
    void* allocate(usize bytes, VboTier) override { return std::malloc(bytes); }
    void release(void* pointer, usize, VboTier) override { std::free(pointer); }
};

Frustum openFrustum()
{
    Frustum f;
    f.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);
    return f;
}

void frame(WorldStreamer& streamer, ChunkRenderer& renderer, u32 n, i32 cx, i32 cz,
           const WorldStreamer::Budget& budget)
{
    renderer.beginFrame(n, openFrustum(), cx, 4, cz);
    streamer.update(renderer, cx, cz, budget);
}

// Runs until the world has nothing outstanding *and* the walk has stopped
// asking for meshes, which is a stricter stop than the generation tests use:
// this is about what is on screen, not about what is on the card.
int settle(WorldStreamer& streamer, ChunkRenderer& renderer, i32 cx, i32 cz,
           const WorldStreamer::Budget& budget, u32* counter, int maxFrames)
{
    int n = 0;
    for (; n < maxFrames; ++n) {
        frame(streamer, renderer, (*counter)++, cx, cz, budget);
        if (streamer.stats().pendingColumns == 0 && streamer.stats().pendingGeneration == 0
            && streamer.stats().pendingReads == 0 && streamer.generationIdle()
            && streamer.storageIdle() && renderer.meshQueue().empty()) {
            // One more frame, so the queue emptying is observed rather than
            // guessed: the walk runs at the top of a frame and the mesh at the
            // bottom of it.
            frame(streamer, renderer, (*counter)++, cx, cz, budget);
            if (renderer.meshQueue().empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return n;
}

// What every section in range has been meshed to: its geometry in bytes, or the
// two states that are not geometry. Keyed by coordinate rather than by cell, so
// the grid wrapping cannot make two snapshots agree by accident.
struct MeshShot {
    std::map<std::tuple<i32, int, i32>, long long> sections;

    // -1 never meshed, -2 meshed and empty; otherwise the byte size.
    static constexpr long long kNever = -1;
    static constexpr long long kEmpty = -2;
};

MeshShot shoot(const ChunkRenderer& renderer, i32 cx, i32 cz, int distance)
{
    MeshShot out;
    const SectionField& field = renderer.field();
    for (i32 x = cx - distance; x <= cx + distance; ++x) {
        for (i32 z = cz - distance; z <= cz + distance; ++z) {
            if (!field.isLoaded(x, z)) {
                continue;
            }
            for (int sy = 0; sy < SectionField::kSectionsY; ++sy) {
                const u16 slot = field.meshSlot(x, sy, z);
                long long value = MeshShot::kNever;
                if (slot == SectionField::kEmptyMesh) {
                    value = MeshShot::kEmpty;
                } else if (slot != SectionField::kNoMesh) {
                    value = static_cast<long long>(renderer.pool().size(slot));
                }
                out.sections[{x, sy, z}] = value;
            }
        }
    }
    return out;
}


// The two session stores, wired the way `src/platform/ctr/main.cpp` wires them.
// Kept here rather than in the platform layer for the same reason the rest of
// this file is: the ordering being tested is the streamer's.
struct TileStores {
    world::SignStore signs;
    entity::MobSpawnerStore spawners;

    static void adopted(void* ctx, const world::ChunkColumn& column)
    {
        auto& self = *static_cast<TileStores*>(ctx);
        entity::readMobSpawners(column.tileEntities, self.spawners);
        world::readSigns(column, self.signs);
    }

    static void saving(void* ctx, world::ChunkColumn& column)
    {
        auto& self = *static_cast<TileStores*>(ctx);
        world::reconcileTileEntities(column);
        world::writeSigns(self.signs, column);
        entity::writeMobSpawners(self.spawners, column);
    }

    static void dropped(void* ctx, i32 chunkX, i32 chunkZ)
    {
        auto& self = *static_cast<TileStores*>(ctx);
        self.signs.eraseColumn(chunkX, chunkZ);
        self.spawners.eraseColumn(chunkX, chunkZ);
    }

    // `cn.a(IIILic;)V` and `cn.l(III)V`, the other pair -- a block appearing
    // and going, rather than a column.
    static void added(void* ctx, i32 x, int y, i32 z)
    {
        static_cast<TileStores*>(ctx)->spawners.put(x, y, z);
    }

    static void removed(void* ctx, i32 x, int y, i32 z)
    {
        auto& self = *static_cast<TileStores*>(ctx);
        self.signs.erase(x, y, z);
        self.spawners.erase(x, y, z);
    }

    // Both pairs at once, which is what main.cpp does.
    void bind(WorldStreamer& streamer)
    {
        streamer.setColumnSinks(adopted, dropped, saving, this);
        if (tick::TickWorld* world = streamer.worldTick()) {
            world->setTileEntityAddedSink(added, this);
            world->setTileEntityRemovedSink(removed, this);
        }
    }
};

}  // namespace

// **After the camera stops, the ground under it must be made before the ground
// it has already left.**
//
// Reported from hardware: flying around leaves a border of chunks that never
// fill, a few appear if you wait a long time, and it comes right much later.
// That was a queue served in the wrong order -- strictly FIFO, so a player who
// outran the generator was behind every column they had already passed. A
// measured sprint at distance 8 ended with 702 columns queued, 341 of them out
// of range, and all 361 columns in range behind them.
//
// The queue is gone now and what is owed is read off the grid, so a column the
// camera has left is not merely served last: it is not owed at all until the
// player comes back. That makes this test the pin on the property rather than
// on the mechanism -- it would have failed on the FIFO queue, it passes on the
// nearest-first one, and it still passes with no queue.
//
// The assertion is deliberately **not** a time or a frame count -- both measure
// the host rather than the ordering. It counts *columns generated* between the
// camera stopping and the area around it being complete: the columns in range,
// plus whatever a sweep finishes on the way, and nothing else.
TEST(the_ground_under_a_stopped_camera_is_made_before_the_ground_it_left)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 90210LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    constexpr int kDistance = 3;

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = kDistance;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    world::ChunkCache::Config cache;
    cache.threaded = true;
    streamer.setCacheConfig(cache);
    CHECK(streamer.open(dir.c_str(), kDistance, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    u32 counter = 0;

    // Outrun it: one chunk every few frames, far enough to build a backlog the
    // generator cannot have finished.
    for (i32 x = 1; x <= 24; ++x) {
        for (int i = 0; i < 3; ++i) {
            frame(streamer, renderer, counter++, x, 0, budget);
        }
    }

    const int owedAtStop = streamer.stats().pendingGeneration;
    // The premise of the test: the camera really did outrun generation, so
    // there is ground both in front of it and behind it left to make. Without
    // this the assertion below could pass vacuously.
    CHECK(owedAtStop > 0);

    // Now stand still and count what gets made until the area is complete.
    int generated = 0;
    int frames = 0;
    for (; frames < 40000; ++frames) {
        frame(streamer, renderer, counter++, 24, 0, budget);
        generated += streamer.stats().generatedThisFrame;
        if (streamer.stats().pendingGeneration == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK_EQ(streamer.stats().pendingGeneration, 0);

    // Generous: nearest-first should spend roughly `owedAtStop` columns, plus
    // whatever a sweep finishes on the way. Oldest-first has to drain the whole
    // backlog, which is several times larger -- that is the difference the
    // border was made of.
    if (generated > owedAtStop * 3) {
        std::printf("  owed at stop %d, generated before filling %d\n", owedAtStop, generated);
    }
    CHECK(generated <= owedAtStop * 3);

    streamer.close(kNow);
    renderer.shutdown();
}

// **Walking away from a column and back must put the same geometry on screen.**
//
// Reported from hardware: revisited chunks came back with their lower sections
// missing -- the terrain appeared to start at sea level -- and some columns did
// not draw at all, and changing the render distance put both right. That last
// detail is the diagnosis: setMeshDistance rebuilds the field and the pool and
// republishes everything, so whatever was wrong lived in per-section renderer
// state that survived a drop and a re-publish.
//
// The camera goes far enough away that every column is dropped, then comes
// back. Nothing about the world has changed in between, so every section must
// mesh to exactly the byte count it did the first time -- including the ones
// that legitimately mesh to nothing, because a section wrongly marked empty is
// a section that never comes back.
TEST(revisiting_a_column_meshes_it_to_exactly_what_it_was)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 4242LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    constexpr int kDistance = 2;

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = kDistance;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    world::ChunkCache::Config cache;
    cache.threaded = true;
    streamer.setCacheConfig(cache);
    streamer.setPrefetchRings(1);
    CHECK(streamer.open(dir.c_str(), kDistance, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    u32 counter = 0;

    // Make the ground once, and let it finish.
    settle(streamer, renderer, 0, 0, budget, &counter, 60000);
    const MeshShot before = shoot(renderer, 0, 0, kDistance);
    CHECK(before.sections.size() > 0);

    // **Out of the renderer's range, and no further -- the distance is the
    // whole point of the test.**
    //
    // The streamer's grid is three rings wider than what it loads, which is
    // itself one ring wider than what is drawn, so there is a band where a
    // column has left the render distance and is still held in the grid. Going
    // far enough to leave the grid entirely drops the column and reloads it
    // from scratch, which is the easy path and the one that always worked.
    // This walks just past the field and back, one chunk at a time, the way a
    // player does.
    for (i32 x = 1; x <= kDistance + 3; ++x) {
        settle(streamer, renderer, x, 0, budget, &counter, 60000);
    }
    for (i32 x = kDistance + 2; x >= 0; --x) {
        settle(streamer, renderer, x, 0, budget, &counter, 60000);
    }
    const MeshShot after = shoot(renderer, 0, 0, kDistance);

    int missing = 0;
    int differing = 0;
    int firstBadX = 0;
    int firstBadY = -1;
    int firstBadZ = 0;
    long long firstWas = 0;
    long long firstNow = 0;
    for (const auto& entry : before.sections) {
        auto other = after.sections.find(entry.first);
        if (other == after.sections.end()) {
            ++missing;
            continue;
        }
        if (other->second != entry.second) {
            if (differing == 0) {
                firstBadX = std::get<0>(entry.first);
                firstBadY = std::get<1>(entry.first);
                firstBadZ = std::get<2>(entry.first);
                firstWas = entry.second;
                firstNow = other->second;
            }
            ++differing;
        }
    }

    CHECK_EQ(missing, 0);
    if (differing != 0) {
        // Reports which section and what happened to it: -1 never meshed,
        // -2 meshed to nothing, anything else a byte count.
        std::printf("  first divergence at chunk (%d, %d) section %d: was %lld, now %lld\n",
                    firstBadX, firstBadZ, firstBadY, firstWas, firstNow);
    }
    CHECK_EQ(differing, 0);

    streamer.close(kNow);
    renderer.shutdown();
}

// **Moving must not put the render thread on the SD card**, which is the whole
// of the reported symptom: flying around made the game freeze for a second or
// two at a time, with the storage page's main-thread figure climbing every time
// it happened.
//
// The mechanism was cell classification. A newly exposed cell asked whether the
// world had that chunk, the answer came from a directory listing that had not
// arrived yet, and the fallback was a `stat` -- on the frame, and behind the
// storage lock, which the I/O thread holds for the length of a chunk write. A
// whole row of cells is exposed per chunk-boundary crossing, and a flush is two
// hundred writes.
//
// So the assertion is a count, not a time: **zero storage operations on the
// calling thread, for the entire flight**. A time would measure this host; the
// count is the property. The camera crosses twenty-four chunk boundaries over
// ground that has never been walked, which is the case that used to stat for
// every cell of every crossing.
TEST(flying_over_new_ground_never_takes_the_render_thread_to_the_card)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 24680LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    constexpr int kDistance = 3;

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = kDistance;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    world::ChunkCache::Config cache;
    // The console's configuration: the I/O thread is what makes a deferred
    // answer possible at all, and it is the configuration the report came from.
    cache.threaded = true;
    streamer.setCacheConfig(cache);
    streamer.setPrefetchRings(2);
    CHECK(streamer.open(dir.c_str(), kDistance, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    u32 counter = 0;
    for (i32 x = 1; x <= 24; ++x) {
        for (int i = 0; i < 3; ++i) {
            frame(streamer, renderer, counter++, x, 0, budget);
        }
    }

    const world::ChunkCache::Stats io = streamer.stats().io;

    // Closed before anything is asserted, because a CHECK returns from the test
    // and a streamer that is never closed takes its worker thread down with the
    // process. The counters are the last frame's and survive the close.
    streamer.close(kNow);
    renderer.shutdown();

    // Both halves of the same fact, because either alone can mislead: a count
    // of zero with a non-zero time would mean something else reached the card,
    // and a time of zero with a non-zero count would mean a host too fast to
    // measure. The console's own debug page reads these two numbers.
    CHECK_EQ(int(io.stats), 0);
    CHECK_EQ(io.mainThreadMicros, 0);

    // The flight really did cross into unwalked ground, or the assertion above
    // is about a world that was already in hand.
    CHECK(io.listings > 0);
}

// **A tick edit must survive the column leaving the render distance.**
//
// `dropCell` hands a departing column back to the cache with `give()`, which
// installs what it is handed as *clean* -- and once the cell is gone,
// `flushTickDirty` cannot find it, because it looks the column up in the grid.
// So a column a tick wrote into and the player then walked away from was never
// written to the card, and with autosave set to Off that was every edit of the
// session. The column then sat in the cache, clean, as the authoritative answer
// for that chunk, so the edit was not merely unsaved: it was gone.
TEST(a_tick_edit_survives_the_column_leaving_the_grid)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 90210LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    constexpr int kDistance = 2;

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = kDistance;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    world::ChunkCache::Config cache;
    cache.threaded = true;
    streamer.setCacheConfig(cache);
    // Autosave off, which is the configuration the loss was total in.
    streamer.setAutosaveSeconds(0);
    CHECK(streamer.open(dir.c_str(), kDistance, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    budget.generatedPerFrame = 2;
    budget.meshesPerFrame = 8;
    u32 counter = 1;

    // Negative coordinates, per CONTRIBUTING: chunk -1 is where block -1 lives.
    constexpr i32 kEditChunkX = -3;
    constexpr i32 kEditChunkZ = -2;
    constexpr i32 kEditX = kEditChunkX * 16 + 5;
    constexpr i32 kEditZ = kEditChunkZ * 16 + 9;
    constexpr int kEditY = 70;

    settle(streamer, renderer, kEditChunkX, kEditChunkZ, budget, &counter, 900);

    tick::TickWorld* world = streamer.worldTick();
    CHECK(world != nullptr);

    // Write through the tick, so the edit takes exactly the path a fluid does.
    //
    // **Something the generator did not already put there.** `writeBlock`
    // returns true and notifies nobody when the block is already what is being
    // written, so a hard-coded id makes the test depend on what the terrain
    // happens to be at that spot.
    const block::BlockId before = world->blockAt(kEditX, kEditY, kEditZ);
    const block::BlockId placed = before == block::BlockId(mcver::Block::Stone)
                                      ? block::BlockId(mcver::Block::Cobblestone)
                                      : block::BlockId(mcver::Block::Stone);
    CHECK(world->setBlockWithNotify(kEditX, kEditY, kEditZ, placed));
    CHECK_EQ(world->blockAt(kEditX, kEditY, kEditZ), placed);
    CHECK(streamer.tickDirtyColumns() > 0);

    // Walk far enough away that the edited column leaves the grid entirely.
    settle(streamer, renderer, kEditChunkX + 12, kEditChunkZ + 12, budget, &counter, 900);
    CHECK(!renderer.field().isLoaded(kEditChunkX, kEditChunkZ));

    // And walk back. The block has to still be there -- from the cache or from
    // the card, it does not matter which, only that it was not dropped.
    settle(streamer, renderer, kEditChunkX, kEditChunkZ, budget, &counter, 900);
    CHECK_EQ(world->blockAt(kEditX, kEditY, kEditZ), placed);

    streamer.close(kNow);

    // Reopened from the card, with nothing left in memory: the strongest form
    // of the assertion, and the one autosave-off used to fail outright.
    WorldStreamer reopened;
    reopened.setGenerateMissing(true);
    reopened.setCacheConfig(cache);
    CHECK(reopened.open(dir.c_str(), kDistance, kNow));
    ChunkRenderer renderer2;
    renderer2.reset(&allocator, config);
    u32 counter2 = 1;
    settle(reopened, renderer2, kEditChunkX, kEditChunkZ, budget, &counter2, 900);
    CHECK(reopened.worldTick() != nullptr);
    CHECK_EQ(reopened.worldTick()->blockAt(kEditX, kEditY, kEditZ), placed);
    reopened.close(kNow);
}

// **A sign's text and a spawner's mob survive the world closing.**
//
// The end-to-end form of what `tests/tile_entity_test.cpp` checks a column at a
// time: the two session stores wired to the streamer's column sinks exactly as
// `src/platform/ctr/main.cpp` wires them, a write, a walk out of range, a
// close, and a reopen with nothing left in memory.
//
// **Autosave off, and that is the point.** The only thing that saves these is
// `dropCell` -- the column leaving the grid -- and the ordering there is what
// broke first: `dropped` used to fire before the save, so the store had already
// forgotten the column by the time the saving sink was asked what was in it.
TEST(sign_text_and_a_spawners_mob_survive_a_close_and_reopen)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 90210LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    constexpr int kDistance = 2;
    constexpr i32 kChunkX = -3;
    constexpr i32 kChunkZ = -2;
    constexpr i32 kSignX = kChunkX * 16 + 5;
    constexpr i32 kSignZ = kChunkZ * 16 + 9;
    constexpr i32 kCageX = kChunkX * 16 + 6;
    constexpr i32 kCageZ = kChunkZ * 16 + 9;
    constexpr int kY = 70;

    TestAllocator allocator;
    ChunkRendererConfig config;
    config.meshDistance = kDistance;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    world::ChunkCache::Config cache;
    cache.threaded = true;
    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    budget.generatedPerFrame = 2;
    budget.meshesPerFrame = 8;

    {
        ChunkRenderer renderer;
        renderer.reset(&allocator, config);
        WorldStreamer streamer;
        streamer.setGenerateMissing(true);
        streamer.setCacheConfig(cache);
        streamer.setAutosaveSeconds(0);
        CHECK(streamer.open(dir.c_str(), kDistance, kNow));

        TileStores stores;
        stores.bind(streamer);

        u32 counter = 1;
        settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);

        // Through `setBlock`, so `jt.e` runs and the spawner store is told --
        // the same road a Creative placement takes.
        CHECK(streamer.setBlock(renderer, kSignX, kY, kSignZ,
                                block::BlockId(mcver::Block::SignPost), 4));
        CHECK(streamer.setBlock(renderer, kCageX, kY, kCageZ,
                                block::BlockId(mcver::Block::MobSpawner), 0));

        // A sign block does not build its own tile entity -- the placement
        // does, because it has to be able to refuse the click. See
        // core/item/use.cpp.
        const int sign = stores.signs.put(kSignX, kY, kSignZ, false, 4);
        CHECK(sign >= 0);
        stores.signs.setLine(sign, 0, "first line");
        stores.signs.setLine(sign, 2, "third");
        // `ga.f()`: text is not a block, so nothing else marks the column.
        streamer.markColumnModified(kSignX, kSignZ);

        const int cage = stores.spawners.find(kCageX, kY, kCageZ);
        CHECK(cage >= 0);
        // What a Creative placement leaves: `bd`'s own default.
        CHECK_EQ(std::string(stores.spawners[cage].entityId), std::string("Pig"));
        stores.spawners.put(kCageX, kY, kCageZ, "Skeleton", 143);

        // Out of range, which with autosave off is the only thing that writes.
        settle(streamer, renderer, kChunkX + 12, kChunkZ + 12, budget, &counter, 900);
        CHECK(!renderer.field().isLoaded(kChunkX, kChunkZ));
        // ...and the stores let go of it as it went. **These two and not the
        // counts**: the ground the walk generated on the way is real world,
        // and a dungeon in it brings a cage of its own -- which is this
        // feature working, not a leak.
        CHECK_EQ(stores.signs.find(kSignX, kY, kSignZ), -1);
        CHECK_EQ(stores.spawners.find(kCageX, kY, kCageZ), -1);

        streamer.close(kNow);
        renderer.shutdown();
    }

    // Nothing left in memory: new streamer, new stores, reopened from the card.
    {
        ChunkRenderer renderer;
        renderer.reset(&allocator, config);
        WorldStreamer streamer;
        streamer.setGenerateMissing(true);
        streamer.setCacheConfig(cache);
        CHECK(streamer.open(dir.c_str(), kDistance, kNow));

        TileStores stores;
        stores.bind(streamer);

        u32 counter = 1;
        settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);

        const int sign = stores.signs.find(kSignX, kY, kSignZ);
        CHECK(sign >= 0);
        CHECK_EQ(std::string(stores.signs[sign].lines[0]), std::string("first line"));
        CHECK_EQ(std::string(stores.signs[sign].lines[1]), std::string());
        CHECK_EQ(std::string(stores.signs[sign].lines[2]), std::string("third"));
        // The board's shape comes off the block, not out of the NBT.
        CHECK(!stores.signs[sign].wall);
        CHECK_EQ(int(stores.signs[sign].metadata), 4);

        const int cage = stores.spawners.find(kCageX, kY, kCageZ);
        CHECK(cage >= 0);
        CHECK_EQ(std::string(stores.spawners[cage].entityId), std::string("Skeleton"));
        CHECK_EQ(stores.spawners[cage].delay, 143);

        streamer.close(kNow);
        renderer.shutdown();
    }
}

// **A player's edit has to redraw, and the obvious way to write it does not.**
//
// TickWorld's change callback only invalidates renderer sections while the
// streamer is holding a renderer, and only stepTicks was ever holding one. An
// edit made straight from the input handler therefore marked its column dirty
// for the save, queued its lighting, and left the section drawing its old
// geometry until something unrelated happened to touch it.
//
// This asserts both halves: the raw write does not mark the section, and
// WorldStreamer::setBlock does.
TEST(a_player_edit_marks_its_section_for_remesh_and_a_raw_tick_write_does_not)
{
    TestAllocator allocator;
    ChunkRendererConfig config;
    config.meshDistance = 2;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;

    ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 4242LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    streamer.setAutosaveSeconds(0);
    CHECK(streamer.open(dir.c_str(), 2, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    budget.generatedPerFrame = 2;
    budget.meshesPerFrame = 8;
    u32 counter = 1;

    // Negative on both axes, per CONTRIBUTING.
    constexpr i32 kChunkX = -2;
    constexpr i32 kChunkZ = -3;
    constexpr i32 kX = kChunkX * 16 + 7;
    constexpr i32 kZ = kChunkZ * 16 + 3;
    constexpr int kY = 68;
    constexpr int kSectionY = kY / 16;

    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);

    tick::TickWorld* world = streamer.worldTick();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }

    // Something the generator did not already put there, so the write is a
    // real change rather than a no-op.
    const block::BlockId before = world->blockAt(kX, kY, kZ);
    const block::BlockId first = before == block::BlockId(mcver::Block::Stone)
                                     ? block::BlockId(mcver::Block::Cobblestone)
                                     : block::BlockId(mcver::Block::Stone);

    // Clear whatever the settle left marked, so the assertions below are about
    // these two writes and nothing else.
    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);
    CHECK(!renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));

    // The raw path: the block changes and the column is dirty for the save,
    // but the section is not queued to be redrawn.
    CHECK(world->setBlockWithNotify(kX, kY, kZ, first));
    CHECK_EQ(world->blockAt(kX, kY, kZ), first);
    CHECK(streamer.tickDirtyColumns() > 0);
    CHECK(!renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));

    // The player's path, one block up so it is again a real change.
    const block::BlockId second = first == block::BlockId(mcver::Block::Stone)
                                      ? block::BlockId(mcver::Block::Cobblestone)
                                      : block::BlockId(mcver::Block::Stone);
    CHECK(streamer.setBlock(renderer, kX, kY + 1, kZ, second, 0));
    CHECK_EQ(world->blockAt(kX, kY + 1, kZ), second);
    CHECK(renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));

    // An edit outside the loaded grid is refused rather than written into a
    // column that is about to be somebody else's.
    CHECK(!streamer.setBlock(renderer, kX + 4000, kY, kZ + 4000, second, 0));

    streamer.close(kNow);
}

// **What an entity writes has to redraw too**, and the entity pools are not
// player edits: `moveEntity`'s tail tramples farmland, presses a plate and
// lands a falling block, and it runs straight out of the frame loop with
// `worldTick()` in hand rather than inside `stepTicks`. Reported as a trampled
// furrow whose wheat went on standing there: the world said dirt, the screen
// went on drawing the crop, because nothing had invalidated the section.
//
// `WorldStreamer::RenderBracket` is what the frame loop holds around those
// pools, and this is the pin on it -- the same two halves as the test above,
// through the door the entities use.
TEST(a_block_an_entity_writes_marks_its_section_only_inside_a_render_bracket)
{
    TestAllocator allocator;
    ChunkRendererConfig config;
    config.meshDistance = 2;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;

    ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 99LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    streamer.setAutosaveSeconds(0);
    CHECK(streamer.open(dir.c_str(), 2, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    budget.generatedPerFrame = 2;
    budget.meshesPerFrame = 8;
    u32 counter = 1;

    // Negative on both axes, per CONTRIBUTING.
    constexpr i32 kChunkX = -2;
    constexpr i32 kChunkZ = -3;
    constexpr i32 kX = kChunkX * 16 + 7;
    constexpr i32 kZ = kChunkZ * 16 + 3;
    constexpr int kY = 68;
    constexpr int kSectionY = kY / 16;

    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);

    tick::TickWorld* world = streamer.worldTick();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }

    // A field of one, with a crop standing on it, and the world settled so the
    // assertions below are about the footstep and nothing else.
    CHECK(streamer.setBlock(renderer, kX, kY, kZ, block::BlockId(mcver::Block::Farmland), 7));
    CHECK(streamer.setBlock(renderer, kX, kY + 1, kZ, block::BlockId(mcver::Block::Wheat), 7));
    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);
    CHECK(!renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));

    // Unbracketed, which is what the frame loop used to do: the trample lands
    // in the world and the screen never hears about it.
    for (int i = 0; i < 40; ++i) {
        if (world->blockAt(kX, kY, kZ) != block::BlockId(mcver::Block::Farmland)) break;
        tick::entityWalkedOnBlock(*world, kX, kY, kZ);
    }
    CHECK_EQ(world->blockAt(kX, kY, kZ), block::BlockId(mcver::Block::Dirt));
    // The crop goes with the ground under it, and it goes at once.
    CHECK_EQ(world->blockAt(kX, kY + 1, kZ), block::kAir);
    CHECK(!renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));

    // Bracketed, which is what it does now.
    CHECK(streamer.setBlock(renderer, kX, kY, kZ, block::BlockId(mcver::Block::Farmland), 7));
    CHECK(streamer.setBlock(renderer, kX, kY + 1, kZ, block::BlockId(mcver::Block::Wheat), 7));
    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);
    CHECK(!renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));
    {
        WorldStreamer::RenderBracket draws(streamer, renderer);
        for (int i = 0; i < 40; ++i) {
            if (world->blockAt(kX, kY, kZ) != block::BlockId(mcver::Block::Farmland)) break;
            tick::entityWalkedOnBlock(*world, kX, kY, kZ);
        }
    }
    CHECK_EQ(world->blockAt(kX, kY, kZ), block::BlockId(mcver::Block::Dirt));
    CHECK_EQ(world->blockAt(kX, kY + 1, kZ), block::kAir);
    CHECK(renderer.field().column(kChunkX, kChunkZ).dirty(kSectionY));

    streamer.close(kNow);
}

// **The map has to be told, and it is not a renderer.**
//
// `MapScreen` keeps a sample of every chunk it has drawn, including ground the
// streamer has handed back to the card, so the section invalidation above is no
// use to it twice over: it is bracketed by `tickRenderer_`, which a leaf
// decaying or a fluid spreading is not necessarily inside, and it says nothing
// about a chunk that is no longer in the grid at all. What it reads instead is
// a serial per column, off one session-wide counter, and this is the pin on the
// three things it promises.
TEST(a_block_change_moves_the_columns_serial_and_a_reload_does_not_reuse_one)
{
    TestAllocator allocator;
    ChunkRendererConfig config;
    config.meshDistance = 2;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;

    ChunkRenderer renderer;
    renderer.reset(&allocator, config);

    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 1337LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    streamer.setAutosaveSeconds(0);
    CHECK(streamer.open(dir.c_str(), 2, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    budget.generatedPerFrame = 2;
    budget.meshesPerFrame = 8;
    u32 counter = 1;

    // Negative on both axes, per CONTRIBUTING.
    constexpr i32 kChunkX = -1;
    constexpr i32 kChunkZ = -2;
    constexpr i32 kX = kChunkX * 16 + 5;
    constexpr i32 kZ = kChunkZ * 16 + 11;
    constexpr int kY = 70;

    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);

    tick::TickWorld* world = streamer.worldTick();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }

    // A resident column has a serial and one that is not held has none, which
    // is what lets a reader tell "unchanged" from "gone".
    const u32 before = streamer.columnBlockSerial(kChunkX, kChunkZ);
    CHECK(before != 0u);
    CHECK_EQ(streamer.columnBlockSerial(kChunkX + 400, kChunkZ + 400), 0u);

    // Standing still writes nothing, so nothing moves. This is the frame the
    // map is allowed to skip its per-chunk pass on.
    //
    // **Looked for rather than demanded of the next frame.** A frame ticks the
    // world, and a freshly generated one still owes scheduled updates -- a
    // fluid settling, a leaf decaying -- plus whatever its random ticks land
    // on. How many frames `settle` above took is decided by a worker thread,
    // so which tick those updates fall on is not fixed, and insisting that one
    // named frame be quiet made this case fail or pass on how busy the machine
    // was. The claim being tested is that a frame *can* write nothing while the
    // camera holds still, and that when one does the column's own serial holds
    // still with it; both survive being asked for patiently.
    u32 worldBefore = streamer.blockChangeSerial();
    u32 columnBefore = before;
    bool quietFrame = false;
    for (int attempt = 0; attempt < 200 && !quietFrame; ++attempt) {
        frame(streamer, renderer, counter++, kChunkX, kChunkZ, budget);
        quietFrame = streamer.blockChangeSerial() == worldBefore;
        if (quietFrame) {
            // The pair of it: a frame that wrote nothing left this column's
            // own serial alone as well.
            CHECK_EQ(streamer.columnBlockSerial(kChunkX, kChunkZ), columnBefore);
        }
        worldBefore = streamer.blockChangeSerial();
        columnBefore = streamer.columnBlockSerial(kChunkX, kChunkZ);
    }
    CHECK(quietFrame);

    // **The raw tick write**, which is what a decaying leaf or a spreading
    // fluid is: no renderer in hand, and the map must still hear about it.
    const block::BlockId was = world->blockAt(kX, kY, kZ);
    const block::BlockId now = was == block::BlockId(mcver::Block::Stone)
                                   ? block::BlockId(mcver::Block::Cobblestone)
                                   : block::BlockId(mcver::Block::Stone);
    CHECK(world->setBlockWithNotify(kX, kY, kZ, now));

    const u32 afterTick = streamer.columnBlockSerial(kChunkX, kChunkZ);
    CHECK(afterTick != before);
    CHECK(streamer.blockChangeSerial() > worldBefore);

    // ...and the player's own edit, one block up.
    CHECK(streamer.setBlock(renderer, kX, kY + 1, kZ, now, 0));
    CHECK(streamer.columnBlockSerial(kChunkX, kChunkZ) != afterTick);

    // A chunk nothing was written into keeps the serial it was adopted with:
    // the counter is the world's, but a column only takes a new value when
    // something happens to *it*.
    const u32 neighbour = streamer.columnBlockSerial(kChunkX + 1, kChunkZ);
    CHECK(neighbour != 0u);
    CHECK(world->setBlockWithNotify(kX, kY + 2, kZ, now));
    CHECK_EQ(streamer.columnBlockSerial(kChunkX + 1, kChunkZ), neighbour);

    // **A column that leaves the grid and comes back is never handed a serial
    // it has already used.** Without this a sample taken before the column left
    // would compare equal to the column that came back, and the map would go on
    // drawing a house that had since been demolished.
    const u32 leaving = streamer.columnBlockSerial(kChunkX, kChunkZ);
    for (i32 x = 0; x < 30; ++x) {
        frame(streamer, renderer, counter++, kChunkX + x, kChunkZ, budget);
    }
    CHECK_EQ(streamer.columnBlockSerial(kChunkX, kChunkZ), 0u);
    settle(streamer, renderer, kChunkX, kChunkZ, budget, &counter, 900);
    const u32 returned = streamer.columnBlockSerial(kChunkX, kChunkZ);
    CHECK(returned != 0u);
    CHECK(returned != leaving);

    streamer.close(kNow);
}


// **A world reopened in the chunk it was left in has to draw.**
//
// Reported from hardware: joining a world sometimes showed the entities and no
// blocks at all, until the player walked across a chunk boundary. The console
// keeps one `WorldStreamer` for the process and builds a new renderer for each
// world, and `close()` left the old centre behind -- so the first `update()` of
// the next session saw the camera in the chunk the centre already named, skipped
// the branch that tells the renderer where its field is, and every column was
// refused as out of range of a field still centred on (0, 0). Near the origin
// that happens to overlap and hides the bug, which is why the chunk is far out.
TEST(a_world_reopened_in_the_chunk_it_was_left_in_draws)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("World");
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 777LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    constexpr int kDistance = 3;
    constexpr i32 kChunkX = 40;
    constexpr i32 kChunkZ = -37;

    TestAllocator allocator;
    ChunkRendererConfig config;
    config.meshDistance = kDistance;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 4;
    world::ChunkCache::Config cache;
    cache.threaded = true;
    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 2;
    budget.generatedPerFrame = 2;
    budget.meshesPerFrame = 4;

    // One streamer for every session, as on the console; a renderer per world.
    static WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    streamer.setCacheConfig(cache);
    for (int session = 0; session < 2; ++session) {
        ChunkRenderer renderer;
        renderer.reset(&allocator, config);
        CHECK(streamer.open(dir.c_str(), kDistance, kNow));

        u32 counter = 1;
        bool drew = false;
        for (int f = 0; f < 3000 && !drew; ++f) {
            frame(streamer, renderer, counter++, kChunkX, kChunkZ, budget);
            drew = !renderer.drawList().empty();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(drew);
        CHECK(renderer.field().isLoaded(kChunkX, kChunkZ));

        streamer.close(kNow);
        renderer.shutdown();
    }
}
