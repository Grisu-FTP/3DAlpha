#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/plants.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "plant_vectors.hpp"

#include <set>
#include <vector>

using namespace mc;
using namespace mc::test;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;
constexpr u8 kReed = 83;
constexpr u8 kCactus = 81;

// **This has to build exactly the scene tools/genref.java built.** The fixture
// carries the plants, not the terrain, so any disagreement here shows up as a
// placement mismatch and looks like a bug in the generator. The parameters --
// box, ground top, depth, headroom, pool bounds -- are all emitted into the
// fixture rather than written twice, so only the *shape* below can drift.
struct Scene {
    std::vector<std::vector<u8>> storage;
    std::vector<u8*> pointers;
    PopulationView view;

    void build(int scene, i32 x, i32 z)
    {
        const i32 originChunkX = (x - kPlantBox) >> 4;
        const i32 originChunkZ = (z - kPlantBox) >> 4;
        const int countX = (((x + kPlantBox) >> 4) - originChunkX) + 1;
        const int countZ = (((z + kPlantBox) >> 4) - originChunkZ) + 1;

        storage.assign(usize(countX * countZ), std::vector<u8>(usize(kChunkBytes), 0));
        pointers.clear();
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }
        view.reset(originChunkX, originChunkZ, countX, countZ, pointers.data());

        const int top = kPlantGroundTop;
        const bool sandy = scene == kSceneSand || scene == kSceneSandScatter;

        for (i32 bx = x - kPlantBox; bx <= x + kPlantBox; ++bx) {
            for (i32 bz = z - kPlantBox; bz <= z + kPlantBox; ++bz) {
                for (i32 by = top - kPlantGroundDepth; by < top; ++by) {
                    view.setBlock(bx, by, bz, sandy ? 12 : 3);
                }
                view.setBlock(bx, top, bz, sandy ? 12 : 2);
                for (i32 by = top + 1; by <= top + kPlantHeadroom; ++by) {
                    view.setBlock(bx, by, bz, 0);
                }
                // Java's % keeps the sign of the dividend and so does C++'s, and
                // both coordinates here are positive, so this matches without
                // needing a floored modulus.
                if (scene == kSceneSandScatter && ((bx + bz) % 3 == 0)) {
                    view.setBlock(bx, top + 1, bz, 1);
                }
            }
        }

        if (scene == kSceneGrassWater) {
            for (i32 bx = x + kWaterX0; bx <= x + kWaterX1; ++bx) {
                for (i32 bz = z + kWaterZ0; bz <= z + kWaterZ1; ++bz) {
                    view.setBlock(bx, top, bz, 9);
                }
            }
        }

        view.clearRefusals();
    }
};

}  // namespace

// Reeds and cactus against the jar's own generators: every block placed, and
// the position of the random stream afterwards.
TEST(reeds_and_cactus_match_the_jar)
{
    for (int c = 0; c < kPlantCaseCount; ++c) {
        const auto& want = kPlantCases[c];

        Scene scene;
        scene.build(want.scene, want.x, want.z);

        JavaRandom random(want.rngSeed);
        if (want.kind == 0) {
            worldgen::generateReeds(scene.view, random, want.x, want.y, want.z);
        } else {
            worldgen::generateCactus(scene.view, random, want.x, want.y, want.z);
        }

        // **The stream fingerprint, checked first.** If the draw count is wrong
        // this is the failure that says so, and reporting it before the block
        // comparison means a desync is not first reported as forty misplaced
        // plants.
        CHECK_EQ(random.nextLong(), want.afterDraw);

        const u8 plantId = want.kind == 0 ? kReed : kCactus;

        // Every placement the jar made, and no others.
        std::set<std::vector<i32>> expected;
        for (int p = 0; p < want.placementCount; ++p) {
            expected.insert({want.x + want.placements[p].dx, want.placements[p].y,
                             want.z + want.placements[p].dz});
        }

        std::set<std::vector<i32>> got;
        for (i32 bx = want.x - kPlantBox; bx <= want.x + kPlantBox; ++bx) {
            for (i32 by = kPlantGroundTop; by <= kPlantGroundTop + kPlantHeadroom; ++by) {
                for (i32 bz = want.z - kPlantBox; bz <= want.z + kPlantBox; ++bz) {
                    if (scene.view.blockAt(bx, by, bz) == plantId) {
                        got.insert({bx, by, bz});
                    }
                }
            }
        }

        CHECK_EQ(int(got.size()), want.placementCount);
        CHECK(got == expected);
        CHECK_EQ(int(scene.view.refusedOutOfWindow()), 0);
    }
}

// The fixture must contain cases that place and cases that refuse. Six of the
// twelve place nothing at all -- wrong ground, no water, blocked neighbours --
// and those are the ones that pin the *draw count* on the refusal path, which
// is where a desync actually comes from.
TEST(the_plant_fixture_covers_both_outcomes)
{
    int total = 0;
    int placingCases = 0;
    int refusingCases = 0;
    int reeds = 0;
    int cacti = 0;

    for (int c = 0; c < kPlantCaseCount; ++c) {
        total += kPlantCases[c].placementCount;
        if (kPlantCases[c].placementCount > 0) {
            ++placingCases;
            if (kPlantCases[c].kind == 0) {
                ++reeds;
            } else {
                ++cacti;
            }
        } else {
            ++refusingCases;
        }
    }

    CHECK_EQ(total, kPlantTotalPlacements);
    CHECK(placingCases >= 4);
    CHECK(refusingCases >= 4);
    // Both generators have to be exercised positively, not just one of them.
    CHECK(reeds > 0);
    CHECK(cacti > 0);
}

// The predicates on their own, so a failure says which half is wrong.
TEST(reed_placement_needs_water_beside_the_block_below)
{
    Scene scene;
    scene.build(kSceneGrassWater, 800, 800);
    const i32 top = kPlantGroundTop;

    // The pool runs from z+2 to z+3, so z+1 is the shoreline and z-1 is not.
    CHECK(worldgen::reedCanStay(scene.view, 800, top + 1, 801));
    CHECK(!worldgen::reedCanStay(scene.view, 800, top + 1, 799));

    // Stacking: a reed on a reed stays regardless of water. Placed two above
    // the ground so the block below is the reed and not the grass.
    scene.view.setBlock(800, top + 1, 799, kReed);
    CHECK(worldgen::reedCanStay(scene.view, 800, top + 2, 799));
}

TEST(cactus_refuses_a_solid_neighbour_and_the_wrong_ground)
{
    Scene sandy;
    sandy.build(kSceneSand, 800, 800);
    const i32 top = kPlantGroundTop;

    CHECK(worldgen::cactusCanStay(sandy.view, 800, top + 1, 800));

    // One solid neighbour is enough to refuse, and it is tested on all four
    // sides -- so check each side separately rather than trusting one.
    for (int side = 0; side < 4; ++side) {
        Scene blocked;
        blocked.build(kSceneSand, 800, 800);
        const i32 dx = side == 0 ? -1 : (side == 1 ? 1 : 0);
        const i32 dz = side == 2 ? -1 : (side == 3 ? 1 : 0);
        blocked.view.setBlock(800 + dx, top + 1, 800 + dz, 1);
        CHECK(!worldgen::cactusCanStay(blocked.view, 800, top + 1, 800));
    }

    // Grass underneath is refused where sand is accepted.
    Scene grassy;
    grassy.build(kSceneGrass, 800, 800);
    CHECK(!worldgen::cactusCanStay(grassy.view, 800, top + 1, 800));
}
