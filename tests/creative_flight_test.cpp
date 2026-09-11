// Creative flight: a camera that collides.
//
// **No oracle, by construction.** a1.1.2 has no flight, so unlike
// player_body_test.cpp there is nothing to compare tick for tick against. What
// makes this worth a test anyway is the one property flight is *for*: it
// reuses `Entity.moveEntity`, so a wall stops it. Spectator's free camera does
// not have that property and this is the whole difference between the two.
//
// The scenes are the smallest ones that can tell the difference between "the
// sweep works" and "the sweep is being skipped": a wall to fly into at both
// speeds, a ceiling to rise into, a floor to sink onto, and open air to prove
// nothing falls.

#include "core/block/registry.hpp"
#include "core/entity/player_body.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::entity::kFlightSpeed;
using mc::entity::PlayerBody;
using mc::entity::PlayerInput;
using mc::test::SceneWorld;

namespace {

const block::BlockId kStone = block::BlockId(mcver::Block::Stone);

// A floor of stone at y = 63, filling the scene's chunks.
void floorAt(SceneWorld& scene, i32 centreX, i32 centreZ, int y)
{
    const i32 minX = (centreX - SceneWorld::kRadius) * 16;
    const i32 minZ = (centreZ - SceneWorld::kRadius) * 16;
    const i32 span = (SceneWorld::kRadius * 2 + 1) * 16;
    for (i32 z = minZ; z < minZ + span; ++z) {
        for (i32 x = minX; x < minX + span; ++x) {
            scene.place(x, y, z, kStone, 0);
        }
    }
}

PlayerInput forward(float yawDegrees)
{
    PlayerInput input;
    input.forward = 1.0f;
    input.yawDegrees = yawDegrees;
    return input;
}

}  // namespace

TEST(flight_does_not_fall)
{
    SceneWorld scene(0, 0);
    floorAt(scene, 0, 0, 63);

    PlayerBody body;
    body.setFeet(0.5, 80.0, 0.5);

    // Yaw 0 faces +Z in the original's convention, and no stick input at all.
    PlayerInput idle;
    for (int i = 0; i < 200; ++i) {
        body.tickFlying(scene.w(), idle, false, false, kFlightSpeed);
    }
    CHECK_EQ(body.y, 80.0);
    CHECK_EQ(body.motionY, 0.0);
    CHECK_EQ(body.fallDistance, 0.0f);
}

TEST(flight_climbs_and_descends_at_the_speed_it_is_given)
{
    SceneWorld scene(0, 0);
    floorAt(scene, 0, 0, 63);

    PlayerBody body;
    body.setFeet(0.5, 80.0, 0.5);
    PlayerInput idle;

    body.tickFlying(scene.w(), idle, true, false, kFlightSpeed);
    CHECK_EQ(body.y, 80.0 + kFlightSpeed);

    body.tickFlying(scene.w(), idle, false, true, kFlightSpeed);
    CHECK_EQ(body.y, 80.0);

    // Ascend wins over descend, which is what a player holding both expects
    // and, more usefully, is a defined answer rather than whichever branch came
    // first.
    body.tickFlying(scene.w(), idle, true, true, kFlightSpeed);
    CHECK_EQ(body.y, 80.0 + kFlightSpeed);
}

TEST(flight_lands_on_a_floor_rather_than_through_it)
{
    SceneWorld scene(0, 0);
    floorAt(scene, 0, 0, 63);

    PlayerBody body;
    body.setFeet(0.5, 80.0, 0.5);
    PlayerInput idle;
    for (int i = 0; i < 200; ++i) {
        body.tickFlying(scene.w(), idle, false, true, kFlightSpeed);
    }
    // The floor's top is y = 64, and the feet stop on it.
    CHECK_EQ(body.y, 64.0);
    CHECK(body.onGround);
}

TEST(flight_stops_at_a_ceiling)
{
    SceneWorld scene(0, 0);
    floorAt(scene, 0, 0, 63);
    floorAt(scene, 0, 0, 70);

    PlayerBody body;
    body.setFeet(0.5, 64.0, 0.5);
    PlayerInput idle;
    for (int i = 0; i < 100; ++i) {
        body.tickFlying(scene.w(), idle, true, false, kFlightSpeed);
    }
    // The body is 1.8 tall and the ceiling's underside is y = 70.
    CHECK_EQ(body.y, 70.0 - double(mc::entity::kPlayerHeight));
    CHECK(body.collidedVertically);
}

TEST(flight_is_stopped_by_a_wall_even_faster_than_a_block_a_tick)
{
    // One block thick, which is the case a swept box handles and a ray would
    // not. **Creative flight has one speed now** -- A's held boost went when A
    // became the drop -- but the sweep is what makes a *fast* mover safe, and
    // an over-speed case is what proves it, so the second pass runs at more
    // than a block a tick even though nothing in the game asks for it.
    constexpr double kOverSpeed = 2.0;
    for (int fast = 0; fast < 2; ++fast) {
        SceneWorld scene(0, 0);
        floorAt(scene, 0, 0, 63);
        for (int y = 64; y < 72; ++y) {
            for (i32 x = -8; x <= 8; ++x) {
                scene.place(x, y, 10, kStone, 0);
            }
        }

        PlayerBody body;
        body.setFeet(0.5, 66.0, 0.5);
        const double speed = fast != 0 ? kOverSpeed : kFlightSpeed;
        for (int i = 0; i < 60; ++i) {
            body.tickFlying(scene.w(), forward(0.0f), false, false, speed);
        }
        // Never past the wall's near face, wherever inside the reach it stopped.
        CHECK(body.z < 10.0);
        CHECK(body.collidedHorizontally);
    }
}

TEST(flight_works_across_the_negative_axis)
{
    // The same reason every physics suite here runs twice: `x >> 4` and
    // `x / 16` agree everywhere except below zero, and disagree by a chunk.
    SceneWorld scene(-40, -40);
    floorAt(scene, -40, -40, 63);
    for (int y = 64; y < 72; ++y) {
        for (i32 x = -650; x <= -630; ++x) {
            scene.place(x, y, -630, kStone, 0);
        }
    }

    PlayerBody body;
    body.setFeet(-639.5, 66.0, -640.5);
    for (int i = 0; i < 60; ++i) {
        body.tickFlying(scene.w(), forward(0.0f), false, false, kFlightSpeed);
    }
    CHECK(body.z < -630.0);
    CHECK(body.collidedHorizontally);
    CHECK_EQ(body.y, 66.0);
}
