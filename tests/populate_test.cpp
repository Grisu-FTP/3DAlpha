#include "framework.hpp"

#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"
#include "impl/worldgen/alpha_nobiome/populate.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "populate_vectors.hpp"

#include <vector>

using namespace mc;
using mc::test::kPopulateCaseCount;
using mc::test::kPopulateCases;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;

std::vector<u8> expand(const u32* runs, int runCount)
{
    std::vector<u8> out;
    out.reserve(usize(kChunkBytes));
    for (int i = 0; i < runCount; ++i) {
        const u32 run = runs[i];
        const u8 value = u8(run & 0xFF);
        for (u32 k = run >> 8; k > 0; --k) {
            out.push_back(value);
        }
    }
    return out;
}

// Where a mismatch is, in a form that can be looked at: which column of the
// 3x3, and where in it.
struct Mismatch {
    int column = -1;
    int x = 0, y = 0, z = 0;
    int got = 0, want = 0, before = 0;
};

}  // namespace

// **The check no per-generator fixture can make.**
//
// Every generator is already verified block-for-block on its own. What this
// adds is the driver: nine passes sharing one Random, where the order they run
// in and the number of draws each makes are as much a part of the seed as their
// contents. One pass out of place, or one draw too many, moves everything after
// it in the chunk -- and every generator, tested on its own, still passes.
//
// So this compares **all twenty-five columns** of the 5x5, not just the centre.
// Population writes across chunk boundaries constantly: an ore vein grows into
// the neighbour about a third of the time, and a tree planted at the +8 offset
// spreads its canopy into two of them.
TEST(the_whole_population_pass_matches_the_jar)
{
    for (int c = 0; c < kPopulateCaseCount; ++c) {
        const auto& want = kPopulateCases[c];

        std::vector<std::vector<u8>> storage;
        std::vector<u8*> pointers;
        for (int i = 0; i < 25; ++i) {
            storage.push_back(expand(want.before[i], want.beforeRuns[i]));
            CHECK_EQ(int(storage.back().size()), kChunkBytes);
        }
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }

        PopulationView view;
        view.reset(want.chunkX - 2, want.chunkZ - 2, 5, 5, pointers.data());

        worldgen::GeneratorOptions options;
        options.snowCovered = want.snowCovered;
        worldgen::ChunkProvider provider(want.seed, options);

        worldgen::populateChunk(provider, view, want.chunkX, want.chunkZ, nullptr);

        // Not one write may have fallen outside the window. Population's whole
        // premise is that a 3x3 is wide enough; if it is not, the fixture would
        // disagree somewhere far from the cause and this says so directly.
        CHECK_EQ(int(view.refusedOutOfWindow()), 0);

        Mismatch bad;
        for (int i = 0; i < 25 && bad.column < 0; ++i) {
            const std::vector<u8> expected = expand(want.after[i], want.afterRuns[i]);
            CHECK_EQ(int(expected.size()), kChunkBytes);
            const std::vector<u8>& before = storage[usize(i)];

            for (int k = 0; k < kChunkBytes; ++k) {
                if (storage[usize(i)][usize(k)] != expected[usize(k)]) {
                    // storage[] now holds our populated result, because the
                    // view wrote through it.
                    bad.column = i;
                    bad.x = k >> 11;
                    bad.z = (k >> 7) & 15;
                    bad.y = k & 127;
                    bad.got = storage[usize(i)][usize(k)];
                    bad.want = expected[usize(k)];
                    bad.before = before[usize(k)];
                    break;
                }
            }
        }

        if (bad.column >= 0) {
            CHECK_EQ(bad.got, bad.want);  // reports both block ids
            CHECK_EQ(bad.column, -1);     // and which of the 25 columns
            CHECK_EQ(bad.y, -1);          // and the height
            CHECK_EQ(bad.x, -1);
            CHECK_EQ(bad.z, -1);
        }
    }
}

// The fixture has to describe a chunk that population actually changed, or the
// case above would pass against a driver that did nothing at all.
//
// It also has to change the *neighbours*, which is the property that makes the
// 3x3 window necessary rather than merely convenient.
TEST(the_populate_fixture_changes_the_centre_and_its_neighbours)
{
    for (int c = 0; c < kPopulateCaseCount; ++c) {
        const auto& want = kPopulateCases[c];

        int changedColumns = 0;
        int centreChanges = 0;

        for (int i = 0; i < 25; ++i) {
            const std::vector<u8> before = expand(want.before[i], want.beforeRuns[i]);
            const std::vector<u8> after = expand(want.after[i], want.afterRuns[i]);

            int changes = 0;
            for (int k = 0; k < kChunkBytes; ++k) {
                if (before[usize(k)] != after[usize(k)]) {
                    ++changes;
                }
            }
            if (changes > 0) {
                ++changedColumns;
            }
            if (i == 12) {
                centreChanges = changes;
            }
        }

        // A populated chunk gains hundreds of blocks. Measured across the
        // fixture the centre column changes between 274 and 577, so 250 is
        // below the floor with room to spare and far above what a driver
        // missing a pass would produce. The first version of this asserted
        // "> 500" from a fixture that had several chunks' work mixed in.
        CHECK(centreChanges > 250);

        // **Exactly the 2x2 to the east and south, and nothing else.** That is
        // the +8 offset made visible: every pass but the ores addresses
        // `chunkX * 16 + 8`, and the ore generator adds its own +8 before
        // growing a vein, so one chunk's population lands squarely across the
        // quadrant (0,0)..(+1,+1) and never reaches the other twenty-one
        // columns of the 5x5.
        //
        // The 5x5 is carried anyway rather than trimmed to a 2x2, because it
        // is what proves the reach *stops* -- a driver that offset one pass
        // wrongly would write outside the quadrant and this would say so.
        CHECK_EQ(changedColumns, 4);
    }
}

// The snow sweep is the one pass that draws nothing and is therefore invisible
// to a stream fingerprint. The fixture carries the same chunk with snow on and
// off, so the two must differ -- and differ only by snow.
TEST(the_snow_sweep_is_exercised)
{
    int snowyCases = 0;

    for (int c = 0; c < kPopulateCaseCount; ++c) {
        if (!kPopulateCases[c].snowCovered) {
            continue;
        }
        ++snowyCases;

        // Find the matching non-snowy case: same seed, same chunk.
        int plain = -1;
        for (int k = 0; k < kPopulateCaseCount; ++k) {
            if (!kPopulateCases[k].snowCovered && kPopulateCases[k].seed == kPopulateCases[c].seed &&
                kPopulateCases[k].chunkX == kPopulateCases[c].chunkX &&
                kPopulateCases[k].chunkZ == kPopulateCases[c].chunkZ) {
                plain = k;
                break;
            }
        }
        CHECK(plain >= 0);

        const std::vector<u8> snowy =
            expand(kPopulateCases[c].after[12], kPopulateCases[c].afterRuns[12]);
        const std::vector<u8> bare =
            expand(kPopulateCases[plain].after[12], kPopulateCases[plain].afterRuns[12]);

        int snowBlocks = 0;
        for (int k = 0; k < kChunkBytes; ++k) {
            if (snowy[usize(k)] == 78) {
                ++snowBlocks;
            }
        }
        // **64 of the chunk's 256 columns, and the quarter is the point.** The
        // sweep runs over `blockX + 8 .. blockX + 23`, so only the 8x8 corner
        // of it falls inside the chunk that ran it -- the other three quarters
        // land in the neighbours. A sweep written without the +8 would cover
        // all 256 and look perfectly reasonable.
        CHECK_EQ(snowBlocks, 64);

        // The bare case has none at all.
        for (int k = 0; k < kChunkBytes; ++k) {
            CHECK(bare[usize(k)] != 78);
        }
    }

    CHECK(snowyCases >= 1);
}

// **How wide the window has to be, measured rather than argued.**
//
// The fixture above hands population a 5x5 of columns, because that is what
// the oracle was captured with. The chunk generator has to force every column
// of the window into existence before it can populate anything, so the width
// is the difference between a 6x6 and an 8x8 of terrain held resident while a
// single column is being finished -- 1.2 MB against 2.1 MB on a 40 MB heap.
//
// The claim to check is that the outer ring is never *read*: population
// addresses its own chunk plus the 2x2 quadrant east and south of it, so a
// 3x3 centred on the chunk should contain every read as well as every write.
// A read that did leave it would come back as air here and as real terrain in
// the oracle, and the two runs would part company.
//
// So: run every case twice, once with all 25 columns and once with only the
// centre 3x3, and require the nine shared columns to agree byte for byte.
TEST(a_3x3_window_is_as_good_as_the_5x5_the_fixture_uses)
{
    for (int c = 0; c < kPopulateCaseCount; ++c) {
        const auto& want = kPopulateCases[c];

        worldgen::GeneratorOptions options;
        options.snowCovered = want.snowCovered;

        // The 5x5 run, which is the one the case above pins against the jar.
        std::vector<std::vector<u8>> wide;
        std::vector<u8*> widePointers;
        for (int i = 0; i < 25; ++i) {
            wide.push_back(expand(want.before[i], want.beforeRuns[i]));
        }
        for (auto& column : wide) {
            widePointers.push_back(column.data());
        }
        {
            PopulationView view;
            view.reset(want.chunkX - 2, want.chunkZ - 2, 5, 5, widePointers.data());
            worldgen::ChunkProvider provider(want.seed, options);
            worldgen::populateChunk(provider, view, want.chunkX, want.chunkZ, nullptr);
            CHECK_EQ(int(view.refusedOutOfWindow()), 0);
        }

        // The same thing over the centre 3x3 alone. Column (ix, iz) of the 5x5
        // is index ix * 5 + iz, so the 3x3's (jx, jz) is (jx + 1) * 5 + jz + 1.
        std::vector<std::vector<u8>> narrow;
        std::vector<u8*> narrowPointers;
        for (int jx = 0; jx < 3; ++jx) {
            for (int jz = 0; jz < 3; ++jz) {
                const int i = (jx + 1) * 5 + jz + 1;
                narrow.push_back(expand(want.before[i], want.beforeRuns[i]));
            }
        }
        for (auto& column : narrow) {
            narrowPointers.push_back(column.data());
        }
        {
            PopulationView view;
            view.reset(want.chunkX - 1, want.chunkZ - 1, 3, 3, narrowPointers.data());
            worldgen::ChunkProvider provider(want.seed, options);
            worldgen::populateChunk(provider, view, want.chunkX, want.chunkZ, nullptr);

            // If a write ever did leave the 3x3 this is where it would show,
            // and the count is the whole reason PopulationView carries it.
            CHECK_EQ(int(view.refusedOutOfWindow()), 0);
        }

        for (int jx = 0; jx < 3; ++jx) {
            for (int jz = 0; jz < 3; ++jz) {
                const int wideIndex = (jx + 1) * 5 + jz + 1;
                const int narrowIndex = jx * 3 + jz;
                int firstDiff = -1;
                for (int k = 0; k < kChunkBytes; ++k) {
                    if (wide[usize(wideIndex)][usize(k)] != narrow[usize(narrowIndex)][usize(k)]) {
                        firstDiff = k;
                        break;
                    }
                }
                CHECK_EQ(firstDiff, -1);
            }
        }
    }
}
