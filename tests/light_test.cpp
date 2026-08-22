#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/world/lighting.hpp"
#include "light_vectors.hpp"

#include <vector>

using namespace mc;
using mc::test::kLightCaseCount;
using mc::test::kLightCases;
using world::kColumnBlocks;
using world::LightEngine;

namespace {

std::vector<u8> expand(const u32* runs, int runCount, int expected)
{
    std::vector<u8> out;
    out.reserve(usize(expected));
    for (int i = 0; i < runCount; ++i) {
        const u32 run = runs[i];
        const u8 value = u8(run & 0xFF);
        for (u32 k = run >> 8; k > 0; --k) {
            out.push_back(value);
        }
    }
    return out;
}

u8 nibbleAt(const u8* packed, int index)
{
    const u8 byte = packed[usize(index >> 1)];
    return (index & 1) == 0 ? u8(byte & 0x0F) : u8(byte >> 4);
}

// A light failure is one nibble in 32,768, so report where it is in a form that
// can be looked at -- and report the *block* there too, because "wrong light on
// a stone block" and "wrong light in open air" are different bugs.
struct Mismatch {
    int index = -1;
    int x = 0, y = 0, z = 0;
    int got = 0, want = 0, blockId = 0;
};

Mismatch firstMismatch(const u8* packed, const std::vector<u8>& want, const std::vector<u8>& blocks)
{
    Mismatch bad;
    for (int i = 0; i < kColumnBlocks; ++i) {
        const u8 got = nibbleAt(packed, i);
        if (got != want[usize(i)]) {
            bad.index = i;
            bad.x = i >> 11;
            bad.z = (i >> 7) & 15;
            bad.y = i & 127;
            bad.got = got;
            bad.want = want[usize(i)];
            bad.blockId = blocks[usize(i)];
            return bad;
        }
    }
    return bad;
}

}  // namespace

// The whole point of the engine: our single-pass bucketed search has to land on
// exactly the same numbers as the original's box queue, once that queue has
// been drained to a standstill. Every one of 32,768 sky values and 32,768 block
// values, per case.
TEST(lighting_matches_a_fully_settled_jar_world)
{
    LightEngine engine;

    for (int c = 0; c < kLightCaseCount; ++c) {
        const auto& want = kLightCases[c];

        std::vector<std::vector<u8>> storage;
        std::vector<const u8*> columns;
        for (int i = 0; i < 9; ++i) {
            storage.push_back(expand(want.blocks[i], want.blockRuns[i], kColumnBlocks));
            CHECK_EQ(int(storage.back().size()), kColumnBlocks);
        }
        for (const auto& column : storage) {
            columns.push_back(column.data());
        }

        std::vector<u8> sky(16384, 0);
        std::vector<u8> blockLight(16384, 0);
        std::vector<u8> heightMap(256, 0);
        engine.computeCentre(columns.data(), sky.data(), blockLight.data(), heightMap.data());

        const std::vector<u8> wantSky = expand(want.sky, want.skyRuns, kColumnBlocks);
        const std::vector<u8> wantBlock =
            expand(want.blockLight, want.blockLightRuns, kColumnBlocks);
        CHECK_EQ(int(wantSky.size()), kColumnBlocks);
        CHECK_EQ(int(wantBlock.size()), kColumnBlocks);

        const std::vector<u8>& centre = storage[4];

        const Mismatch badSky = firstMismatch(sky.data(), wantSky, centre);
        if (badSky.index >= 0) {
            CHECK_EQ(badSky.got, badSky.want);
            CHECK_EQ(badSky.index, -1);
            CHECK_EQ(badSky.y, -1);        // reports the height
            CHECK_EQ(badSky.blockId, -1);  // and what is standing there
        }

        // **Block light is exact everywhere the renderer can see, and only
        // there.** The one place this port does not reproduce a1.1.2 byte for
        // byte is the light value stored *inside* fully light-blocking blocks,
        // and the reason is in the original's own machinery rather than in
        // ours -- see the note in core/world/lighting.hpp.
        //
        // The split is checked rather than assumed: at every cell that can
        // hold or pass light, the two agree exactly; at a cell that cannot,
        // ours is never darker and the block there is always an emitter.
        int opaqueDifferences = 0;
        for (int i = 0; i < kColumnBlocks; ++i) {
            const u8 id = centre[usize(i)];
            const block::BlockDef& def = block::def(id);
            const int got = nibbleAt(blockLight.data(), i);
            const int expected = wantBlock[usize(i)];
            const bool blocksLight = def.known && def.opacity >= 15;

            if (!blocksLight) {
                if (got != expected) {
                    // Reports the position and the block, like the sky case.
                    CHECK_EQ(got, expected);
                    CHECK_EQ(i & 127, -1);
                    CHECK_EQ(int(id), -1);
                }
                continue;
            }

            CHECK(got >= expected);
            if (got != expected) {
                ++opaqueDifferences;
                CHECK(def.light > 0);
            }
        }

        // And the divergence stays small and confined. If this ever grows, the
        // characterisation above has stopped being true and the claim in the
        // header needs re-deriving rather than the bound relaxing.
        CHECK(opaqueDifferences < 200);

        // The height map falls out of the same pass and is stored in the chunk
        // file, so it is compared too rather than trusted.
        for (int i = 0; i < 256; ++i) {
            CHECK_EQ(int(heightMap[usize(i)]), int(want.heightMap[i]));
        }
    }
}

// `computeHeightMap` is used on its own by the storage path, so it is pinned
// separately from the copy the light engine builds for the whole window.
TEST(the_standalone_height_map_matches_the_jar)
{
    for (int c = 0; c < kLightCaseCount; ++c) {
        const auto& want = kLightCases[c];
        const std::vector<u8> centre = expand(want.blocks[4], want.blockRuns[4], kColumnBlocks);

        std::vector<u8> heightMap(256, 0);
        world::computeHeightMap(centre.data(), heightMap.data());
        for (int i = 0; i < 256; ++i) {
            CHECK_EQ(int(heightMap[usize(i)]), int(want.heightMap[i]));
        }
    }
}

// The fixtures have to contain light worth comparing. A world of solid stone
// would be all zeros and would pass against an engine that did nothing, and a
// world of open sky would be all fifteens and would pass against one that only
// seeded. This asserts both cases carry a real gradient, and that at least one
// carries block light -- which only exists where the generator put lava.
TEST(the_light_fixtures_are_not_degenerate)
{
    int casesWithBlockLight = 0;

    for (int c = 0; c < kLightCaseCount; ++c) {
        const auto& want = kLightCases[c];
        const std::vector<u8> sky = expand(want.sky, want.skyRuns, kColumnBlocks);
        const std::vector<u8> blockLight =
            expand(want.blockLight, want.blockLightRuns, kColumnBlocks);

        int skyHistogram[16] = {};
        for (const u8 value : sky) {
            ++skyHistogram[value];
        }
        // Full dark, full bright, and something in between: the partial values
        // are the ones only a correct propagation produces.
        CHECK(skyHistogram[0] > 0);
        CHECK(skyHistogram[15] > 0);
        int partial = 0;
        for (int v = 1; v <= 14; ++v) {
            partial += skyHistogram[v];
        }
        CHECK(partial > 100);

        for (const u8 value : blockLight) {
            if (value != 0) {
                ++casesWithBlockLight;
                break;
            }
        }
    }

    CHECK(casesWithBlockLight > 0);
}

// The claim the whole design rests on, stated as a measurement rather than as a
// comment: the search settles each cell about once, so its cost is the size of
// the lit region and not the number of boxes the original would have queued.
//
// The bound is deliberately loose -- this is a regression guard against the
// search degenerating into repeated re-scans, not a benchmark. A full re-scan
// of the window per level would be 15 x 294,912.
TEST(the_search_settles_each_cell_about_once)
{
    LightEngine engine;
    const auto& want = kLightCases[0];

    std::vector<std::vector<u8>> storage;
    std::vector<const u8*> columns;
    for (int i = 0; i < 9; ++i) {
        storage.push_back(expand(want.blocks[i], want.blockRuns[i], kColumnBlocks));
    }
    for (const auto& column : storage) {
        columns.push_back(column.data());
    }

    std::vector<u8> sky(16384, 0);
    std::vector<u8> blockLight(16384, 0);
    engine.computeCentre(columns.data(), sky.data(), blockLight.data(), nullptr);

    CHECK(engine.lastSkyVisits() > 0);
    CHECK(engine.lastSkyVisits() < u32(LightEngine::kWindowCells));
}

// A missing neighbour has to read as air rather than as a crash or as stone.
// The original lights against unloaded chunks constantly -- it is what the
// whole box-queue re-scan exists to correct later -- so a null column is an
// ordinary input here, not an error.
TEST(a_missing_neighbour_column_is_air_not_a_crash)
{
    LightEngine engine;
    const auto& want = kLightCases[0];

    const std::vector<u8> centre = expand(want.blocks[4], want.blockRuns[4], kColumnBlocks);
    const u8* columns[9] = {};
    columns[4] = centre.data();

    std::vector<u8> sky(16384, 0);
    std::vector<u8> blockLight(16384, 0);
    engine.computeCentre(columns, sky.data(), blockLight.data(), nullptr);

    // With every neighbour open to the sky, the centre is lit at least as well
    // as it was with real neighbours around it -- light can only be added by
    // removing obstructions, never taken away.
    const std::vector<u8> wantSky = expand(want.sky, want.skyRuns, kColumnBlocks);
    for (int i = 0; i < kColumnBlocks; ++i) {
        CHECK(int(nibbleAt(sky.data(), i)) >= int(wantSky[usize(i)]));
    }
}
