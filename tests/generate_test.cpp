#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/world/chunk.hpp"
#include "core/world/tile_entity.hpp"
#include "generate_vectors.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_generator.hpp"

#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace mc;
using mc::test::kGenerateCaseCount;
using mc::test::kGenerateCases;
using worldgen::ChunkGenerator;

namespace {

constexpr int kColumnBlocks = 32768;

std::vector<u8> expand(const u32* runs, int runCount)
{
    std::vector<u8> out;
    out.reserve(usize(kColumnBlocks));
    for (int i = 0; i < runCount; ++i) {
        const u32 run = runs[i];
        const u8 value = u8(run & 0xFF);
        for (u32 k = run >> 8; k > 0; --k) {
            out.push_back(value);
        }
    }
    return out;
}

// The fixture's index, x << 11 | z << 7 | y, taken apart.
struct Cell {
    int x, y, z;
};

Cell cellOf(int index) { return Cell{index >> 11, index & 127, (index >> 7) & 15}; }

}  // namespace

// **The check that no stage below it can make.**
//
// Terrain, caves, every population generator, the pass driver and the light
// engine each have an oracle already, and all of them pass. What none of them
// covers is `ft` -- which chunk gets generated, and *when* it gets populated.
// Population reads the world it writes into, so two neighbouring chunks that
// reach the same blocks give different answers depending on which ran first,
// and that order is decided here and nowhere else.
//
// The fixture is a real a1.1.2 World asked for chunks in exactly the order
// ChunkGenerator::provide sweeps them. It is deliberately not "the world for
// seed S": an Alpha seed does not determine one, because population order
// follows chunk residency and residency follows the player. Same rule, same
// sequence, same world -- that is the claim, and this is what pins it.
TEST(the_chunk_generator_matches_a_real_world)
{
    for (int c = 0; c < kGenerateCaseCount; ++c) {
        const auto& want = kGenerateCases[c];

        worldgen::GeneratorOptions options;
        options.snowCovered = want.snowCovered;

        // No existing chunks: every column of every case is generated here.
        ChunkGenerator::Store store;
        auto generator = std::make_unique<ChunkGenerator>(want.seed, options, store);
        auto column = std::make_unique<world::ChunkColumn>();

        for (int ix = 0; ix < 2; ++ix) {
            for (int iz = 0; iz < 2; ++iz) {
                const int k = ix * 2 + iz;
                CHECK(generator->provide(want.chunkX + ix, want.chunkZ + iz, column.get()));

                const std::vector<u8> blocks = expand(want.blocks[k], want.blockRuns[k]);
                const std::vector<u8> sky = expand(want.sky[k], want.skyRuns[k]);
                const std::vector<u8> light = expand(want.blockLight[k], want.blockLightRuns[k]);
                CHECK_EQ(int(blocks.size()), kColumnBlocks);
                CHECK_EQ(int(sky.size()), kColumnBlocks);
                CHECK_EQ(int(light.size()), kColumnBlocks);

                // Blocks first: a mismatch here makes every light comparison
                // below meaningless, so it is reported on its own terms.
                int badBlock = -1;
                for (int i = 0; i < kColumnBlocks; ++i) {
                    const Cell cell = cellOf(i);
                    if (int(column->block(cell.x, cell.y, cell.z)) != int(blocks[usize(i)])) {
                        badBlock = i;
                        break;
                    }
                }
                if (badBlock >= 0) {
                    const Cell cell = cellOf(badBlock);
                    CHECK_EQ(int(column->block(cell.x, cell.y, cell.z)),
                             int(blocks[usize(badBlock)]));
                    CHECK_EQ(cell.x, -1);  // reports where
                    CHECK_EQ(cell.y, -1);
                    CHECK_EQ(cell.z, -1);
                    CHECK_EQ(k, -1);  // and which of the four columns
                    continue;
                }

                for (int i = 0; i < kColumnBlocks; ++i) {
                    const Cell cell = cellOf(i);
                    if (int(column->skyLight(cell.x, cell.y, cell.z)) != int(sky[usize(i)])) {
                        CHECK_EQ(int(column->skyLight(cell.x, cell.y, cell.z)),
                                 int(sky[usize(i)]));
                        CHECK_EQ(cell.y, -1);
                        CHECK_EQ(int(blocks[usize(i)]), -1);
                        break;
                    }
                }

                // **Block light is exact everywhere the renderer can see, and
                // only there.** The one divergence this port has is the value
                // stored *inside* a fully light-blocking block, which the
                // original never schedules and no cell can receive from; the
                // split is characterised in core/world/lighting.hpp and pinned
                // the same way in tests/light_test.cpp.
                int opaqueDifferences = 0;
                for (int i = 0; i < kColumnBlocks; ++i) {
                    const Cell cell = cellOf(i);
                    const block::BlockDef& def = block::def(blocks[usize(i)]);
                    const int got = int(column->blockLight(cell.x, cell.y, cell.z));
                    const int expected = int(light[usize(i)]);

                    if (!(def.known && def.opacity >= 15)) {
                        if (got != expected) {
                            CHECK_EQ(got, expected);
                            CHECK_EQ(cell.y, -1);
                            CHECK_EQ(int(blocks[usize(i)]), -1);
                            break;
                        }
                        continue;
                    }
                    CHECK(got >= expected);
                    if (got != expected) {
                        ++opaqueDifferences;
                        CHECK(def.light > 0);
                    }
                }
                CHECK(opaqueDifferences < 200);

                for (int i = 0; i < 256; ++i) {
                    if (int(column->heightMap[i]) != int(want.heightMap[k][i])) {
                        CHECK_EQ(int(column->heightMap[i]), int(want.heightMap[k][i]));
                        CHECK_EQ(i, -1);
                        break;
                    }
                }
            }
        }

        const ChunkGenerator::Stats& stats = generator->stats();

        // The three counters that must never move. Each is a distinct way for
        // the design in chunk_generator.hpp to be wrong, and each is silent.
        CHECK_EQ(int(stats.refusedOutOfWindow), 0);
        CHECK_EQ(int(stats.populationEscapes), 0);
        CHECK_EQ(int(stats.evictedLive), 0);

        CHECK_EQ(int(stats.lit), 4);
    }
}

// **The seam between a world made elsewhere and the ground beyond it, which is
// where generation used to stop for good.**
//
// A column that comes out of the save with `terrainPopulated` set is one whose
// population pass has already run, and `ft` skips it on exactly that basis. But
// that pass writes into a 2x2, and we -- unlike the original -- have to know
// when a column has stopped changing before we can light it. So the four
// columns the skipped pass reaches still have to record it as done.
//
// While they did not, the first generated column east of a stored one waited
// for a pass that would never run again: `provide` failed `lightable` for it
// and for everything behind it, every frame, for the rest of the session. On
// hardware that is generation stopping and the world going undrawn -- a
// streamer will not publish a column whose neighbour is still `Ungenerated`, so
// the unlit frontier walks back inward.
//
// The store here is half a world: everything at x <= 0 is stored and populated,
// everything east of it has to be made. Asking for (2, 0) sweeps x from -1 to
// 4, so the seam sits inside one sweep.
TEST(generation_continues_past_the_edge_of_a_world_made_elsewhere)
{
    struct Half {
        std::unique_ptr<world::ChunkColumn> scratch;
    };
    Half half;

    ChunkGenerator::Store store;
    store.context = &half;
    store.load = [](void* context, i32 chunkX, i32 chunkZ,
                    world::ChunkColumn* scratch) -> const world::ChunkColumn* {
        (void)context;
        if (chunkX > 0) {
            return nullptr;  // never made; this is what the generator is for
        }
        // Stored, and through the whole pipeline once already. The blocks do
        // not matter here -- what is being tested is the bookkeeping around
        // them -- so bare stone is enough to be a column that is not air.
        *scratch = world::ChunkColumn(chunkX, chunkZ);
        for (int sy = 0; sy < 4; ++sy) {
            world::Section& section = scratch->section(sy);
            for (int i = 0; i < world::Section::kVolume; ++i) {
                section.setBlock(i, world::BlockId(1));
            }
        }
        scratch->terrainPopulated = true;
        return scratch;
    };

    worldgen::GeneratorOptions options;
    auto generator = std::make_unique<ChunkGenerator>(1234567890LL, options, store);
    auto column = std::make_unique<world::ChunkColumn>();

    // Every column the far side of the seam, out to where a sweep no longer
    // touches a stored one at all. Each of these failed before.
    for (i32 x = 1; x <= 6; ++x) {
        for (i32 z = -1; z <= 1; ++z) {
            CHECK(generator->provide(x, z, column.get()));
            CHECK(column->terrainPopulated);
        }
    }

    const ChunkGenerator::Stats& stats = generator->stats();
    CHECK_EQ(int(stats.refusedOutOfWindow), 0);
    CHECK_EQ(int(stats.populationEscapes), 0);
    CHECK_EQ(int(stats.evictedLive), 0);
    CHECK_EQ(int(stats.lit), 18);
}

namespace {

// A world in a map: everything the generator hands over is kept, and load()
// answers from it. This is what the streamer's ChunkCache is to the generator,
// with none of the threading.
struct MemoryWorld {
    std::map<i64, world::ChunkColumn> columns;

    static i64 key(i32 x, i32 z) { return i64((u64(u32(x)) << 32) | u64(u32(z))); }

    static const world::ChunkColumn* load(void* context, i32 chunkX, i32 chunkZ,
                                          world::ChunkColumn* scratch)
    {
        (void)scratch;
        auto* self = static_cast<MemoryWorld*>(context);
        auto it = self->columns.find(key(chunkX, chunkZ));
        return it == self->columns.end() ? nullptr : &it->second;
    }

    static void deliver(void* context, world::ChunkColumn& column)
    {
        auto* self = static_cast<MemoryWorld*>(context);
        self->columns.insert_or_assign(key(column.x, column.z), column.clone());
    }

    ChunkGenerator::Store store()
    {
        ChunkGenerator::Store s;
        s.context = this;
        s.load = &MemoryWorld::load;
        s.deliver = &MemoryWorld::deliver;
        return s;
    }
};

// A straight line of columns, which is the shape of a player walking. Returns
// every column the walk produced, keyed the same way, so two walks can be
// compared block for block.
std::map<i64, world::ChunkColumn> walk(int cacheColumns, int retireRadius, u32* evictedLive,
                                       u32* retiredLive, u32* peakLive, u32* failures, u32* stuck)
{
    constexpr i32 kSteps = 40;

    MemoryWorld world;
    worldgen::GeneratorOptions options;
    ChunkGenerator::Store store = world.store();
    auto generator = std::make_unique<ChunkGenerator>(1234567890LL, options, store, cacheColumns);
    auto column = std::make_unique<world::ChunkColumn>();

    for (i32 x = 0; x < kSteps; ++x) {
        if (retireRadius > 0) {
            generator->retire(x, 0, retireRadius);
        }
        // **Retried, because that is what the streamer does.** A failed sweep
        // leaves the column on the slate and it is asked for again next frame.
        // What must not happen is a column that fails for ever; a handful of
        // retries in a cache this small is the last resort in acquire() having
        // just dropped a region the sweep was standing on, and the region
        // re-derives on the way back through.
        //
        // Counted rather than asserted: this runs outside a TEST, where the
        // framework's CHECK cannot return.
        bool made = false;
        for (int attempt = 0; attempt < 4 && !made; ++attempt) {
            made = generator->provide(x, 0, column.get());
            if (!made) {
                ++*failures;
            }
        }
        if (!made) {
            ++*stuck;
            continue;
        }
        world.columns.insert_or_assign(MemoryWorld::key(x, 0), column->clone());
    }

    *evictedLive = generator->stats().evictedLive;
    *retiredLive = generator->stats().retiredLive;
    *peakLive = generator->stats().peakLive;
    return std::move(world.columns);
}

constexpr int kRoomyCache = 512;
constexpr int kTightCache = 64;

}  // namespace

// **Walking away from ground must not corrupt it, and not walking away from it
// must give the same world.**
//
// A column the generator has not handed over cannot be evicted -- its
// neighbours' populations are written into it and nowhere else -- and nothing
// ever finishes the ones a moving centre leaves at the sides of the corridor it
// sweeps. That set grows with the distance walked: measured under `--fly` at
// render distance 8, peak live went 235, 274, 372, 468 as the walk went 75,
// 200, 400 and 600 chunks. When it fills the cache, acquire() takes a live
// column anyway, and that column comes back as bare terrain -- missing the
// trees and the snow its neighbours put in it, and never final again, so
// `lightable` fails for it and its neighbours for the rest of the session.
//
// Every symptom of that was reported from hardware together: trees generated
// only down one side of a chunk border, whole chunks with no snow in a snow
// world, and generation stopping until the world was reloaded.
//
// retire() is what drains it, and the claim being pinned here is that draining
// costs nothing: a forward walk never asks for a retired column again, so the
// world it produces has to be the same one, block for block, as a walk that
// kept everything.
TEST(retiring_ground_behind_a_walk_leaves_the_same_world)
{
    u32 keptEvicted = 0;
    u32 keptRetired = 0;
    u32 keptPeak = 0;
    u32 keptFailures = 0;
    u32 keptStuck = 0;
    // Room for the whole walk, so nothing is ever retired or evicted and this
    // is the world the generator would make if memory were free.
    const std::map<i64, world::ChunkColumn> kept =
        walk(kRoomyCache, 0, &keptEvicted, &keptRetired, &keptPeak, &keptFailures, &keptStuck);

    u32 retiredEvicted = 0;
    u32 retiredRetired = 0;
    u32 retiredPeak = 0;
    u32 retiredFailures = 0;
    u32 retiredStuck = 0;
    const std::map<i64, world::ChunkColumn> retired =
        walk(kTightCache, 6, &retiredEvicted, &retiredRetired, &retiredPeak, &retiredFailures,
             &retiredStuck);

    CHECK_EQ(int(keptEvicted), 0);
    CHECK_EQ(int(keptFailures), 0);
    CHECK_EQ(int(keptStuck), 0);

    // The thing that goes wrong without it, and does not with it: the same walk
    // in the same cache with no retirement is the test below.
    CHECK_EQ(int(retiredFailures), 0);
    CHECK_EQ(int(retiredStuck), 0);
    CHECK_EQ(int(retiredEvicted), 0);
    CHECK(retiredRetired > 0);
    CHECK(retiredPeak < keptPeak);

    // ...and the world is the same one. Every column either walk delivered, on
    // every block, both light planes and the height map.
    CHECK_EQ(int(retired.size()), int(kept.size()));
    for (const auto& entry : kept) {
        auto it = retired.find(entry.first);
        CHECK(it != retired.end());
        if (it == retired.end()) {
            continue;
        }
        const world::ChunkColumn& a = entry.second;
        const world::ChunkColumn& b = it->second;
        CHECK_EQ(int(b.x), int(a.x));
        CHECK_EQ(int(b.z), int(a.z));
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                CHECK_EQ(int(b.heightMap[x * 16 + z]), int(a.heightMap[x * 16 + z]));
                for (int y = 0; y < 128; ++y) {
                    if (a.block(x, y, z) != b.block(x, y, z)
                        || a.skyLight(x, y, z) != b.skyLight(x, y, z)
                        || a.blockLight(x, y, z) != b.blockLight(x, y, z)) {
                        CHECK_EQ(int(b.block(x, y, z)), int(a.block(x, y, z)));
                        CHECK_EQ(int(b.skyLight(x, y, z)), int(a.skyLight(x, y, z)));
                        CHECK_EQ(int(b.blockLight(x, y, z)), int(a.blockLight(x, y, z)));
                        CHECK_EQ(a.x, -1);  // reports where
                        CHECK_EQ(x, -1);
                        CHECK_EQ(y, -1);
                        CHECK_EQ(z, -1);
                        return;
                    }
                }
            }
        }
    }
}

// **A caller that never retires must not corrupt the world either.**
//
// The streamer retires against the player's position, but the generator cannot
// depend on being told: a caller that does not -- the tests here, a probe, a
// future one -- would otherwise fill the cache with live columns, and acquire()
// would take one that its neighbours were still writing into. That column comes
// back as bare terrain missing their trees and their snow, and it is never final
// again, so every sweep that needs it fails `lightable` for the rest of the
// session. On hardware all of that arrived together: half trees, chunks with no
// snow in a snow world, and generation stopping until a reload.
//
// So acquire() has a last resort of its own: when nothing has been delivered
// that it could take, it lets go of everything outside the sweep in progress --
// a region, not a column, which is what makes it safe. The same forty-column
// walk in the same small cache with retire() never called therefore still
// finishes, and `evictedLive` -- the counter for the thing that must never
// happen -- stays at zero.
TEST(a_walk_that_never_retires_heals_itself)
{
    u32 evictedLive = 0;
    u32 retiredLive = 0;
    u32 peakLive = 0;
    u32 failures = 0;
    u32 stuck = 0;
    walk(kTightCache, 0, &evictedLive, &retiredLive, &peakLive, &failures, &stuck);

    // The thing that must never happen, and the thing that must never happen
    // twice: no column taken while its neighbours were still writing into it,
    // and no column that could not be made at all.
    CHECK_EQ(int(evictedLive), 0);
    CHECK_EQ(int(stuck), 0);
    // ...and it got there by retiring, which is the path being exercised.
    CHECK(retiredLive > 0);
}

// retire() frees by distance and by nothing else, and it frees delivered and
// live columns alike -- a region goes as a unit, which is the property that
// makes dropping a live column safe here and unsafe in acquire().
TEST(retire_frees_exactly_what_is_out_of_range)
{
    MemoryWorld world;
    worldgen::GeneratorOptions options;
    ChunkGenerator::Store store = world.store();
    auto generator = std::make_unique<ChunkGenerator>(1234567890LL, options, store, 160);
    auto column = std::make_unique<world::ChunkColumn>();

    CHECK(generator->provide(0, 0, column.get()));
    // The sweep is (cx-3..cx+2) in both axes, so a radius of 3 covers all of it
    // and there is nothing to let go of.
    CHECK_EQ(int(generator->retire(0, 0, 3)), 0);

    // A radius of 1 keeps a 3x3 and drops the rest of the 6x6.
    const u32 freed = generator->retire(0, 0, 1);
    CHECK_EQ(int(freed), 36 - 9);
    CHECK(generator->liveColumns() <= 9);

    // And the columns are gone rather than merely unreachable: asking again
    // reads them back or makes them again, and either way succeeds.
    CHECK(generator->provide(0, 0, column.get()));
    CHECK_EQ(int(generator->stats().evictedLive), 0);
}

// **A dungeon's mob and loot reach the column the generator hands over.**
//
// `PopulationSideEffects` used to be passed as null on the generation worker
// with a comment saying its records had no consumer, so a dungeon this build
// made came back as a mossy room around an empty cage. The records go onto the
// generator's `Entry` now -- not onto a column, because a pass writes into a
// 2x2 quadrant and a dungeon rolled for one chunk routinely lands in the next.
//
// It sweeps rather than naming a coordinate: eight tries a chunk and nearly
// all of them refused, so which chunk holds the first dungeon is a fact about
// the seed and not worth pinning. What is pinned is that every spawner carries
// one of `cg.b`'s three names and that no cage in the world is empty.
TEST(a_generated_dungeon_carries_its_mob_and_its_loot)
{
    MemoryWorld world;
    worldgen::GeneratorOptions options;
    ChunkGenerator::Store store = world.store();
    auto generator = std::make_unique<ChunkGenerator>(1234567890LL, options, store, 512);
    auto column = std::make_unique<world::ChunkColumn>();

    int spawners = 0;
    int chests = 0;
    int stacks = 0;

    for (i32 x = 0; x < 12; ++x) {
        for (i32 z = 0; z < 12; ++z) {
            if (!generator->provide(x, z, column.get())) {
                continue;
            }
            for (const world::TileEntity& tile : column->tileEntities) {
                // Every entry belongs to the column carrying it.
                CHECK_EQ(tile.x >> 4, column->x);
                CHECK_EQ(tile.z >> 4, column->z);
                // ...and stands on the block it is the tile entity of.
                const world::BlockId id =
                    column->block(int(tile.x & 15), tile.y, int(tile.z & 15));
                world::TileEntityKind kind = world::TileEntityKind::Unknown;
                CHECK(world::tileEntityKindForBlock(block::def(id).tick, &kind));
                CHECK_EQ(int(kind), int(tile.kind));

                if (tile.kind == world::TileEntityKind::MobSpawner) {
                    ++spawners;
                    // `cg.b(Random)`: Skeleton, Zombie (twice) or Spider, and
                    // never `bd`'s own default.
                    CHECK(tile.entityId == "Skeleton" || tile.entityId == "Zombie" ||
                          tile.entityId == "Spider");
                } else if (tile.kind == world::TileEntityKind::Chest) {
                    ++chests;
                    stacks += int(tile.items.size());
                    for (const item::ItemStack& item : tile.items) {
                        CHECK(item.slot >= 0 && item.slot < world::kChestSlots);
                        CHECK(item.count > 0);
                    }
                }
            }
        }
    }

    // 144 chunks is far more than enough for a1.1.2's dungeon rate.
    CHECK(spawners > 0);
    CHECK(chests > 0);
    CHECK(stacks > 0);
}
