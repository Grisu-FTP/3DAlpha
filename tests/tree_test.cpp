#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "impl/worldgen/alpha_nobiome/trees.hpp"
#include "tree_vectors.hpp"

#include <map>
#include <vector>

using namespace mc;
using namespace mc::test;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;

// The shared terrain patch written into a 3x3 of chunk columns, then the
// case's scene applied with the same rules genref used.
struct Scene {
    std::vector<std::vector<u8>> storage;
    std::vector<u8*> pointers;
    PopulationView view;

    void build(int scene, i32 x, i32 z)
    {
        const i32 originChunkX = (x >> 4) - 1;
        const i32 originChunkZ = (z >> 4) - 1;

        storage.assign(9, std::vector<u8>(usize(kChunkBytes), 0));
        pointers.clear();
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }
        view.reset(originChunkX, originChunkZ, 3, 3, pointers.data());

        int run = 0;
        u32 remaining = 0;
        u8 value = 0;
        for (i32 dx = -kTreePatch; dx <= kTreePatch; ++dx) {
            for (i32 dz = -kTreePatch; dz <= kTreePatch; ++dz) {
                for (i32 y = 0; y < 128; ++y) {
                    while (remaining == 0) {
                        value = u8(kTreeTerrain[run] & 0xFF);
                        remaining = kTreeTerrain[run] >> 8;
                        ++run;
                    }
                    --remaining;
                    view.setBlock(x + dx, y, z + dz, value);
                }
            }
        }
        // Rebuild the height map now that the blocks are in.
        view.reset(originChunkX, originChunkZ, 3, 3, pointers.data());

        if (scene != kTSceneOpen) {
            for (i32 bx = x - kTreePatch; bx <= x + kTreePatch; ++bx) {
                for (i32 bz = z - kTreePatch; bz <= z + kTreePatch; ++bz) {
                    const i32 top = i32(view.heightAt(bx, bz));
                    if (scene == kTSceneCeiling) {
                        view.setBlock(bx, top + 4, bz, u8(1));
                    } else if (scene == kTSceneLeaves) {
                        view.setBlock(bx, top + 5, bz, u8(18));
                    } else if (scene == kTSceneClip && bx == x + 2 && bz == z) {
                        for (i32 by = top + 1; by <= top + 3; ++by) {
                            view.setBlock(bx, by, bz, u8(1));
                        }
                    }
                }
            }
        }

        view.clearRefusals();
    }

    // Every block in the patch, so the caller can diff before and after.
    std::map<std::vector<i32>, u8> snapshot(i32 x, i32 z) const
    {
        std::map<std::vector<i32>, u8> out;
        for (i32 dx = -kTreePatch; dx <= kTreePatch; ++dx) {
            for (i32 dz = -kTreePatch; dz <= kTreePatch; ++dz) {
                for (i32 y = 0; y < 128; ++y) {
                    out[{dx, y, dz}] = view.blockAt(x + dx, y, z + dz);
                }
            }
        }
        return out;
    }
};

}  // namespace

// WorldGenTrees against the jar's own: every block it changed, what it
// returned, and where the random stream ended up.
TEST(trees_match_the_jar)
{
    for (int c = 0; c < kTreeCaseCount; ++c) {
        const auto& want = kTreeCases[c];

        Scene scene;
        scene.build(want.scene, want.x, want.z);
        const std::map<std::vector<i32>, u8> before = scene.snapshot(want.x, want.z);

        JavaRandom random(want.rngSeed);
        const bool planted =
            worldgen::generateTree(scene.view, random, want.x, want.y, want.z);

        // **The fingerprint first.** A tree's draw count depends on its height
        // and on how many canopy corners it evaluates, and a refused tree
        // draws exactly one number -- so this is the check that catches a
        // corner test that short-circuits the wrong way.
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
            const auto found = before.find(entry.first);
            if (found->second != entry.second) {
                got[entry.first] = entry.second;
            }
        }

        CHECK_EQ(int(got.size()), want.changeCount);
        CHECK(got == expected);
        CHECK_EQ(int(scene.view.refusedOutOfWindow()), 0);
    }
}

// The fixture has to contain trees that grew and trees that did not, and the
// two refusal reasons are different: no headroom at all, and a canopy that
// collides with something the clearance check did not look at.
TEST(the_tree_fixture_covers_growth_and_refusal)
{
    int planted = 0;
    int refused = 0;
    int total = 0;
    int scenes = 0;
    bool seen[4] = {};

    for (int c = 0; c < kTreeCaseCount; ++c) {
        const auto& want = kTreeCases[c];
        total += want.changeCount;
        (want.planted ? planted : refused)++;
        if (!seen[want.scene]) {
            seen[want.scene] = true;
            ++scenes;
        }
        // A refused tree changes nothing at all -- not even the dirt under it,
        // which is written only after every check has passed.
        if (!want.planted) {
            CHECK_EQ(want.changeCount, 0);
        }
    }

    CHECK_EQ(total, kTreeTotalChanges);
    CHECK(planted >= 4);
    CHECK(refused >= 2);
    CHECK_EQ(scenes, 4);
}

// A grown tree has to look like a tree: a trunk of wood, a canopy of leaves,
// and the grass beneath it turned to dirt. Stated separately because the
// case above compares against the jar and would pass just as well if both
// sides agreed on something that was not a tree at all.
TEST(a_grown_tree_has_a_trunk_a_canopy_and_dirt_beneath_it)
{
    for (int c = 0; c < kTreeCaseCount; ++c) {
        const auto& want = kTreeCases[c];
        if (!want.planted) {
            continue;
        }

        int wood = 0;
        int leaves = 0;
        int dirt = 0;
        for (int i = 0; i < want.changeCount; ++i) {
            const auto& change = want.changes[i];
            if (change.id == 17) {
                ++wood;
                // The trunk is a single column directly above the origin.
                CHECK_EQ(change.dx, 0);
                CHECK_EQ(change.dz, 0);
            } else if (change.id == 18) {
                ++leaves;
            } else if (change.id == 3) {
                ++dirt;
                CHECK_EQ(change.y, want.y - 1);
            }
        }

        CHECK(wood >= 4);
        CHECK(wood <= 6);
        CHECK(leaves > 20);
        CHECK_EQ(dirt, 1);
    }
}

// How many trees a chunk gets, and whether they are big ones -- the part of
// `populate` that runs before any tree is placed.
//
// This is the first thing in the whole port that reads the eighth octave
// generator, which was documented for a long time as a mob-spawner noise that
// nothing consumes. If the constructor built the eight generators in a
// different order, or built them from a different Random, every count here
// would be wrong -- so this doubles as a check on the seed derivation that no
// terrain test can make, because terrain never touches this generator.
TEST(the_tree_count_matches_the_jar)
{
    for (int c = 0; c < kTreeBatchCaseCount; ++c) {
        const auto& want = kTreeBatchCases[c];

        worldgen::GeneratorOptions options;
        worldgen::ChunkProvider provider(want.seed, options);

        JavaRandom random(want.rngSeed);
        const worldgen::TreeBatch batch =
            worldgen::treeBatchFor(provider.treeDensity(), random, want.blockX, want.blockZ);

        CHECK_EQ(batch.count, want.count);
        CHECK_EQ(int(batch.big), int(want.big));
        // Three draws whatever the count, so this catches a `nextInt(10)` that
        // was skipped because the chunk had no trees anyway.
        CHECK_EQ(random.nextLong(), want.afterDraw);
    }
}

// And the fixture has to contain variety, or the case above would pass against
// a generator that returned a constant.
TEST(the_tree_count_fixture_is_not_constant)
{
    int distinct = 0;
    bool seen[8] = {};
    int bigCases = 0;
    for (int c = 0; c < kTreeBatchCaseCount; ++c) {
        const int count = kTreeBatchCases[c].count;
        CHECK(count >= 0);
        if (count < 8 && !seen[count]) {
            seen[count] = true;
            ++distinct;
        }
        if (kTreeBatchCases[c].big) {
            ++bigCases;
        }
    }
    CHECK(distinct >= 3);
    CHECK(bigCases > 0);
    CHECK(bigCases < kTreeBatchCaseCount);
}
