// **What a river does to whatever is standing in it** --
// `cn.a(cf,gb,kh)Z` (World.handleMaterialAcceleration) and the flow field
// under it, `jp.e(nm,III)`.
//
// The flow field itself has been in this project since the mesher needed it:
// it is what spins a flowing block's top texture, and `mesher_test.cpp` pins
// the *angle* it produces. What was missing was the other caller. The vector
// is summed over every water cell an entity's box touches, normalised, and
// four thousandths of it added to the motion every tick -- so a player in a
// stream drifts downstream without pressing anything, and a dropped item goes
// with them.
//
// Both readers now go through core/block/fluid_flow.hpp, which is why the
// angle tests in the mesher suite and these push tests are pinning one
// transcription rather than two.
//
// **The numbers here are geometric, not oracles.** There is no JVM vector to
// compare against without a running client; what is checkable is the direction
// (downstream, which is toward the *higher* decay), the magnitude (0.004 of a
// unit vector, whatever the box), and the cases that must produce nothing at
// all -- still water, and water too shallow to reach the inset probe.

#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/player_body.hpp"
#include "core/item/creative_palette.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::PlayerBody;
using mc::entity::PlayerInput;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;

// The push, once, on a motion that starts at rest.
constexpr double kPush = 0.0040000000000000001;

// A stone floor with a stream running along +x on top of it: a source at x = 0
// and flowing cells whose decay climbs by one each block, which is exactly the
// shape `BlockFluid` leaves behind when a source spills across flat ground.
struct Stream {
    SceneWorld world{0, 0};

    Stream()
    {
        for (i32 x = -4; x <= 12; ++x) {
            for (i32 z = -4; z <= 4; ++z) {
                world.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
        world.place(0, 64, 0, bid(mcver::Block::Water), 0);
        for (i32 x = 1; x <= 6; ++x) {
            world.place(x, 64, 0, bid(mcver::Block::FlowingWater), u8(x));
        }
    }

    // A lake instead: seven by seven of source blocks, which is water that is
    // not going anywhere.
    void makeStill()
    {
        for (i32 x = -3; x <= 3; ++x) {
            for (i32 z = -3; z <= 3; ++z) {
                world.place(x, 64, z, bid(mcver::Block::Water), 0);
            }
        }
    }
};

AABB bodyBoxAt(double x, double feetY, double z)
{
    return AABB{x - 0.3, feetY, z - 0.3, x + 0.3, feetY + 1.8, z + 0.3};
}

}  // namespace

TEST(a_flowing_stream_pushes_downstream)
{
    Stream s;
    double mx = 0.0, my = 0.0, mz = 0.0;
    const bool inWater = block::handleWaterMovement(s.world.w(), bodyBoxAt(2.5, 64.0, 0.5),
                                                    kWaterMaterial, &mx, &my, &mz);
    CHECK(inWater);

    // Downstream is toward the higher decay, which is +x here.
    CHECK(mx > 0.0);
    CHECK_EQ(mz, 0.0);
}

TEST(the_push_is_four_thousandths_whatever_the_box)
{
    // The sum of the cells' unit vectors is normalised a *second* time, so
    // what the cells decide is the direction and never the speed. A box
    // straddling three water cells is pushed exactly as hard as one inside a
    // single cell.
    Stream s;

    double nx = 0.0, ny = 0.0, nz = 0.0;
    block::handleWaterMovement(s.world.w(), bodyBoxAt(2.5, 64.0, 0.5), kWaterMaterial, &nx,
                               &ny, &nz);

    double wx = 0.0, wy = 0.0, wz = 0.0;
    const AABB wide{1.1, 64.0, 0.2, 3.9, 65.8, 0.8};
    block::handleWaterMovement(s.world.w(), wide, kWaterMaterial, &wx, &wy, &wz);

    const double narrow = std::sqrt(nx * nx + ny * ny + nz * nz);
    const double broad = std::sqrt(wx * wx + wy * wy + wz * wz);
    CHECK(std::fabs(narrow - kPush) < 1e-12);
    CHECK(std::fabs(broad - kPush) < 1e-12);
}

TEST(a_still_lake_does_not_push_anything)
{
    // Every neighbour has the same decay, so every weight is zero and the sum
    // normalises to nothing. Without the `lengthVector() > 0` guard this would
    // be a division by zero rather than a still lake.
    Stream s;
    s.makeStill();

    double mx = 0.0, my = 0.0, mz = 0.0;
    const bool inWater = block::handleWaterMovement(s.world.w(), bodyBoxAt(0.5, 64.0, 0.5),
                                                    kWaterMaterial, &mx, &my, &mz);
    CHECK(inWater);
    CHECK_EQ(mx, 0.0);
    CHECK_EQ(my, 0.0);
    CHECK_EQ(mz, 0.0);
}

TEST(a_body_standing_beside_the_stream_is_not_touched_by_it)
{
    Stream s;
    double mx = 0.0, my = 0.0, mz = 0.0;
    const bool inWater = block::handleWaterMovement(s.world.w(), bodyBoxAt(2.5, 64.0, 2.5),
                                                    kWaterMaterial, &mx, &my, &mz);
    CHECK(!inWater);
    CHECK_EQ(mx, 0.0);
    CHECK_EQ(mz, 0.0);
}

TEST(lava_is_not_asked_for_a_push_at_all)
{
    // `kh.G()` -- handleLavaMovement -- is `isMaterialInBB`, which has no
    // vector in it. A player in a lava fall in a1.1.2 sinks straight down.
    Stream s;
    for (i32 x = 0; x <= 4; ++x) {
        s.world.place(x, 64, 2, bid(mcver::Block::FlowingLava), u8(x));
    }

    double mx = 0.0, my = 0.0, mz = 0.0;
    const bool inWater = block::handleWaterMovement(s.world.w(), bodyBoxAt(2.5, 64.0, 2.5),
                                                    kWaterMaterial, &mx, &my, &mz);
    CHECK(!inWater);
    CHECK_EQ(mx, 0.0);
}

TEST(a_player_left_alone_in_a_stream_drifts_downstream)
{
    // The whole thing through `PlayerBody::tick`: the swimming branch takes
    // the push as its starting motion and the 0.8 drag keeps it bounded, so a
    // player who touches nothing ends up further down the stream than they
    // started.
    Stream s;
    PlayerBody body;
    body.setFeet(2.5, 64.0, 0.5);
    const double startX = body.x;
    const double startZ = body.z;

    PlayerInput input;
    for (int i = 0; i < 100; ++i) {
        body.tick(s.world.w(), input);
    }

    CHECK(body.x > startX);
    CHECK(std::fabs(body.z - startZ) < 1e-9);
}

TEST(a_player_left_alone_in_a_lake_stays_put)
{
    Stream s;
    s.makeStill();
    PlayerBody body;
    body.setFeet(0.5, 64.0, 0.5);
    const double startX = body.x;
    const double startZ = body.z;

    PlayerInput input;
    for (int i = 0; i < 100; ++i) {
        body.tick(s.world.w(), input);
    }

    CHECK_EQ(body.x, startX);
    CHECK_EQ(body.z, startZ);
}

TEST(a_dropped_item_in_a_stream_drifts_too)
{
    // `dx.e_()` calls `handleWaterMovement()` and throws its answer away: the
    // entity wants the push and nothing else. This was the gap named in
    // core/entity/item_entity.hpp.
    Stream s;
    entity::ItemEntitySystem items{99};
    CHECK(items.spawn(s.world.w(), 2.5, 64.4, 0.5, item::paletteItem(0), 1, 0));
    const double startX = items[0].x;

    for (int i = 0; i < 200; ++i) {
        items.tick(s.world.w());
    }

    CHECK_EQ(items.count(), 1);
    CHECK(items[0].x > startX);
}
