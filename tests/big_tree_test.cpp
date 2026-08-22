#include "framework.hpp"

#include "big_tree_vectors.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/big_tree.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

#include <map>
#include <vector>

using namespace mc;
using namespace mc::test;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;

// The shared terrain patch written into a 5x5 of chunk columns. Five rather
// than three because kBigPatch is 20 blocks either side of a site that is not
// chunk-aligned, so the patch spans three chunks and the tree can then reach
// into a fourth.
struct Scene {
    std::vector<std::vector<u8>> storage;
    std::vector<u8*> pointers;
    PopulationView view;

    void build(i32 x, i32 z, bool obstructed)
    {
        const i32 originChunkX = (x >> 4) - 2;
        const i32 originChunkZ = (z >> 4) - 2;

        storage.assign(25, std::vector<u8>(usize(kChunkBytes), 0));
        pointers.clear();
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }
        view.reset(originChunkX, originChunkZ, 5, 5, pointers.data());

        int run = 0;
        u32 remaining = 0;
        u8 value = 0;
        for (i32 dx = -kBigPatch; dx <= kBigPatch; ++dx) {
            for (i32 dz = -kBigPatch; dz <= kBigPatch; ++dz) {
                for (i32 y = 0; y < 128; ++y) {
                    while (remaining == 0) {
                        value = u8(kBigTreeTerrain[run] & 0xFF);
                        remaining = kBigTreeTerrain[run] >> 8;
                        ++run;
                    }
                    --remaining;
                    view.setBlock(x + dx, y, z + dz, value);
                }
            }
        }
        view.reset(originChunkX, originChunkZ, 5, 5, pointers.data());

        if (obstructed) {
            // The same scattered obstruction genref builds.
            const i32 surface = i32(view.heightAt(x, z));
            for (i32 dx = -6; dx <= 6; ++dx) {
                for (i32 dz = -6; dz <= 6; ++dz) {
                    const i32 ax = dx < 0 ? -dx : dx;
                    const i32 az = dz < 0 ? -dz : dz;
                    if ((ax > az ? ax : az) < 5) {
                        continue;
                    }
                    if (((dx + 6) % 2 != 0) || ((dz + 6) % 2 != 0)) {
                        continue;
                    }
                    for (i32 by = surface + 8; by <= surface + 14; by += 2) {
                        view.setBlock(x + dx, by, z + dz, u8(1));
                    }
                }
            }
            view.reset(originChunkX, originChunkZ, 5, 5, pointers.data());
        }

        view.clearRefusals();
    }

    std::map<std::vector<i32>, u8> snapshot(i32 x, i32 z) const
    {
        std::map<std::vector<i32>, u8> out;
        for (i32 dx = -kBigPatch; dx <= kBigPatch; ++dx) {
            for (i32 dz = -kBigPatch; dz <= kBigPatch; ++dz) {
                for (i32 y = 0; y < 128; ++y) {
                    out[{dx, y, dz}] = view.blockAt(x + dx, y, z + dz);
                }
            }
        }
        return out;
    }
};

}  // namespace

// WorldGenBigTree against the jar's own: every block it changed, and where the
// stream ended up.
//
// This is the generator with the most internal machinery in the whole of
// population -- a leaf-node plan, a line-of-sight check, a line rasteriser and
// four passes over the plan -- and the only one that calls a transcendental on
// a path that matters. If sin/cos disagreed enough to move a branch, this is
// where it would show.
TEST(big_trees_match_the_jar)
{
    for (int c = 0; c < kBigTreeCaseCount; ++c) {
        const auto& want = kBigTreeCases[c];

        Scene scene;
        scene.build(want.x, want.z, want.obstructed);
        const std::map<std::vector<i32>, u8> before = scene.snapshot(want.x, want.z);

        JavaRandom random(want.rngSeed);
        // A fresh state per case, which is what a chunk's first big tree gets.
        // The carried-over height limit is the thing tests/generate_test.cpp
        // covers, because it only exists between two trees of one chunk.
        worldgen::BigTreeState state;
        const bool planted =
            worldgen::generateBigTree(scene.view, random, state, want.x, want.y, want.z);

        // **One draw, whatever the tree did.** `ej` takes a nextLong and then
        // runs on its own Random, so this fingerprint is checking a structural
        // property rather than a count that varies with the shape.
        CHECK_EQ(random.nextLong(), want.afterDraw);
        CHECK_EQ(int(planted), int(want.planted));

        const std::map<std::vector<i32>, u8> after = scene.snapshot(want.x, want.z);

        std::map<std::vector<i32>, u8> expected;
        for (int i = 0; i < want.changeCount; ++i) {
            const auto& change = want.changes[i];
            expected[{change.dx, change.y, change.dz}] = change.id;
        }

        std::map<std::vector<i32>, u8> got;
        for (const auto& entry : after) {
            if (before.find(entry.first)->second != entry.second) {
                got[entry.first] = entry.second;
            }
        }

        CHECK_EQ(int(got.size()), want.changeCount);
        CHECK(got == expected);
        CHECK_EQ(int(scene.view.refusedOutOfWindow()), 0);
    }
}

// A big tree has to actually be big, and be a tree. The comparison above would
// pass just as well if both sides agreed on a single block.
//
// **The thresholds are measured, not guessed.** The first version of this case
// asserted every tree has more than six wood blocks, which failed: three of the
// eight roll a short height, plant no branches at all, and come out with four
// or five. That is correct behaviour -- `branchWorthDrawing` refuses anything
// less than a fifth of the way up -- so the per-tree bound is the weak one and
// the fixture-wide bounds carry the weight.
TEST(a_big_tree_is_taller_and_wider_than_an_ordinary_one)
{
    int total = 0;
    int branchingTrees = 0;
    int tallestOverall = 0;

    for (int c = 0; c < kBigTreeCaseCount; ++c) {
        const auto& want = kBigTreeCases[c];
        total += want.changeCount;
        if (!want.planted) {
            continue;
        }

        int wood = 0;
        int leaves = 0;
        i32 highest = want.y;
        bool offAxisWood = false;

        for (int i = 0; i < want.changeCount; ++i) {
            const auto& change = want.changes[i];
            if (change.id == 17) {
                ++wood;
                // **Branches**, which is what distinguishes this from `oa`:
                // an ordinary tree's wood is a single column, a big tree's is
                // not -- when it is tall enough to have branches at all.
                if (change.dx != 0 || change.dz != 0) {
                    offAxisWood = true;
                }
            } else if (change.id == 18) {
                ++leaves;
            }
            if (change.y > highest) {
                highest = change.y;
            }
        }

        // Every tree, however short: a trunk and a canopy far larger than the
        // ordinary generator's. 69 rather than 70 because the obstructed cases
        // lose a leaf or two to the pillars -- measured, not padded.
        CHECK(wood >= 4);
        CHECK(leaves >= 69);

        if (offAxisWood) {
            ++branchingTrees;
        }
        if (highest - want.y > tallestOverall) {
            tallestOverall = highest - want.y;
        }
    }

    CHECK_EQ(total, kBigTreeTotalChanges);

    // Across the fixture: most of these branch, and at least one reaches well
    // past the six blocks an ordinary tree tops out at.
    CHECK(branchingTrees >= 4);
    CHECK(tallestOverall > 10);
}

// **The obstructed cases, which exist for one line of the generator.** On clear
// ground every `clearLineLength` call runs through air and returns -1 whatever
// rounding it uses, so the difference between the checked line (truncation) and
// the drawn line (round-to-nearest) is invisible -- mutating one to match the
// other passed the whole suite before these were added. Pillars inside the
// canopy's reach are what make the two distinguishable.
TEST(the_fixture_reaches_the_obstructed_line_check)
{
    int obstructed = 0;
    for (int c = 0; c < kBigTreeCaseCount; ++c) {
        if (kBigTreeCases[c].obstructed) {
            ++obstructed;
            // A tree that grew at all, so the check line was consulted rather
            // than the whole attempt being refused up front.
            CHECK(kBigTreeCases[c].planted);
            CHECK(kBigTreeCases[c].changeCount > 0);
        }
    }
    CHECK(obstructed >= 4);
}

// The fixture has to hold trees of different shapes, or one lucky agreement
// would stand in for all of them. The change counts vary by a factor of five
// across these seeds, which only happens if the leaf-node plan really is
// seed-dependent.
TEST(the_big_tree_fixture_covers_different_shapes)
{
    int smallest = kBigTreeCases[0].changeCount;
    int largest = kBigTreeCases[0].changeCount;
    for (int c = 1; c < kBigTreeCaseCount; ++c) {
        const int count = kBigTreeCases[c].changeCount;
        if (count < smallest) {
            smallest = count;
        }
        if (count > largest) {
            largest = count;
        }
    }
    CHECK(smallest > 0);
    CHECK(largest > smallest * 2);
}
