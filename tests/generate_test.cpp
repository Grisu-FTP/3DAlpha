#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/world/chunk.hpp"
#include "generate_vectors.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_generator.hpp"

#include <memory>
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
