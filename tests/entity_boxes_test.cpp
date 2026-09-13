// **The entity half of `cn.a(Lkh;Lcf;)Ljava/util/List;`** -- both of its
// questions: `kh.f_()` (getBoundingBox, asked of the neighbour) and `kh.b_(kh)`
// (getCollisionBox, asked of the mover).
//
// This suite exists because of a report: "in alpha 1.1.2 you can stand on top
// of a minecart, in this you can't." The sweep here was the block loop of
// `getCollidingBoundingBoxes` and nothing else, so every entity in the world
// was scenery. In the jar the same method then asks each nearby entity two
// things. Exactly two classes answer the first with a box -- `dc` (EntityBoat)
// and `oc` (EntityMinecart), which is why a cow is walked through and a cart is
// not -- and those same two are the only ones that answer the second, with the
// *neighbour's* box whatever the neighbour is, which is why a cart is stopped
// by the cow standing on the track.
//
// What is checkable without a screen: a body lands on a cart's roof rather than
// through it, a body is stopped by one sideways, a cart still moves with the
// query installed -- it must not find its own box in the way -- getting off
// leaves the player standing on the roof, which is `mountEntity`'s own tail,
// and a rolling cart or boat stops at a mob, an item or a player while a
// walking player passes straight through all three.

#include "core/block/registry.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/entity_boxes.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/mob.hpp"
#include "core/item/creative_palette.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Boat;
using mc::entity::BoatSystem;
using mc::entity::Minecart;
using mc::entity::MinecartSystem;
using mc::entity::MinecartType;
using mc::entity::PlayerBody;
using mc::entity::PlayerInput;
using mc::entity::EntityBoxes;
using mc::entity::ItemEntitySystem;
using mc::entity::MobSystem;
using mc::entity::MobType;
using mc::entity::VehicleRider;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A straight rail on a stone bed at y = 64, the pools a1.1.2 keeps in its world
// entity list, and a body -- with the entity query bound the way the frame loop
// binds it.
struct Yard {
    SceneWorld scene{0, 0};
    MinecartSystem carts{4242};
    BoatSystem boats{4242};
    MobSystem mobs{4242};
    ItemEntitySystem items{4242};
    PlayerBody body;
    EntityBoxes solid{&boats, &carts,   &mobs,   &items,
                      nullptr, nullptr, nullptr, nullptr, &body};

    explicit Yard(bool bindSolid = true)
    {
        for (i32 z = -8; z <= 8; ++z) {
            for (i32 x = -8; x <= 8; ++x) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
            scene.place(0, 64, z, bid(mcver::Block::Rail), 0);
        }
        if (bindSolid) {
            mc::entity::bindEntityBoxes(w(), &solid);
        }
    }

    tick::TickWorld& w() { return scene.w(); }

    void runCarts(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            carts.tick(w(), VehicleRider{});
        }
    }

    void runBody(int ticks, const PlayerInput& input = PlayerInput{})
    {
        for (int i = 0; i < ticks; ++i) {
            body.tick(w(), input);
        }
    }
};

}  // namespace

TEST(a_body_dropped_on_a_minecart_lands_on_its_roof)
{
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    // Let the cart settle onto the rail: its height is looked up, not
    // integrated, so this is where the roof really is.
    yard.runCarts(4);
    const double roof = yard.carts[0].box.maxY;

    yard.body.setFeet(0.5, roof + 2.0, 0.5);
    yard.runBody(40);

    CHECK(yard.body.onGround);
    CHECK(std::fabs(yard.body.y - roof) < 1e-9);
    // Not resting on the stone bed, which is where it used to end up.
    CHECK(yard.body.y > 64.5);
}

TEST(without_the_query_a_body_falls_straight_through_a_cart)
{
    // The bug, kept as the contrast: the block sweep alone is what this had,
    // and it is also what every headless tool still gets.
    Yard yard{false};
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    yard.runCarts(4);
    const double roof = yard.carts[0].box.maxY;

    yard.body.setFeet(0.5, roof + 2.0, 0.5);
    yard.runBody(40);

    CHECK(yard.body.onGround);
    // Down on the stone, through the cart.
    CHECK(std::fabs(yard.body.y - 64.0) < 1e-9);
}

TEST(a_cart_stops_a_body_walking_into_it)
{
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    yard.runCarts(4);

    // Standing on the stone two blocks down the track, walking towards the
    // cart along +z. The cart's roof is 0.7 above the bed and a step is 0.5,
    // so this is a wall and not a kerb.
    yard.body.setFeet(0.5, 64.0, -2.0);
    PlayerInput input;
    input.forward = 1.0f;
    // `moveFlying` sends `forward` along `(-sin yaw, cos yaw)`, so a yaw of
    // zero walks towards +z.
    input.yawDegrees = 0.0f;
    yard.runBody(30, input);

    CHECK(yard.body.collidedHorizontally);
    // Stopped at the cart's near face rather than standing in the middle of it.
    CHECK(yard.body.z < yard.carts[0].box.minZ + 1e-9);
    CHECK(yard.body.z > yard.carts[0].box.minZ - 0.6);
    CHECK(std::fabs(yard.body.y - 64.0) < 1e-9);
}

TEST(a_cart_is_left_out_of_its_own_collision_list)
{
    // The exclusion `getCollidingBoundingBoxes`'s first argument is for. A cart
    // handed its own box finds it overlapping every swept volume it builds and
    // never moves at all -- so this is the same roll as the minecart suite's,
    // run with the query bound.
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    const_cast<Minecart&>(yard.carts[0]).motionZ = 0.3;
    yard.runCarts(60);

    CHECK(yard.carts[0].onRail);
    CHECK(yard.carts[0].z > 2.0);
}

TEST(a_cart_stops_behind_another_cart_on_the_same_track)
{
    // Two carts *are* in each other's lists, which is the other half of the
    // exclusion being by identity rather than by kind.
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    CHECK(yard.carts.place(yard.w(), 0, 64, 3, MinecartType::Rideable));
    const_cast<Minecart&>(yard.carts[0]).motionZ = 0.3;
    yard.runCarts(60);

    // The chaser never reaches the parked cart's near face.
    CHECK(yard.carts[0].box.maxZ <= yard.carts[1].box.minZ + 1e-9);
}

TEST(getting_off_a_cart_leaves_the_player_on_its_roof)
{
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    yard.runCarts(4);
    CHECK(yard.carts.mount(0));

    const entity::RiderSeat off = yard.carts.dismount();
    CHECK(off.valid);
    // `setLocationAndAngles(posX, boundingBox.minY + height, posZ)`, which is
    // the roof and not the seat -- the seat is three tenths *below* the cart.
    CHECK(std::fabs(off.y - yard.carts[0].box.maxY) < 1e-12);

    yard.body.setFeet(off.x, off.y, off.z);
    yard.runBody(20);
    CHECK(yard.body.onGround);
    CHECK(std::fabs(yard.body.y - yard.carts[0].box.maxY) < 1e-9);
}

TEST(a_boat_is_solid_too_and_a_mob_is_not)
{
    // `dc.f_()` is the same two lines as `oc.f_()`, so a boat parked on land
    // holds a body up exactly as a cart does. Nothing else in the jar
    // overrides the method -- the mobs and the items are in this suite's query
    // for the *other* branch, and a walking body never sees them.
    Yard yard;
    // `new dc(world, x + 0.5, y + 1.5, z + 0.5)` -- a boat placed on the block
    // at y = 63 floats a block and a half above its floor. Nothing ticks it
    // here, so it stays parked over the stone.
    CHECK(yard.boats.place(yard.w(), 4, 63, 4));
    const double roof = yard.boats[0].box.maxY;

    yard.body.setFeet(4.5, roof + 2.0, 4.5);
    yard.runBody(40);

    CHECK(yard.body.onGround);
    CHECK(std::fabs(yard.body.y - roof) < 1e-9);
}

// ---- `kh.b_(kh)` -- getCollisionBox, asked of the mover ------------------
//
// The second entry `getCollidingBoundingBoxes` adds per neighbour. `kh.b_` is
// null, so for a player, a mob or an item this branch is empty and nothing
// changes; `dc.b_` and `oc.b_` are `return e.boundingBox` with no test in
// front of them, so a moving boat or cart is stopped by everything near it.

TEST(a_pig_on_the_track_stops_a_rolling_cart)
{
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    yard.runCarts(4);
    // Standing on the rail three blocks along. A pig is 0.9 tall, so its box
    // reaches into the cart's -- which is the overlap `calculateOffset` needs
    // on the other two axes before it will clip the z at all.
    CHECK(yard.mobs.spawn(yard.w(), MobType::Pig, 0.5, 64.0, 3.5, 0.0f));
    const AABB pig = yard.mobs[0].body.box;

    const_cast<Minecart&>(yard.carts[0]).motionZ = 0.3;
    yard.runCarts(60);

    // It rolled, and it stopped at the pig's near face rather than through it.
    CHECK(yard.carts[0].z > 1.0);
    CHECK(yard.carts[0].box.maxZ <= pig.minZ + 1e-9);
    CHECK(yard.carts[0].box.maxZ > pig.minZ - 0.25);
}

TEST(a_pig_does_not_stop_a_walking_player)
{
    // The same pig, and the other side of the branch: `kh.b_` is null on a
    // player, so the pig is not in the player's collision list at all and the
    // player walks through it. This is the behaviour the first half must not
    // have broken by making entities collidable in general.
    Yard yard;
    CHECK(yard.mobs.spawn(yard.w(), MobType::Pig, 0.5, 64.0, 0.5, 0.0f));
    const AABB pig = yard.mobs[0].body.box;

    yard.body.setFeet(0.5, 64.0, -2.0);
    PlayerInput input;
    input.forward = 1.0f;
    input.yawDegrees = 0.0f;
    yard.runBody(30, input);

    CHECK(!yard.body.collidedHorizontally);
    CHECK(yard.body.z > pig.maxZ);
}

TEST(a_dropped_item_stops_a_rolling_cart)
{
    // Not only mobs: the branch adds whatever is in the world entity list, and
    // an item lying on the rail is in it.
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    yard.runCarts(4);
    CHECK(yard.items.spawn(yard.w(), 0.5, 64.6, 3.5, item::paletteItem(0), 1, 0));
    const AABB drop = yard.items[0].box;
    // The clip only happens if the boxes overlap on the other two axes, so
    // check the fixture really did put the item inside the cart's height.
    CHECK(drop.minY < yard.carts[0].box.maxY);
    CHECK(drop.maxY > yard.carts[0].box.minY);

    const_cast<Minecart&>(yard.carts[0]).motionZ = 0.3;
    yard.runCarts(60);

    CHECK(yard.carts[0].z > 1.0);
    CHECK(yard.carts[0].box.maxZ <= drop.minZ + 1e-9);
}

TEST(a_cart_is_not_stopped_by_its_own_rider)
{
    // `getEntitiesWithinAABBExcludingEntity` leaves out the mover and nothing
    // else -- not the rider -- so the player *is* in the moving cart's list.
    // It costs nothing, and the jar says why: `oc.h()` is `height * 0.0 - 0.3`,
    // so a rider's feet sit 0.3 below the cart's centre and its box overlaps
    // the cart's on all three axes. `calculateOffset` clips against a box that
    // is ahead and clear, never one already overlapping.
    Yard yard;
    CHECK(yard.carts.place(yard.w(), 0, 64, 0, MinecartType::Rideable));
    yard.runCarts(4);
    CHECK(yard.carts.mount(0));

    // Twenty ticks and not sixty: an occupied cart keeps its speed, and this
    // yard's track runs out at z = 8.
    const_cast<Minecart&>(yard.carts[0]).motionZ = 0.3;
    for (int i = 0; i < 20; ++i) {
        yard.carts.tick(yard.w(), VehicleRider{});
        // What the frame loop does with a rider: assign the position, never
        // move it. `updateRidden` and not `moveEntity`.
        const entity::RiderSeat seat = yard.carts.seat();
        CHECK(seat.valid);
        yard.body.setFeet(seat.x, seat.y, seat.z);
    }

    // The rider's box overlaps the cart's, which is what makes this a test.
    CHECK(yard.body.box.minY < yard.carts[0].box.maxY);
    CHECK(yard.body.box.maxY > yard.carts[0].box.minY);
    CHECK(yard.carts[0].onRail);
    CHECK(yard.carts[0].z > 2.0);
}

TEST(a_pig_stops_a_moving_boat)
{
    // `dc.b_(kh)` is `oc.b_(kh)` -- the same one instruction -- so everything
    // the cart above collides with, a boat collides with too.
    Yard yard;
    CHECK(yard.boats.place(yard.w(), 4, 63, 4));
    // Let it fall onto the stone, so its box is level with the pig's feet.
    for (int i = 0; i < 20; ++i) {
        yard.boats.tick(yard.w(), VehicleRider{});
    }
    CHECK(yard.boats[0].onGround);

    CHECK(yard.mobs.spawn(yard.w(), MobType::Pig, 4.5, 64.0, 6.0, 0.0f));
    const AABB pig = yard.mobs[0].body.box;
    CHECK(yard.boats[0].box.maxZ < pig.minZ);

    // Held at the speed cap, because a boat on land sheds half its motion a
    // tick and would otherwise stop short of the pig on its own.
    for (int i = 0; i < 20; ++i) {
        const_cast<Boat&>(yard.boats[0]).motionZ = mc::entity::kBoatSpeedCap;
        yard.boats.tick(yard.w(), VehicleRider{});
    }

    CHECK(yard.boats[0].box.maxZ <= pig.minZ + 1e-9);
    CHECK(yard.boats[0].box.maxZ > pig.minZ - 1e-6);
}
