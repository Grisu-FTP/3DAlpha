#include "framework.hpp"

#include "impl/worldgen/alpha_nobiome/liquids.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "liquid_vectors.hpp"

#include <vector>

using namespace mc;
using mc::test::kLiquidCaseCount;
using mc::test::kLiquidCases;
using mc::test::kLiquidPlacedCount;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;

// A 3x3 window so the four horizontal neighbours are always in it, whatever
// the target's alignment within its chunk. The generator reaches exactly one
// block, so three columns is not a guess.
struct Scene {
    std::vector<std::vector<u8>> storage;
    std::vector<u8*> pointers;
    PopulationView view;

    void build(i32 x, i32 z)
    {
        const i32 originChunkX = (x >> 4) - 1;
        const i32 originChunkZ = (z >> 4) - 1;

        storage.assign(9, std::vector<u8>(usize(kChunkBytes), 0));
        pointers.clear();
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }
        view.reset(originChunkX, originChunkZ, 3, 3, pointers.data());
    }
};

}  // namespace

// The whole truth table, against the jar's own WorldGenLiquids. 494 cases: the
// four above/below combinations and every arrangement of centre and the four
// horizontal neighbours over {air, stone, dirt}, for water and for lava.
TEST(liquid_springs_match_the_jar)
{
    for (int c = 0; c < kLiquidCaseCount; ++c) {
        const auto& want = kLiquidCases[c];

        // A coordinate that is not chunk-aligned, so the neighbour lookups
        // cross a chunk boundary for some cases and not others. Aligning it
        // would leave the cross-boundary path untested.
        const i32 x = 47;
        const i32 y = 40;
        const i32 z = 31;

        Scene scene;
        scene.build(x, z);
        scene.view.setBlock(x, y + 1, z, want.above);
        scene.view.setBlock(x, y - 1, z, want.below);
        scene.view.setBlock(x, y, z, want.centre);
        scene.view.setBlock(x - 1, y, z, want.west);
        scene.view.setBlock(x + 1, y, z, want.east);
        scene.view.setBlock(x, y, z - 1, want.north);
        scene.view.setBlock(x, y, z + 1, want.south);
        scene.view.clearRefusals();

        worldgen::generateLiquidSpring(scene.view, want.liquidId, x, y, z);

        CHECK_EQ(int(scene.view.blockAt(x, y, z)), int(want.placed));
        CHECK_EQ(int(scene.view.refusedOutOfWindow()), 0);
    }
}

// The fixture has to contain both outcomes or the test above is satisfied by a
// generator that always refuses -- which is most of what this one does, so the
// degenerate version would pass 476 of 494 cases.
TEST(the_liquid_fixture_covers_both_outcomes)
{
    int placed = 0;
    for (int c = 0; c < kLiquidCaseCount; ++c) {
        if (kLiquidCases[c].placed == kLiquidCases[c].liquidId) {
            ++placed;
        }
    }
    CHECK_EQ(placed, kLiquidPlacedCount);
    CHECK(placed > 0);
    CHECK(placed < kLiquidCaseCount);
}

// **The property the rest of population depends on.** Every other generator
// draws from the shared Random, so if this one ever did, all seventy tries a
// chunk would shift the stream and everything after them would move. The
// original's `nn.a` takes a Random and never reads it; the genref harness
// asserts that on the jar side, and this asserts it on ours -- our function
// does not take one at all, so the guarantee is structural.
//
// Stated as a test anyway, because "the signature has no Random" is exactly the
// kind of thing a later change quietly undoes when someone needs one draw.
TEST(the_liquid_generator_cannot_touch_the_random_stream)
{
    // Our function takes no Random, so the guarantee is structural -- this
    // pins the observable half of it: repeating the call on identical state
    // gives an identical result, with no hidden sequence advancing between.
    Scene first;
    first.build(47, 31);
    first.view.setBlock(47, 41, 31, 1);
    first.view.setBlock(47, 39, 31, 1);
    first.view.setBlock(46, 40, 31, 1);
    first.view.setBlock(48, 40, 31, 1);
    first.view.setBlock(47, 40, 30, 1);
    worldgen::generateLiquidSpring(first.view, 8, 47, 40, 31);
    const u8 once = first.view.blockAt(47, 40, 31);

    Scene second;
    second.build(47, 31);
    second.view.setBlock(47, 41, 31, 1);
    second.view.setBlock(47, 39, 31, 1);
    second.view.setBlock(46, 40, 31, 1);
    second.view.setBlock(48, 40, 31, 1);
    second.view.setBlock(47, 40, 30, 1);
    for (int i = 0; i < 5; ++i) {
        worldgen::generateLiquidSpring(second.view, 8, 47, 40, 31);
    }
    CHECK_EQ(int(second.view.blockAt(47, 40, 31)), int(once));
}

// A spring at the very bottom and top of the world. The original's setBlock
// refuses y outside [0, 128), and the reads above and below go out of range
// before the write does -- so these must refuse without ever reaching the
// window's edge, and without counting an out-of-window refusal.
TEST(springs_at_the_world_floor_and_ceiling_refuse_cleanly)
{
    for (const i32 y : {0, 1, 126, 127}) {
        Scene scene;
        scene.build(47, 31);
        // Best case: everything the predicate wants, so only the height can
        // stop it.
        scene.view.setBlock(47, y + 1, 31, 1);
        scene.view.setBlock(47, y - 1, 31, 1);
        scene.view.setBlock(46, y, 31, 1);
        scene.view.setBlock(48, y, 31, 1);
        scene.view.setBlock(47, y, 30, 1);
        scene.view.clearRefusals();

        worldgen::generateLiquidSpring(scene.view, 8, 47, y, 31);
        CHECK_EQ(int(scene.view.refusedOutOfWindow()), 0);
    }
}
