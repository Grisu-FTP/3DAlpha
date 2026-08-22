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
