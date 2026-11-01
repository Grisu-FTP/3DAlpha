// A guest's footsteps, paid out on the host's world. See
// core/entity/remote_walk.hpp.

#include "framework.hpp"

#include "core/entity/remote_walk.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

using namespace mc;
using mc::entity::RemoteWalk;
using mc::test::SceneWorld;

namespace {

// A stone floor at y = 63, so a body stands with its feet at 64.0.
struct Floor {
    SceneWorld scene{0, 0};
    int lastY = 0;

    Floor()
    {
        for (i32 x = -16; x <= 16; ++x) {
            for (i32 z = -16; z <= 16; ++z) {
                scene.place(x, 63, z, block::BlockId(mcver::Block::Stone), 0);
            }
        }
    }

    // Walks `walk` from x = 0.5 to `toX` in steps of `perTick`, counting the
    // steps it pays out.
    int stroll(RemoteWalk& walk, double toX, double perTick, bool crouched = false,
               bool riding = false)
    {
        int steps = 0;
        i32 bx = 0;
        int by = 0;
        i32 bz = 0;
        for (double x = 0.5; x <= toX + 1e-9; x += perTick) {
            if (walk.step(scene.w(), x, 64.0, 0.5, crouched, riding, &bx, &by, &bz)) {
                ++steps;
                lastY = by;
            }
        }
        return steps;
    }
};

}  // namespace

TEST(a_guest_walking_pays_out_a_step_every_block_and_two_thirds)
{
    // `distanceWalkedModified` grows at 0.6 of the ground covered and a step is
    // each whole unit of it past `nextStepDistance`'s starting 1, so 9.75
    // blocks (5.85 of it) is five steps.
    Floor floor;
    RemoteWalk walk;
    CHECK_EQ(floor.stroll(walk, 10.25, 0.25), 5);
    CHECK_EQ(walk.nextStep, 6);
    CHECK_EQ(floor.lastY, 63);  // the cell underfoot, not the one the feet are in
}

TEST(a_crouching_guest_banks_the_distance_and_takes_no_step)
{
    // As at home: the distance is counted while crouched and paid out the
    // moment they stand up, one step a tick.
    Floor floor;
    RemoteWalk walk;
    CHECK_EQ(floor.stroll(walk, 10.25, 0.25, true), 0);
    i32 bx = 0;
    int by = 0;
    i32 bz = 0;
    CHECK(walk.step(floor.scene.w(), 10.5, 64.0, 0.5, false, false, &bx, &by, &bz));
}

TEST(a_riding_guest_takes_no_step_and_banks_nothing)
{
    Floor floor;
    RemoteWalk walk;
    CHECK_EQ(floor.stroll(walk, 10.5, 0.25, false, true), 0);
    CHECK(walk.walked == 0.0f);
}

TEST(a_respawn_or_teleport_is_not_a_walk)
{
    // A host hears positions, not moves, so the jump to a spawn point would
    // otherwise be one enormous stride paid out a step a tick afterwards.
    Floor floor;
    RemoteWalk walk;
    i32 bx = 0;
    int by = 0;
    i32 bz = 0;
    CHECK(!walk.step(floor.scene.w(), 0.5, 64.0, 0.5, false, false, &bx, &by, &bz));
    CHECK(!walk.step(floor.scene.w(), 12.5, 64.0, 0.5, false, false, &bx, &by, &bz));
    CHECK(walk.walked == 0.0f);
}

TEST(nothing_underfoot_is_no_step)
{
    // `l > 0` in `moveEntity`: a body in the air over a gap banks its distance
    // and steps on nothing until it is over a block again.
    Floor floor;
    floor.scene.place(2, 63, 0, block::kAir, 0);
    floor.scene.place(3, 63, 0, block::kAir, 0);
    RemoteWalk walk;
    i32 bx = 0;
    int by = 0;
    i32 bz = 0;
    CHECK(!walk.step(floor.scene.w(), 0.5, 64.0, 0.5, false, false, &bx, &by, &bz));
    CHECK(!walk.step(floor.scene.w(), 2.5, 64.0, 0.5, false, false, &bx, &by, &bz));
    CHECK(!walk.step(floor.scene.w(), 3.5, 64.0, 0.5, false, false, &bx, &by, &bz));
    CHECK(walk.walked > 1.0f);
    CHECK(walk.step(floor.scene.w(), 4.5, 64.0, 0.5, false, false, &bx, &by, &bz));
    CHECK_EQ(bx, 4);
}
