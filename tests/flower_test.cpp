#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/flowers.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "flower_vectors.hpp"

#include <set>
#include <vector>

using namespace mc;
using namespace mc::test;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;
constexpr int kPatchSide = 2 * kFlowerPatch + 1;

// Rebuilds what genref had in front of it: the shared terrain patch written
// into a 3x3 of chunk columns, then the case's scene laid over it with the
// same rules `buildFlowerScene` uses.
//
// Columns outside the patch are left as air. That is safe rather than
// convenient: the generator perturbs by at most seven blocks and reads one
// below, so it never looks past the patch, and a test that violated that
// would report a mismatch rather than passing quietly.
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

        // Expand the run-length encoded patch straight into the columns.
        int cell = 0;
        int run = 0;
        u32 remaining = 0;
        u8 value = 0;
        for (int dx = -kFlowerPatch; dx <= kFlowerPatch; ++dx) {
            for (int dz = -kFlowerPatch; dz <= kFlowerPatch; ++dz) {
                for (int y = 0; y < 128; ++y) {
                    while (remaining == 0) {
                        value = u8(kFlowerTerrain[run] & 0xFF);
                        remaining = kFlowerTerrain[run] >> 8;
                        ++run;
                    }
                    --remaining;
                    ++cell;
                    view.setBlock(x + dx, i32(y), z + dz, value);
                }
            }
        }
        (void)cell;

        // `reset` built the height map from empty columns, so it has to be
        // rebuilt now that the blocks are in. Re-resetting is the honest way
        // to say that rather than reaching into the view.
        view.reset(originChunkX, originChunkZ, 3, 3, pointers.data());

        if (scene != kFSceneOpen) {
            for (i32 bx = x - kFlowerBox; bx <= x + kFlowerBox; ++bx) {
                for (i32 bz = z - kFlowerBox; bz <= z + kFlowerBox; ++bz) {
                    const int top = view.heightAt(bx, bz);
                    if (scene == kFSceneStone || scene == kFSceneStoneRoofed) {
                        view.setBlock(bx, i32(top - 1), bz, 1);
                    } else if (scene == kFSceneLeafFloor) {
                        view.setBlock(bx, i32(top - 1), bz, u8(18));
                    }
                    if (scene == kFSceneLowCanopy) {
                        view.setBlock(bx, i32(top + 1), bz, u8(18));
                    } else if (scene != kFSceneStone) {   // includes kFSceneLeafFloor
                        view.setBlock(bx, i32(top + kFlowerRoof), bz,
                                      scene == kFSceneCanopy ? u8(18) : u8(1));
                    }
                }
            }
        }

        view.clearRefusals();
    }
};

}  // namespace

// Dandelions, roses and both mushrooms against the jar's own WorldGenFlowers:
// every block placed, and where the random stream ended up.
TEST(flowers_and_mushrooms_match_the_jar)
{
    for (int c = 0; c < kFlowerCaseCount; ++c) {
        const auto& want = kFlowerCases[c];

        Scene scene;
        scene.build(want.scene, want.x, want.z);

        JavaRandom random(want.rngSeed);
        worldgen::generatePlants(scene.view, random, u8(want.plantId), want.x, want.y, want.z);

        // `ae` draws a fixed 192 numbers whatever it finds -- three per try,
        // sixty-four tries, and neither branch draws again. Checking the
        // fingerprint first is what would catch that claim being wrong.
        CHECK_EQ(random.nextLong(), want.afterDraw);

        std::set<std::vector<i32>> expected;
        for (int p = 0; p < want.placementCount; ++p) {
            expected.insert({want.x + want.placements[p].dx, want.placements[p].y,
                             want.z + want.placements[p].dz});
        }

        // **The same window genref scanned, which is kFlowerBox and not the
        // wider patch.** The terrain here is real generated terrain, so it
        // already contains flowers that a1.1.2's own population put there --
        // scanning wider than the oracle did picks those up as if this
        // generator had placed them. Two dandelions eleven blocks out, well
        // past the seven this generator can reach, are what said so.
        std::set<std::vector<i32>> got;
        for (i32 bx = want.x - kFlowerBox; bx <= want.x + kFlowerBox; ++bx) {
            for (i32 by = want.y - 8; by <= want.y + 8; ++by) {
                for (i32 bz = want.z - kFlowerBox; bz <= want.z + kFlowerBox; ++bz) {
                    if (scene.view.blockAt(bx, by, bz) == u8(want.plantId)) {
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

// **The fixture has to prove the two predicates are not interchangeable.**
// Flowers want light; mushrooms want darkness. A fixture where both only ever
// planted in the same conditions would pass with the two swapped, which is the
// single most likely way to get this generator wrong.
TEST(the_flower_fixture_separates_light_from_dark)
{
    int flowersInOpen = 0;
    int flowersInShade = 0;
    int mushroomsInOpen = 0;
    int mushroomsInShade = 0;
    int total = 0;

    for (int c = 0; c < kFlowerCaseCount; ++c) {
        const auto& want = kFlowerCases[c];
        total += want.placementCount;
        const bool mushroom = want.plantId == 39 || want.plantId == 40;
        const bool shaded = want.scene != kFSceneOpen && want.scene != kFSceneStone;
        if (mushroom) {
            (shaded ? mushroomsInShade : mushroomsInOpen) += want.placementCount;
        } else {
            (shaded ? flowersInShade : flowersInOpen) += want.placementCount;
        }
    }

    CHECK_EQ(total, kFlowerTotalPlacements);

    // Flowers grow in the open, and also in shade -- the light under a canopy
    // is 14, 13, 12 rather than 0, and 8 is enough.
    CHECK(flowersInOpen > 0);
    CHECK(flowersInShade > 0);

    // Mushrooms grow only in shade. **Zero in the open is the assertion that
    // makes the pair non-interchangeable**: it can only hold if the mushroom
    // predicate is the one that refuses bright light.
    CHECK(mushroomsInShade > 0);
    CHECK_EQ(mushroomsInOpen, 0);
}

// The predicates on their own, so a failure says which half is wrong.
TEST(the_two_ground_and_light_predicates_disagree_as_they_should)
{
    const auto& open = kFlowerCases[0];

    Scene lit;
    lit.build(kFSceneOpen, open.x, open.z);
    const i32 surface = i32(lit.view.heightAt(open.x, open.z));

    // Open grass: bright, so a flower is happy and a mushroom is not.
    CHECK(worldgen::flowerCanStay(lit.view, open.x, surface, open.z));
    CHECK(!worldgen::mushroomCanStay(lit.view, open.x, surface, open.z));

    // Under a stone roof: dark, so the positions swap. Sampled at the centre,
    // well away from the edge of the roof where sky still reaches in.
    Scene dark;
    dark.build(kFSceneRoofed, open.x, open.z);
    CHECK(worldgen::mushroomCanStay(dark.view, open.x, surface, open.z));
    CHECK(!worldgen::flowerCanStay(dark.view, open.x, surface, open.z));

    // And the ground test is independent of the light test: stone underfoot
    // refuses a flower even in full daylight, while a mushroom accepts it and
    // is stopped only by the brightness.
    Scene stony;
    stony.build(kFSceneStone, open.x, open.z);
    CHECK(!worldgen::flowerCanStay(stony.view, open.x, surface, open.z));
    CHECK(!worldgen::mushroomCanStay(stony.view, open.x, surface, open.z));
}
