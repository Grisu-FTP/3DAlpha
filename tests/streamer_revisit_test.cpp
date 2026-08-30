#include "framework.hpp"

#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/frustum.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
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
