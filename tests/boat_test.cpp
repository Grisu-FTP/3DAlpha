// The boat -- `dc` (EntityBoat) and `me.a(...)` (ItemBoat).
//
// "Boats and minecarts don't work (not even placeable)" had two halves for the
// boat. The first is the one paintings had: nowhere to put an entity, so the
// click did everything and produced nothing. The second is specific and is the
// reason a boat could never have gone down the block-placement path at all:
// `ItemBoat` is an `onItemRightClick` that casts **its own ray, at reach 5.0,
// with liquids on**. The crosshair's ray is not allowed to see water, and water
// is the only place a boat is any use.
//
// What is checkable without a screen: buoyancy, the speed cap, the steering,
// breaking on a wall, and the seat.

#include "core/block/registry.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/player_body.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/boat_mesh.hpp"
#include "core/render/entity_range.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Boat;
using mc::entity::BoatSystem;
using mc::entity::VehicleRider;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

item::ItemId boatItem()
{
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Boat) {
            return item::ItemId(id);
        }
    }
    return 0;
}

// A pond: stone floor at y = 62, still water filling y = 63, air above.
struct Pond {
    SceneWorld scene{0, 0};
    BoatSystem boats{99};

    Pond()
    {
        for (i32 x = -30; x <= 30; ++x) {
            for (i32 z = -30; z <= 30; ++z) {
                scene.place(x, 62, z, bid(mcver::Block::Stone), 0);
                scene.place(x, 63, z, bid(mcver::Block::Water), 0);
            }
        }
    }

    tick::TickWorld& w() { return scene.w(); }

    void run(int ticks, const VehicleRider& rider = VehicleRider{})
    {
        for (int i = 0; i < ticks; ++i) {
            boats.tick(w(), rider);
        }
    }
};

}  // namespace

TEST(the_boat_item_is_the_one_the_table_says_spawns_a_boat)
{
    int found = 0;
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Boat) {
            ++found;
        }
    }
    CHECK_EQ(found, 1);
    CHECK(boatItem() != 0);
}

TEST(a_boat_placed_on_water_floats_rather_than_sinking_or_flying)
{
    Pond pond;
    // `me.a(...)` spawns at (x + 0.5, y + 1.5, z + 0.5) of the block its ray
    // hit -- the water cell at y = 63.
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK_EQ(pond.boats.count(), 1);

    pond.run(200);
    CHECK_EQ(pond.boats.count(), 1);

    const Boat& b = pond.boats[0];
    // It settles around the water's surface. The buoyancy has no water level
    // in it at all -- it is five slices of the hull asked whether they are wet
    // -- so "floats" here means it neither sank to the floor nor climbed away.
    CHECK(b.y > 63.0);
    CHECK(b.y < 66.0);
    CHECK(std::fabs(b.motionY) < 0.1);
}

TEST(a_boat_out_of_water_falls)
{
    // No water at all: every slice is dry, the buoyancy term is -1, and the
    // boat falls at 0.04 a tick until it lands.
    SceneWorld scene{0, 0};
    for (i32 x = -6; x <= 6; ++x) {
        for (i32 z = -6; z <= 6; ++z) {
            scene.place(x, 62, z, bid(mcver::Block::Stone), 0);
        }
    }
    BoatSystem boats{3};
    CHECK(boats.place(scene.w(), 0, 70, 0));
    const double start = boats[0].y;
    for (int i = 0; i < 200; ++i) {
        boats.tick(scene.w(), VehicleRider{});
    }
    CHECK(boats[0].y < start);
    CHECK(boats[0].y < 64.0);
    CHECK(boats[0].onGround);
}

TEST(a_rider_pushes_the_boat_and_the_cap_holds)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(pond.boats.mount(0));

    // A rider holding forward settles near 0.22 of motion under the 0.91 air
    // friction; the boat takes a fifth of that per tick and drags at 0.99, so
    // it pins at its own cap.
    VehicleRider rider;
    rider.present = true;
    rider.motionZ = 0.22;
    pond.run(200, rider);

    CHECK_EQ(pond.boats.count(), 1);
    const Boat& b = pond.boats[0];
    CHECK(b.motionZ > 0.3);
    // **Never past the cap**, which is what makes a boat feel like a boat.
    CHECK(b.motionZ <= entity::kBoatSpeedCap + 1e-9);
    CHECK(b.z > 5.0);
}

TEST(an_empty_boat_does_not_steer_itself)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    // A rider that is not present contributes nothing, even if the numbers are
    // set -- which is the guard that stops a dismounted player still driving.
    VehicleRider ghost;
    ghost.present = false;
    ghost.motionZ = 0.5;
    pond.run(60, ghost);
    CHECK(std::fabs(pond.boats[0].motionZ) < 1e-6);
}

TEST(the_seat_is_under_the_boat_by_three_tenths)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(!pond.boats.seat().valid);
    CHECK(pond.boats.mount(0));

    const entity::RiderSeat seat = pond.boats.seat();
    CHECK(seat.valid);
    CHECK_EQ(seat.x, pond.boats[0].x);
    CHECK_EQ(seat.z, pond.boats[0].z);
    // `getMountedYOffset()` is `height * 0.0D - 0.3D`, which is a constant with
    // the height multiplied out by zero.
    CHECK(std::fabs(seat.y - (pond.boats[0].y + entity::kBoatMountedYOffset)) < 1e-12);
}

TEST(a_rider_sits_in_the_boat_rather_than_under_or_above_it)
{
    // The arithmetic in `updateRidden` cancels -- 1.62 goes into `setPosition`
    // and comes straight back out of the box -- so the feet land exactly on the
    // seat. Getting it wrong by the eye height is the easy mistake and puts the
    // camera in the water.
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(pond.boats.mount(0));
    const entity::RiderSeat seat = pond.boats.seat();

    entity::PlayerBody body;
    entity::PlayerInput input;
    body.tickRiding(input, seat.x, seat.y, seat.z);

    CHECK(std::fabs(body.y - seat.y) < 1e-12);
    CHECK(std::fabs(body.eyeY() - (seat.y + 1.62)) < 1e-6);
    CHECK(std::fabs(body.x - seat.x) < 1e-12);
}

TEST(only_one_thing_may_ride_a_boat)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(pond.boats.place(pond.w(), 4, 63, 0));
    CHECK(pond.boats.mount(0));
    CHECK_EQ(pond.boats.riddenIndex(), 0);
    // The same boat again is refused, which is `EntityBoat.interact`'s only
    // rule.
    CHECK(!pond.boats.mount(0));
    // A different one is not: mounting it leaves the first.
    CHECK(pond.boats.mount(1));
    CHECK_EQ(pond.boats.riddenIndex(), 1);
    CHECK(!pond.boats[0].ridden);

    pond.boats.dismount();
    CHECK_EQ(pond.boats.riddenIndex(), -1);
    CHECK(!pond.boats.seat().valid);
}

TEST(a_boat_driven_into_a_wall_fast_enough_breaks)
{
    Pond pond;
    // A wall across the pond.
    for (i32 x = -30; x <= 30; ++x) {
        for (int y = 63; y <= 66; ++y) {
            pond.scene.place(x, y, 10, bid(mcver::Block::Stone), 0);
        }
    }
    mc::test::DropCatcher caught;
    caught.watch(pond.w());
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(pond.boats.mount(0));

    // **Driven at an angle, and that is not a detail of the test.** The break
    // test is `isCollidedHorizontally && speed > 0.15`, and `speed` is measured
    // *after* `moveEntity` -- which zeroes the motion on whichever axis was
    // clipped. So a boat driven perfectly square into a wall has its motionZ
    // set to zero by the collision and measures a speed of nothing: **a
    // head-on boat does not break in a1.1.2 either.** What breaks one is
    // hitting at an angle, where the un-collided axis survives the move and is
    // still above the threshold. That is why boats break on shores rather than
    // on flat walls.
    VehicleRider rider;
    rider.present = true;
    rider.motionX = 0.22;
    rider.motionZ = 0.22;
    for (int i = 0; i < 200 && pond.boats.count() > 0; ++i) {
        pond.boats.tick(pond.w(), rider);
    }
    CHECK_EQ(pond.boats.count(), 0);
    // ...and the rider is put back on their feet, because the pool clears the
    // seat when the boat it belonged to goes.
    CHECK_EQ(pond.boats.riddenIndex(), -1);
    // The wall and the hand leave the same wreckage, through the same helper.
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Stick)), entity::kBoatSticksDropped);
}

TEST(a_boat_nudged_into_a_wall_slowly_survives)
{
    Pond pond;
    for (i32 x = -30; x <= 30; ++x) {
        for (int y = 63; y <= 66; ++y) {
            pond.scene.place(x, y, 4, bid(mcver::Block::Stone), 0);
        }
    }
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    // Below the 0.15 threshold, so it bumps and stops.
    VehicleRider rider;
    rider.present = true;
    rider.motionZ = 0.01;
    CHECK(pond.boats.mount(0));
    pond.run(120, rider);
    CHECK_EQ(pond.boats.count(), 1);
}

TEST(there_is_no_boat_limit)
{
    Pond pond;
    const int many = BoatSystem::kInitialCapacity * 4;
    for (int i = 0; i < many; ++i) {
        CHECK(pond.boats.place(pond.w(), i32(i), 63, 0));
    }
    CHECK_EQ(pond.boats.count(), many);
    CHECK_EQ(int(pond.boats.refused()), 0);
}

TEST(a_right_click_with_a_boat_rays_through_water)
{
    // The seam, and the half that is specific to boats: the item's own ray
    // has to see the water, which the crosshair's never does. Aimed straight
    // down at a pond from above.
    Pond pond;

    // Within the boat's own reach, which is 5.0 and not the hand's 4.0 -- an
    // eye seven blocks up would be out of range and the ray would find nothing.
    item::Effects none;
    item::ItemUse used =
        item::useItem(pond.w(), boatItem(), 0.5, 66.5, 0.5, 0.0, -1.0, 0.0, none);
    CHECK(!used.changed);
    CHECK_EQ(pond.boats.count(), 0);

    item::Effects effects;
    effects.entities.boats = &pond.boats;
    used = item::useItem(pond.w(), boatItem(), 0.5, 66.5, 0.5, 0.0, -1.0, 0.0, effects);
    CHECK(used.changed);
    CHECK_EQ(pond.boats.count(), 1);
    // It landed on the water rather than passing through to the stone.
    CHECK(pond.boats[0].y > 63.0);
}

TEST(the_boat_meshes_into_five_boxes)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));

    mesh::DetailVertex verts[render::kBoatMaxVertices];
    const int written = render::buildBoats(pond.boats, 0.0, 64.0, 0.0, 0.0f, verts,
                                           render::kBoatMaxVertices);
    CHECK_EQ(written, render::kBoatVerticesEach);

    // A boat is 1.5 blocks across. Its geometry should be about that and not
    // sixteen times it, which is what a missing model-unit scale looks like.
    double lo[3] = {1e30, 1e30, 1e30};
    double hi[3] = {-1e30, -1e30, -1e30};
    for (int i = 0; i < written; ++i) {
        const double p[3] = {double(verts[i].x) / double(render::kEntityUnitsPerBlock),
                             double(verts[i].y) / double(render::kEntityUnitsPerBlock),
                             double(verts[i].z) / double(render::kEntityUnitsPerBlock)};
        for (int a = 0; a < 3; ++a) {
            lo[a] = p[a] < lo[a] ? p[a] : lo[a];
            hi[a] = p[a] > hi[a] ? p[a] : hi[a];
        }
    }
    CHECK(hi[0] - lo[0] > 1.0);
    CHECK(hi[0] - lo[0] < 2.0);
    CHECK(hi[2] - lo[2] > 1.0);
    CHECK(hi[2] - lo[2] < 2.0);
    // ...and it is a boat, not a cube: much wider than it is tall.
    CHECK(hi[1] - lo[1] < 1.0);
}

TEST(every_boat_uv_lands_inside_the_boat_page)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));

    mesh::DetailVertex verts[render::kBoatMaxVertices];
    const int written = render::buildBoats(pond.boats, 0.0, 64.0, 0.0, 0.0f, verts,
                                           render::kBoatMaxVertices);
    CHECK(written > 0);

    int ox = 0, oy = 0;
    texture::skinOrigin(texture::EntitySkin::Boat, &ox, &oy);
    for (int i = 0; i < written; ++i) {
        const double u = double(verts[i].u) * texture::kEntitySheetWidth
                         / double(mesh::kUvUnitsPerAtlas);
        const double v = double(verts[i].v) * texture::kEntitySheetHeight
                         / double(mesh::kUvUnitsPerAtlas);
        CHECK(u >= double(ox) - 0.01);
        CHECK(u <= double(ox) + double(texture::kSkinPageWidth) + 0.01);
        CHECK(v >= double(oy) - 0.01);
        CHECK(v <= double(oy) + double(texture::kSkinPageHeight) + 0.01);
    }
}

TEST(a_boat_picked_by_a_ray_is_the_one_looked_at)
{
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(pond.boats.place(pond.w(), 10, 63, 0));

    item::EntityPools pools;
    pools.boats = &pond.boats;
    const entity::RayHit noBlock{};
    const double y = pond.boats[0].y;

    // Looking along +X from the origin finds the near one.
    const item::EntityTarget near =
        item::pickEntity(pools, -2.0, y, 0.5, 1.0, 0.0, 0.0, noBlock);
    CHECK(near.kind == item::EntityTarget::Kind::Boat);
    CHECK_EQ(near.index, 0);
    // Looking the other way finds nothing within reach.
    CHECK(!item::pickEntity(pools, -2.0, y, 0.5, -1.0, 0.0, 0.0, noBlock).found());
    // And the far one is out of the entity pick's three blocks.
    CHECK(!item::pickEntity(pools, 6.5, y, 0.5, 1.0, 0.0, 0.0, noBlock).found());
    CHECK(item::pickEntity(pools, 7.5, y, 0.5, 1.0, 0.0, 0.0, noBlock).index == 1);
}

// ---------------------------------------------------------------------------
// Breaking one by hand
// ---------------------------------------------------------------------------

TEST(five_bare_handed_hits_break_a_boat_and_it_leaves_planks_and_sticks)
{
    // `dc.a(Lkh;I)Z`: `damage += i * 10`, break past 40. An empty hand is worth
    // 1, so four hits leave the boat afloat at 40 and the fifth takes it to 50.
    // Nothing damaged a boat at all before this, which is why one could not be
    // broken and therefore never dropped anything.
    Pond pond;
    mc::test::DropCatcher caught;
    caught.watch(pond.w());
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK_EQ(pond.boats.count(), 1);

    for (int hit = 0; hit < 4; ++hit) {
        CHECK(pond.boats.attack(pond.w(), 0, 1));
        CHECK_EQ(pond.boats.count(), 1);
        CHECK_EQ(int(caught.drops.size()), 0);
    }
    CHECK(pond.boats.attack(pond.w(), 0, 1));
    CHECK_EQ(pond.boats.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Stick)), entity::kBoatSticksDropped);
}

TEST(a_hit_rocks_the_hull_before_it_breaks_it)
{
    // The two fields the renderer reads: `forwardDirection` flips and
    // `timeSinceHit` goes to 10. They were transcribed and never written to.
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    const int before = pond.boats[0].forwardDirection;
    CHECK(pond.boats.attack(pond.w(), 0, 1));
    CHECK_EQ(pond.boats[0].forwardDirection, -before);
    CHECK_EQ(pond.boats[0].timeSinceHit, entity::kVehicleHitTime);
    CHECK_EQ(pond.boats[0].damage, entity::kVehicleDamageScale);
}

TEST(one_hard_enough_hit_breaks_a_boat_outright)
{
    // A sword is worth more than a fist in the original and there is no
    // `damageVsEntity` column yet -- but the arithmetic that would use one is
    // here, and this is the check that it is the argument and not a counter of
    // presses that decides.
    Pond pond;
    mc::test::DropCatcher caught;
    caught.watch(pond.w());
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    CHECK(pond.boats.attack(pond.w(), 0, 5));
    CHECK_EQ(pond.boats.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);
}

TEST(attacking_a_boat_that_is_not_there_does_nothing)
{
    Pond pond;
    CHECK(!pond.boats.attack(pond.w(), 0, 1));
    CHECK(!pond.boats.attack(pond.w(), -1, 1));
}

TEST(a_boat_is_drawn_to_seventy_seven_blocks)
{
    // `kh.a(D)Z` of a 1.5 x 0.6 x 1.5 box is 1.2 * 64 = 76.8 blocks -- past the
    // 31.25 a detail vertex reaches, which is why the pass writes 1/256 of a
    // block. The origin moves rather than the boat.
    Pond pond;
    CHECK(pond.boats.place(pond.w(), 0, 63, 0));
    const double y = pond.boats[0].y;

    static mesh::DetailVertex verts[render::kBoatMaxVertices];
    const auto from = [&](double originX) {
        return render::buildBoats(pond.boats, originX, y, 0.5, 0.0f, verts,
                                  render::kBoatMaxVertices);
    };
    CHECK_EQ(from(-60.0), render::kBoatVerticesEach);
    // Where it was drawn is where it is: sixty blocks out, not wrapped round.
    double lo = 1e30;
    for (int i = 0; i < render::kBoatVerticesEach; ++i) {
        const double x = double(verts[i].x) / double(render::kEntityUnitsPerBlock);
        lo = x < lo ? x : lo;
    }
    CHECK(lo > 59.0 && lo < 61.0);
    CHECK_EQ(from(-78.0), 0);
}
