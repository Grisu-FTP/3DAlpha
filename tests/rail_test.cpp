// Which way a rail lies once its neighbours have had their say.
//
// `mk` -- RailLogic -- is the largest single block behaviour in a1.1.2 and it
// had no implementation here at all: every rail lay north-south whatever was
// beside it, nothing curved, and the four ascending shapes were unreachable
// because `Block.onBlockPlaced` returns 0 for a rail and nothing else ever
// wrote its metadata.
//
// The cases below are the shape table read back out of the world: lay track,
// then ask what shape each piece settled on. That is the only way to check this
// -- the method has no return value and its whole output is metadata.

#include "core/block/registry.hpp"
#include "core/item/use.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/rail.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

constexpr int kFloorY = 63;
constexpr int kRailY = 64;

// A floor of stone with rails laid on it through the placement path, so that
// `blockAdded` runs and the shapes are worked out the way the game works them
// out rather than being written down.
struct Track {
    SceneWorld scene{0, 0};

    Track()
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 8; ++z) {
                scene.place(x, kFloorY, z, bid(mcver::Block::Stone), 0);
            }
        }
    }

    tick::TickWorld& w() { return scene.w(); }

    // `setBlockAndDataWithNotify` is what a placement does, and it is what runs
    // `blockAdded` -- which for a rail is the whole of the behaviour.
    void lay(i32 x, i32 z, int y = kRailY)
    {
        scene.w().setBlockAndDataWithNotify(x, y, z, bid(mcver::Block::Rail), 0);
    }

    int shape(i32 x, i32 z, int y = kRailY) { return int(scene.w().dataAt(x, y, z)); }
    int id(i32 x, int y, i32 z) { return int(scene.w().blockAt(x, y, z)); }
};

}  // namespace

TEST(a_lone_rail_lies_along_z)
{
    // Nothing to join, so the shape falls through every branch to the `if
    // (shape < 0) shape = 0` at the end. 0 is north-south, which is what a
    // single rail looks like in the original.
    Track t;
    t.lay(0, 0);
    CHECK_EQ(t.shape(0, 0), 0);
}

TEST(two_rails_side_by_side_along_x_both_lie_along_x)
{
    // The second one sees the first and takes shape 1; the first is
    // re-pointed by `connectTo`, which is the half of the method that stops the
    // two ends of a join disagreeing.
    Track t;
    t.lay(0, 0);
    t.lay(1, 0);
    CHECK_EQ(t.shape(0, 0), 1);
    CHECK_EQ(t.shape(1, 0), 1);
}

TEST(two_rails_along_z_both_lie_along_z)
{
    Track t;
    t.lay(0, 0);
    t.lay(0, 1);
    CHECK_EQ(t.shape(0, 0), 0);
    CHECK_EQ(t.shape(0, 1), 0);
}

TEST(a_corner_curves)
{
    // A rail with one neighbour on +x and one on +z is shape 6, which is the
    // curve joining those two sides. The three other corners are 7, 8 and 9,
    // and getting the pairs the wrong way round is the classic transcription
    // slip here -- so all four are checked.
    Track t;
    t.lay(0, 0);
    t.lay(1, 0);
    t.lay(0, 1);
    CHECK_EQ(t.shape(0, 0), 6);

    Track t7;
    t7.lay(0, 0);
    t7.lay(-1, 0);
    t7.lay(0, 1);
    CHECK_EQ(t7.shape(0, 0), 7);

    Track t8;
    t8.lay(0, 0);
    t8.lay(-1, 0);
    t8.lay(0, -1);
    CHECK_EQ(t8.shape(0, 0), 8);

    Track t9;
    t9.lay(0, 0);
    t9.lay(1, 0);
    t9.lay(0, -1);
    CHECK_EQ(t9.shape(0, 0), 9);
}

TEST(a_rail_climbs_towards_the_one_above_it)
{
    // 2 ascends towards +x and 3 towards -x; 4 towards -z and 5 towards +z.
    // The renderer had 2 and 3 the other way round until this landed, so a
    // staircase of rails climbed backwards.
    Track east;
    east.scene.place(1, kRailY, 0, bid(mcver::Block::Stone), 0);
    east.lay(0, 0);
    east.lay(1, 0, kRailY + 1);
    CHECK_EQ(east.shape(0, 0), 2);

    Track west;
    west.scene.place(-1, kRailY, 0, bid(mcver::Block::Stone), 0);
    west.lay(0, 0);
    west.lay(-1, 0, kRailY + 1);
    CHECK_EQ(west.shape(0, 0), 3);

    Track north;
    north.scene.place(0, kRailY, -1, bid(mcver::Block::Stone), 0);
    north.lay(0, 0);
    north.lay(0, -1, kRailY + 1);
    CHECK_EQ(north.shape(0, 0), 4);

    Track south;
    south.scene.place(0, kRailY, 1, bid(mcver::Block::Stone), 0);
    south.lay(0, 0);
    south.lay(0, 1, kRailY + 1);
    CHECK_EQ(south.shape(0, 0), 5);
}

TEST(a_long_run_is_straight_all_the_way)
{
    // Laid one after another, which is how a player lays one: every piece
    // re-points the one behind it, so the run has to end up straight rather
    // than a chain of corners.
    Track t;
    for (i32 x = -4; x <= 4; ++x) {
        t.lay(x, 0);
    }
    for (i32 x = -3; x <= 3; ++x) {
        CHECK_EQ(t.shape(x, 0), 1);
    }
}

TEST(a_rail_whose_floor_goes_falls_and_leaves_a_rail_behind)
{
    // `if.a(Lcn;IIII)V`: no opaque cube under it, so it drops itself and goes.
    // The drop is counted rather than seen -- there is no entity pool in a test
    // world -- but the removal is the visible half.
    Track t;
    t.lay(0, 0);
    CHECK_EQ(t.id(0, kRailY, 0), int(mcver::Block::Rail));
    t.w().setBlockWithNotify(0, kFloorY, 0, block::kAir);
    CHECK_EQ(t.id(0, kRailY, 0), int(mcver::Block::Air));
}

TEST(an_ascending_rail_needs_the_block_it_leans_on)
{
    // The other four lines of `onNeighborBlockChange`: an ascending rail also
    // wants an opaque cube under the *neighbour it climbs towards*.
    Track t;
    t.scene.place(1, kRailY, 0, bid(mcver::Block::Stone), 0);
    t.lay(0, 0);
    t.lay(1, 0, kRailY + 1);
    CHECK_EQ(t.shape(0, 0), 2);

    // Take the step away and the ascending rail has nothing to lean on.
    t.w().setBlockWithNotify(1, kRailY, 0, block::kAir);
    CHECK_EQ(t.id(0, kRailY, 0), int(mcver::Block::Air));
}

TEST(a_freshly_placed_rail_is_not_left_holding_metadata_fifteen)
{
    // `if.e` writes 15 before it refreshes, which is a value the game never
    // draws -- `setBasicRail` matches none of its ten cases, which is the
    // point. If the refresh ever stopped running, this is what would be left.
    Track t;
    t.lay(0, 0);
    CHECK(t.shape(0, 0) < 10);
}
