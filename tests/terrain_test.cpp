#include "framework.hpp"

#include "blocks.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"
#include "terrain_vectors.hpp"

#include <vector>

using namespace mc;
using mc::test::kTerrainCaseCount;
using mc::test::kTerrainCases;
using worldgen::ChunkProvider;
using worldgen::GeneratorOptions;
using worldgen::kChunkBlocks;

namespace {

// The fixtures are run-length encoded as (count << 8) | blockId; see
// tools/genref.java for why. Expanding here rather than comparing run-by-run
// means a failure reports a block index, which can be turned straight into
// x/y/z, instead of a run number that cannot.
std::vector<u8> expand(const u32* runs, int runCount)
{
    std::vector<u8> out;
    out.reserve(usize(kChunkBlocks));
    for (int i = 0; i < runCount; ++i) {
        const u32 run = runs[i];
        const u8 value = u8(run & 0xFF);
        const u32 count = run >> 8;
        for (u32 k = 0; k < count; ++k) {
            out.push_back(value);
        }
    }
    return out;
}

// A failure here is one byte in 32,768, so say where it is in a form that can
// be looked at rather than just which index disagreed.
struct Mismatch {
    int index = -1;
    int x = 0;
    int y = 0;
    int z = 0;
    u8 got = 0;
    u8 want = 0;
};

Mismatch firstMismatch(const std::vector<u8>& got, const std::vector<u8>& want)
{
    Mismatch out;
    for (int i = 0; i < kChunkBlocks; ++i) {
        if (got[usize(i)] != want[usize(i)]) {
            out.index = i;
            out.x = i >> 11;
            out.z = (i >> 7) & 15;
            out.y = i & 127;
            out.got = got[usize(i)];
            out.want = want[usize(i)];
            return out;
        }
    }
    return out;
}

}  // namespace

// Raw terrain -- stone, water, ice and air -- against the jar's own
// ChunkProviderGenerate, every byte of every column.
TEST(generate_terrain_matches_the_jar)
{
    for (int c = 0; c < kTerrainCaseCount; ++c) {
        const auto& want = kTerrainCases[c];

        GeneratorOptions options;
        options.snowCovered = want.snowCovered;
        ChunkProvider provider(want.seed, options);

        std::vector<u8> blocks(usize(kChunkBlocks), 0);
        // The seed is set here rather than by generateColumn because this case
        // is about generateTerrain alone -- and generateTerrain draws no random
        // numbers at all, so the value cannot matter. Setting it anyway proves
        // that claim: if terrain ever started consuming the stream, this test
        // would still pass while the surface case below would not, and the two
        // together localise it.
        provider.random().setSeed(0);
        provider.generateTerrain(want.chunkX, want.chunkZ, blocks.data());

        const std::vector<u8> expected = expand(want.terrain, want.terrainRuns);
        CHECK_EQ(int(expected.size()), kChunkBlocks);

        const Mismatch bad = firstMismatch(blocks, expected);
        if (bad.index >= 0) {
            CHECK_EQ(int(bad.got), int(bad.want));  // reports both ids
            CHECK_EQ(bad.index, -1);                // and where
        }
    }
}

// Terrain plus the surface pass, which is where the shared Random starts
// mattering: three draws per column and one nextInt(6) on every Y step.
TEST(replace_blocks_for_biome_matches_the_jar)
{
    for (int c = 0; c < kTerrainCaseCount; ++c) {
        const auto& want = kTerrainCases[c];

        GeneratorOptions options;
        options.snowCovered = want.snowCovered;
        ChunkProvider provider(want.seed, options);

        std::vector<u8> blocks(usize(kChunkBlocks), 0);
        // Deliberately not generateColumn: that carves caves too, and this case
        // is about the surface pass on its own. The chunk seed is set the way
        // provideChunk sets it, since the surface pass draws from it.
        const u64 chunkSeed =
            u64(i64(want.chunkX)) * 341873128712ULL + u64(i64(want.chunkZ)) * 132897987541ULL;
        provider.random().setSeed(i64(chunkSeed));
        provider.generateTerrain(want.chunkX, want.chunkZ, blocks.data());
        provider.replaceBlocksForBiome(want.chunkX, want.chunkZ, blocks.data());

        const std::vector<u8> expected = expand(want.surface, want.surfaceRuns);
        CHECK_EQ(int(expected.size()), kChunkBlocks);

        const Mismatch bad = firstMismatch(blocks, expected);
        if (bad.index >= 0) {
            CHECK_EQ(int(bad.got), int(bad.want));
            CHECK_EQ(bad.index, -1);
        }
    }
}

// Terrain, surface and **caves** -- the whole of provideChunk short of
// population. This is the case the cave carver has to survive: 289 reseeded
// cells per column, a float-only tunnel walk, a 65,536-entry sine table, and an
// off-by-one in the carve loop that is genuinely in the original.
TEST(caves_match_the_jar)
{
    for (int c = 0; c < kTerrainCaseCount; ++c) {
        const auto& want = kTerrainCases[c];

        GeneratorOptions options;
        options.snowCovered = want.snowCovered;
        ChunkProvider provider(want.seed, options);

        std::vector<u8> blocks(usize(kChunkBlocks), 0);
        provider.generateColumn(want.chunkX, want.chunkZ, blocks.data());

        const std::vector<u8> expected = expand(want.caved, want.caveRuns);
        CHECK_EQ(int(expected.size()), kChunkBlocks);

        const Mismatch bad = firstMismatch(blocks, expected);
        if (bad.index >= 0) {
            CHECK_EQ(int(bad.got), int(bad.want));
            CHECK_EQ(bad.index, -1);
        }
    }
}

// Caves are sparse -- fourteen chunk cells in fifteen roll out entirely -- so a
// fixture set chosen for terrain can leave the carver almost untouched and
// still pass. This asserts the fixtures actually exercise it, and would fail if
// someone replaced the cave-heavy cases with quiet ones.
TEST(the_cave_fixtures_actually_carve_something)
{
    int totalCarved = 0;
    for (int c = 0; c < kTerrainCaseCount; ++c) {
        const auto& want = kTerrainCases[c];
        const std::vector<u8> before = expand(want.surface, want.surfaceRuns);
        const std::vector<u8> after = expand(want.caved, want.caveRuns);
        for (int i = 0; i < kChunkBlocks; ++i) {
            if (before[usize(i)] != after[usize(i)]) {
                ++totalCarved;
            }
        }
    }
    CHECK(totalCarved > 1000);
}

// **The Far Lands.** Four of the fixtures sit at chunk +-784,426 and beyond,
// which is where `latticeX * 684.412` crosses 2^31 and the `d2i` in the Perlin
// lattice becomes a clamp. The three cases above already compare those chunks
// byte for byte against the jar, so faithfulness is covered; what this case
// covers is that the *fixtures are still Far Lands fixtures*.
//
// That matters because the failure mode is invisible. Regenerate the vectors
// with those four rows deleted, or with coordinates a few chunks short of the
// boundary, and every test above still passes while the entire question of
// what happens past 12,550,824 stops being asked.
//
// The threshold is measured, not guessed. Across the ten ordinary fixtures
// **not one column is solid at y = 100 or above**; across the four far ones the
// count at y = 120 runs 49, 57, 112 and 112 out of 256. There is no overlap to
// be careful about, so 32 sits in the gap.
TEST(the_far_lands_fixtures_are_actually_far_lands)
{
    // 784,426 is the first chunk any of whose five lattice columns overflows.
    auto isFarOut = [](const auto& c) {
        return c.chunkX >= 784426 || c.chunkX <= -784426 || c.chunkZ >= 784426 ||
               c.chunkZ <= -784426;
    };

    int farCases = 0;
    for (int c = 0; c < kTerrainCaseCount; ++c) {
        const auto& want = kTerrainCases[c];
        if (!isFarOut(want)) {
            continue;
        }
        ++farCases;

        const std::vector<u8> blocks = expand(want.terrain, want.terrainRuns);

        int tall = 0;
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                if (blocks[usize((x << 11) | (z << 7) | 120)] != 0) {
                    ++tall;
                }
            }
        }
        CHECK(tall >= 32);
    }

    // Both signs of X and a Z case, because they fail differently: the positive
    // side clamps to Integer.MAX_VALUE and the negative side clamps to
    // Integer.MIN_VALUE and then wraps past it in the floor's decrement.
    CHECK_EQ(farCases, 4);
}

// And the other half of the same claim: an ordinary chunk must *not* look like
// that. Without this, the case above would still pass if a bug made every chunk
// in the world a wall to the ceiling -- which is precisely the shape of a
// mistake in a saturating cast, since a clamp that fires everywhere would put
// the Far Lands at the origin.
TEST(ordinary_chunks_are_empty_at_the_build_ceiling)
{
    for (int c = 0; c < kTerrainCaseCount; ++c) {
        const auto& want = kTerrainCases[c];
        if (want.chunkX >= 784426 || want.chunkX <= -784426 || want.chunkZ >= 784426 ||
            want.chunkZ <= -784426) {
            continue;
        }

        const std::vector<u8> blocks = expand(want.terrain, want.terrainRuns);
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                CHECK_EQ(int(blocks[usize((x << 11) | (z << 7) | 100)]), 0);
            }
        }
    }
}

// The chunk seed, on its own. It is `cx * 341873128712 + cz * 132897987541`
// with **no XOR against the world seed** -- unlike the population pass, which
// does XOR -- and it overflows 64 bits routinely, which is defined in Java and
// undefined in C++ if written the obvious way.
//
// Checked by consequence rather than by reading the field: two different world
// seeds at the same chunk must still produce different terrain (the generators
// differ), while the same world seed at the same chunk must be reproducible.
TEST(chunk_seeding_is_reproducible_and_seed_dependent)
{
    GeneratorOptions options;

    ChunkProvider first(1234567890LL, options);
    std::vector<u8> a(usize(kChunkBlocks), 0);
    first.generateColumn(37, -14, a.data());

    ChunkProvider again(1234567890LL, options);
    std::vector<u8> b(usize(kChunkBlocks), 0);
    again.generateColumn(37, -14, b.data());

    ChunkProvider other(1234567891LL, options);
    std::vector<u8> c(usize(kChunkBlocks), 0);
    other.generateColumn(37, -14, c.data());

    CHECK(a == b);
    CHECK(a != c);
}

// A provider is reused across chunks, and its scratch buffers are members that
// are never cleared between columns. That is how the original works too, so it
// is only correct if every buffer is fully overwritten each time. If any is
// not, a chunk generated second would differ from the same chunk generated
// first -- which is exactly the sort of bug that produces a world that is
// self-consistent, wrong, and impossible to reproduce in a unit test that only
// ever generates one chunk.
TEST(a_reused_provider_generates_the_same_column_as_a_fresh_one)
{
    GeneratorOptions options;

    ChunkProvider fresh(99LL, options);
    std::vector<u8> once(usize(kChunkBlocks), 0);
    fresh.generateColumn(5, 5, once.data());

    ChunkProvider reused(99LL, options);
    std::vector<u8> scratch(usize(kChunkBlocks), 0);
    reused.generateColumn(-3, 12, scratch.data());
    reused.generateColumn(40, -40, scratch.data());
    std::vector<u8> twice(usize(kChunkBlocks), 0);
    reused.generateColumn(5, 5, twice.data());

    CHECK(once == twice);
}

// Sea level is 64 and the ice option puts ice at 63, on top of the water rather
// than instead of it. Stated as its own case because the vectors above would
// pass just as happily with ice *replacing* the whole water column, and the
// difference is a lake you can walk on versus a lake made of ice.
TEST(snow_covered_puts_ice_only_at_the_waterline)
{
    GeneratorOptions plain;
    GeneratorOptions snowy;
    snowy.snowCovered = true;

    ChunkProvider warm(1234567890LL, plain);
    std::vector<u8> warmBlocks(usize(kChunkBlocks), 0);
    warm.generateColumn(0, 0, warmBlocks.data());

    ChunkProvider cold(1234567890LL, snowy);
    std::vector<u8> coldBlocks(usize(kChunkBlocks), 0);
    cold.generateColumn(0, 0, coldBlocks.data());

    constexpr u8 kIce = u8(mcver::Block::Ice);
    int iceBlocks = 0;
    for (int i = 0; i < kChunkBlocks; ++i) {
        if (coldBlocks[usize(i)] == kIce) {
            ++iceBlocks;
            CHECK_EQ(i & 127, 63);  // sea level - 1, and nowhere else
        }
    }

    // The warm world has none at all.
    for (int i = 0; i < kChunkBlocks; ++i) {
        CHECK(warmBlocks[usize(i)] != kIce);
    }

    // And the chosen chunk actually has water in it, or the case proves
    // nothing. Case 4 of the fixtures is this same chunk with snow on.
    CHECK(iceBlocks > 0);
}
