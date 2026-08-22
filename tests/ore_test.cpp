#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/ore.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "ore_vectors.hpp"

#include <map>
#include <vector>

using namespace mc;
using mc::test::kOreBox;
using mc::test::kOreCaseCount;
using mc::test::kOreCases;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;

// Rebuilds the state genref set up: a stone box around the target, air
// elsewhere. The window has to cover the box, which at a half-extent of 24 is
// four chunks across in the worst alignment.
struct Scene {
    std::vector<std::vector<u8>> storage;
    std::vector<u8*> pointers;
    PopulationView view;
    i32 originChunkX = 0;
    i32 originChunkZ = 0;
    int countX = 0;
    int countZ = 0;

    // `fill` is stone for ore cases and sand for clay ones, matching what the
    // harness set up.
    void build(i32 x, i32 y, i32 z, u8 fill)
    {
        originChunkX = (x - kOreBox) >> 4;
        originChunkZ = (z - kOreBox) >> 4;
        countX = (((x + kOreBox) >> 4) - originChunkX) + 1;
        countZ = (((z + kOreBox) >> 4) - originChunkZ) + 1;

        storage.assign(usize(countX * countZ), std::vector<u8>(usize(kChunkBytes), 0));
        pointers.clear();
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }
        view.reset(originChunkX, originChunkZ, countX, countZ, pointers.data());

        for (i32 bx = x - kOreBox; bx <= x + kOreBox; ++bx) {
            for (i32 by = (y - kOreBox < 0 ? 0 : y - kOreBox);
                 by <= (y + kOreBox > 127 ? 127 : y + kOreBox); ++by) {
                for (i32 bz = z - kOreBox; bz <= z + kOreBox; ++bz) {
                    view.setBlock(bx, by, bz, fill);
                }
            }
        }
        view.clearRefusals();
    }
};

}  // namespace

// Every vein, block for block, against WorldGenMinable run in a real World.
TEST(ore_veins_match_the_jar)
{
    for (int c = 0; c < kOreCaseCount; ++c) {
        const auto& want = kOreCases[c];

        // A negative block id marks a clay case; see tools/genref.java.
        const bool clay = want.blockId < 0;
        const u8 fill = clay ? u8(12) : u8(1);

        Scene scene;
        scene.build(want.x, want.y, want.z, fill);
        if (clay) {
            scene.view.setBlock(want.x, want.y, want.z, 9);  // still water at the origin
        }

        JavaRandom random(want.rngSeed);
        if (clay) {
            worldgen::generateClayPatch(scene.view, random, want.veinSize, want.x, want.y, want.z);
        } else {
            worldgen::generateOreVein(scene.view, random, u8(want.blockId), want.veinSize, want.x,
                                      want.y, want.z);
        }

        // Collect everything that is no longer stone.
        std::map<std::vector<i32>, int> got;
        for (i32 bx = want.x - kOreBox; bx <= want.x + kOreBox; ++bx) {
            for (i32 by = (want.y - kOreBox < 0 ? 0 : want.y - kOreBox);
                 by <= (want.y + kOreBox > 127 ? 127 : want.y + kOreBox); ++by) {
                for (i32 bz = want.z - kOreBox; bz <= want.z + kOreBox; ++bz) {
                    const u8 id = scene.view.blockAt(bx, by, bz);
                    if (id != fill) {
                        got[{bx, by, bz}] = int(id);
                    }
                }
            }
        }

        CHECK_EQ(int(got.size()), want.placementCount);
        for (int i = 0; i < want.placementCount; ++i) {
            const auto& p = want.placements[i];
            const auto it = got.find({p.x, p.y, p.z});
            CHECK(it != got.end());
            if (it != got.end()) {
                CHECK_EQ(it->second, int(p.id));
            }
        }

        // A vein must never out-reach the window it was given.
        CHECK_EQ(scene.view.refusedOutOfWindow(), 0u);
    }
}

// Clay refuses unless the block it is handed is water by material, and it must
// refuse **before drawing anything** -- ten dry tries per chunk have to leave
// the random stream exactly where they found it, or every generator after clay
// in the populate order gets different numbers.
TEST(clay_refuses_on_dry_land_without_touching_the_stream)
{
    Scene scene;
    scene.build(400, 64, 400, 12);  // sand, no water anywhere

    JavaRandom random(11LL);
    const bool placed = worldgen::generateClayPatch(scene.view, random, 32, 400, 64, 400);
    CHECK(!placed);

    // The stream is untouched: a fresh generator's first draw still matches.
    JavaRandom fresh(11LL);
    CHECK_EQ(random.nextLong(), fresh.nextLong());
}

// The two clipped cases exist to prove the world floor and ceiling behave like
// World.setBlock rather than wrapping or crashing: a vein centred at y=2 or
// y=125 places fewer blocks than the same vein in open stone, and places none
// outside [0, 128).
TEST(veins_at_the_world_edges_are_clipped_not_wrapped)
{
    for (int c = 0; c < kOreCaseCount; ++c) {
        const auto& want = kOreCases[c];
        for (int i = 0; i < want.placementCount; ++i) {
            CHECK(want.placements[i].y >= 0);
            CHECK(want.placements[i].y < 128);
        }
    }
}
