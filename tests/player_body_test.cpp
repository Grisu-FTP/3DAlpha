// The player body against a real a1.1.2 EntityPlayer, tick for tick.
//
// The fixture drove the game's own `dm` through `moveEntityWithHeading`; this
// rebuilds each scene block for block and drives `PlayerBody::tick` through the
// same inputs, comparing **raw bit patterns**. Not a tolerance: gravity
// compounds through a drag of 0.9800000190734863, and a body that is right to
// twelve digits is a body that is wrong.
//
// Every case runs twice in the fixture, once around the origin and once across
// the negative axis, because `x >> 4` and `x / 16` agree everywhere except
// there and disagree by a whole chunk.
//
// The world is a grid of plain `ChunkColumn`s reached through `TickAccess`, the
// same fixture shape tests/tick_test.cpp uses -- no streamer, no renderer, no
// card. It starts as air, which is what the generator's cleared volume makes
// true on its side.

#include "core/block/registry.hpp"
#include "core/entity/player_body.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/chunk.hpp"
#include "framework.hpp"
#include "player_body_vectors.hpp"
#include "scene_world.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::PlayerBody;
using mc::entity::PlayerInput;
using mc::test::SceneWorld;
using mc::tick::TickWorld;

namespace {

u64 bitsOf(double v)
{
    u64 out = 0;
    std::memcpy(&out, &v, sizeof out);
    return out;
}

u32 bitsOf(float v)
{
    u32 out = 0;
    std::memcpy(&out, &v, sizeof out);
    return out;
}

}  // namespace

TEST(the_body_matches_a_real_entityplayer_tick_for_tick)
{
    for (int c = 0; c < test::kPlayerBodyCaseCount; ++c) {
        const test::PlayerBodyCase& tc = test::kPlayerBodyCases[c];

        SceneWorld scene(tc.originX >> 4, tc.originZ >> 4);
        for (int i = 0; i < tc.placementCount; ++i) {
            const test::ScenePlacement& p = tc.placements[i];
            scene.place(tc.originX + p.dx, tc.originY + p.dy, tc.originZ + p.dz,
                        BlockId(p.id), p.metadata);
        }

        double startAbove = 0.0;
        std::memcpy(&startAbove, &tc.startFeetAbove, sizeof startAbove);

        PlayerBody body;
        body.setFeet(double(tc.originX) + 0.5, double(tc.originY) + 1.0 + startAbove,
                     double(tc.originZ) + 0.5);

        PlayerInput input;
        input.strafe = tc.strafe;
        input.forward = tc.forward;
        input.yawDegrees = tc.yawDegrees;
        input.jump = tc.jump;
        input.sneak = false;

        for (int s = 0; s < tc.stepCount; ++s) {
            body.tick(scene.w(), input);
            const test::PlayerBodyStep& want = tc.steps[s];

            const u64 gotX = bitsOf(body.x);
            const u64 gotY = bitsOf(body.eyeY());
            const u64 gotZ = bitsOf(body.z);
            const u64 gotFeet = bitsOf(body.y);
            const u64 gotMx = bitsOf(body.motionX);
            const u64 gotMy = bitsOf(body.motionY);
            const u64 gotMz = bitsOf(body.motionZ);

            struct Field {
                const char* name;
                u64 got;
                u64 want;
                double gotValue;
                double wantValue;
            };
            const double wantFeet = *reinterpret_cast<const double*>(&want.boxMinY);
            const Field fields[] = {
                {"posX", gotX, want.posX, body.x, *reinterpret_cast<const double*>(&want.posX)},
                {"posY(eye)", gotY, want.posY, body.eyeY(),
                 *reinterpret_cast<const double*>(&want.posY)},
                {"posZ", gotZ, want.posZ, body.z, *reinterpret_cast<const double*>(&want.posZ)},
                {"feet", gotFeet, want.boxMinY, body.y, wantFeet},
                {"motionX", gotMx, want.motionX, body.motionX,
                 *reinterpret_cast<const double*>(&want.motionX)},
                {"motionY", gotMy, want.motionY, body.motionY,
                 *reinterpret_cast<const double*>(&want.motionY)},
                {"motionZ", gotMz, want.motionZ, body.motionZ,
                 *reinterpret_cast<const double*>(&want.motionZ)},
                {"ySize", bitsOf(body.ySize), want.ySize, double(body.ySize),
                 double(*reinterpret_cast<const float*>(&want.ySize))},
                {"fallDistance", bitsOf(body.fallDistance), want.fallDistance,
                 double(body.fallDistance),
                 double(*reinterpret_cast<const float*>(&want.fallDistance))},
                {"onGround", u64(body.onGround), u64(want.onGround), double(body.onGround),
                 double(want.onGround)},
                {"collidedHorizontally", u64(body.collidedHorizontally),
                 u64(want.collidedHorizontally), double(body.collidedHorizontally),
                 double(want.collidedHorizontally)},
                {"collidedVertically", u64(body.collidedVertically),
                 u64(want.collidedVertically), double(body.collidedVertically),
                 double(want.collidedVertically)},
            };
            for (const Field& f : fields) {
                if (f.got == f.want) {
                    continue;
                }
                char message[512];
                std::snprintf(message, sizeof message,
                              "%s step %d: %s is %.17g, expected %.17g",
                              tc.name, s, f.name, f.gotValue, f.wantValue);
                test::reportFailure(__FILE__, __LINE__, message);
                return;  // One divergence is the finding; the rest is noise.
            }
        }
    }
}

TEST(the_fixture_exercises_both_standing_and_falling)
{
    // A body that never left the ground, or never touched it, would satisfy a
    // comparison that only ever saw one of those states.
    CHECK(test::kPlayerBodyGroundedSteps > 100);
    CHECK(test::kPlayerBodyAirborneSteps > 100);
}

// **A fixture whose name claims a behaviour needs a test that the rows show
// it.** `swim_up_by_holding_jump` sank for as long as `tools/genref.java` drove
// the jar with `if (jumping && onGround) jump()` -- onLivingUpdate's third
// branch and neither of its first two -- and the comparison above passed all
// fourteen liquid cases against that wrong reference, bit for bit. Nothing in a
// tick-for-tick match can catch an oracle that is wrong in the same way the
// port is; only a claim about the *shape* of the answer can.
TEST(holding_jump_in_water_rises_instead_of_jumping)
{
    int cases = 0;
    for (int c = 0; c < test::kPlayerBodyCaseCount; ++c) {
        const test::PlayerBodyCase& tc = test::kPlayerBodyCases[c];
        if (std::string(tc.name).find("swim_up_by_holding_jump") == std::string::npos) {
            continue;
        }
        ++cases;

        double firstFeet = 0.0;
        double lastFeet = 0.0;
        double firstMotionY = 0.0;
        std::memcpy(&firstFeet, &tc.steps[0].boxMinY, sizeof firstFeet);
        std::memcpy(&lastFeet, &tc.steps[tc.stepCount - 1].boxMinY, sizeof lastFeet);
        std::memcpy(&firstMotionY, &tc.steps[0].motionY, sizeof firstMotionY);

        // The first tick already moves upward. The jump would have been 0.42
        // and sinking would have been -0.02; a rise is the flat 0.04 damped.
        CHECK(firstMotionY > 0.0);
        CHECK(firstMotionY < 0.05);
        // And it keeps going: two blocks of water in sixty ticks at least.
        CHECK(lastFeet > firstFeet + 2.0);
        // Never off the bottom of the pool, which is what a jump would be.
        for (int s = 0; s < tc.stepCount; ++s) {
            CHECK(!tc.steps[s].onGround);
        }
    }
    // Once around the origin and once across the negative axis.
    CHECK_EQ(cases, 2);
}

TEST(the_bodys_constants_are_the_ones_the_jar_reports)
{
    // Read off a constructed EntityPlayer by the generator, so the header is
    // checked against the game rather than against the prose describing it.
    CHECK_EQ(double(test::kPlayerYOffset), double(entity::kEyeHeight));
    CHECK_EQ(double(test::kPlayerBoxWidth), double(entity::kPlayerWidth));
    CHECK_EQ(double(test::kPlayerBoxHeight), double(entity::kPlayerHeight));
    CHECK_EQ(double(test::kPlayerStepHeight), double(entity::kStepHeight));
}

TEST(sneaking_walks_the_step_back_rather_than_off_the_ledge)
{
    // Not in the oracle, and it cannot be: Entity.isSneaking is a hardcoded
    // false and only the client-side player class overrides it, so an
    // EntityPlayer cannot be made to sneak from outside the game. The
    // behaviour is small enough to state directly.
    SceneWorld scene(0, 0);
    for (i32 x = -4; x <= 0; ++x) {
        for (i32 z = -4; z <= 4; ++z) {
            scene.place(x, 64, z, BlockId(mcver::Block::Stone), 0);
        }
    }
    // The plate ends at x = 0, so x > 0.5 is over the drop.

    PlayerBody sneaker;
    sneaker.setFeet(0.0, 65.0, 0.0);
    sneaker.onGround = true;
    sneaker.sneaking = true;
    sneaker.move(scene.w(), 4.0, 0.0, 0.0);

    // The invariant the walk-back exists to hold, stated as itself rather than
    // as a number: the box, dropped one block, still overlaps the plate. The
    // plate's last column spans x in [0, 1), so the near edge must be under 1.
    const double halfWidth = double(entity::kPlayerWidth / 2.0f);
    if (!(sneaker.x - halfWidth < 1.0)) {
        char message[160];
        std::snprintf(message, sizeof message,
                      "sneaker walked to x = %.17g, whose near edge %.17g is past the "
                      "plate at x = 1", sneaker.x, sneaker.x - halfWidth);
        test::reportFailure(__FILE__, __LINE__, message);
        return;
    }
    // It stopped at the edge rather than refusing to move at all.
    CHECK(sneaker.x > 1.0);
    // And it never left the surface it started on.
    CHECK(sneaker.y == 65.0);

    PlayerBody walker;
    walker.setFeet(0.0, 65.0, 0.0);
    walker.onGround = true;
    walker.sneaking = false;
    walker.move(scene.w(), 4.0, 0.0, 0.0);
    // Without sneaking the same move goes the whole way, out over the edge.
    CHECK(walker.x > 3.0);
    CHECK(walker.y == 65.0);  // moveEntity does not fall; gravity is tick's job

    CHECK(sneaker.x < walker.x);
}

// ---------------------------------------------------------------------------
// Ladders
// ---------------------------------------------------------------------------
//
// Not in the oracle, and for the same reason sneaking is not: the generator
// drives a real `EntityPlayer` through `moveEntityWithHeading`, and getting one
// to press itself into a wall for sixty ticks needs an input the harness has no
// way to supply. The three properties below are what the two `isOnLadder`
// branches decide, stated directly.

namespace {

// A wall of stone at x = 1 with a ladder up its -x face, standing on a floor.
// The ladder is at metadata 4, which is the face the placement table writes for
// a click on the wall's -x side.
struct LadderShaft {
    SceneWorld scene{0, 0};

    LadderShaft()
    {
        for (i32 x = -4; x <= 4; ++x) {
            for (i32 z = -4; z <= 4; ++z) {
                scene.place(x, 63, z, BlockId(mcver::Block::Stone), 0);
            }
        }
        for (int y = 64; y <= 76; ++y) {
            scene.place(1, y, 0, BlockId(mcver::Block::Stone), 0);
            scene.place(0, y, 0, BlockId(mcver::Block::Ladder), 4);
        }
    }
};

// Walking into the wall: full forward, facing +x, which is yaw 270 in the
// original's degrees.
entity::PlayerInput intoTheWall()
{
    entity::PlayerInput in;
    in.forward = 1.0f;
    in.strafe = 0.0f;
    in.yawDegrees = 270.0f;
    return in;
}

}  // namespace

TEST(pressing_into_a_ladder_climbs_it)
{
    LadderShaft shaft;
    PlayerBody body;
    body.setFeet(0.4, 64.0, 0.5);
    body.onGround = true;

    const double start = body.box.minY;
    for (int i = 0; i < 60; ++i) {
        body.tick(shaft.scene.w(), intoTheWall());
    }
    // 0.2 a tick, damped -- several blocks in three seconds, and the shaft is
    // twelve tall so it cannot simply be standing on the floor.
    CHECK(body.box.minY > start + 3.0);
}

TEST(jump_climbs_a_ladder_without_pressing_into_it)
{
    // **Ours, not a1.1.2's** -- see the note in `PlayerBody::tick`. The stick
    // is doing nothing at all here, which is precisely the case the original
    // cannot climb: `collidedHorizontally` is false the whole way up.
    LadderShaft shaft;
    PlayerBody body;
    body.setFeet(0.4, 64.0, 0.5);
    body.onGround = true;

    entity::PlayerInput jumping;
    jumping.yawDegrees = 270.0f;
    jumping.jump = true;

    const double start = body.box.minY;
    for (int i = 0; i < 60; ++i) {
        body.tick(shaft.scene.w(), jumping);
    }
    // The same 0.2 a tick the wall-press gives, so the same bound as
    // `pressing_into_a_ladder_climbs_it` -- the two routes are one speed.
    CHECK(body.box.minY > start + 3.0);
}

TEST(jump_off_a_ladder_is_still_a_jump)
{
    // The branch is ordered ladder-before-ground, so the negative control is
    // that a body nowhere near one still leaves the floor at the jump impulse
    // rather than at the climb rate.
    LadderShaft shaft;
    PlayerBody body;
    body.setFeet(-3.5, 64.0, -3.5);
    body.onGround = true;

    entity::PlayerInput jumping;
    jumping.yawDegrees = 270.0f;
    jumping.jump = true;

    CHECK(!body.onLadder(shaft.scene.w()));
    body.tick(shaft.scene.w(), jumping);
    // kJumpVelocity is 0.42 and kLadderClimb is 0.2; one tick tells them apart.
    CHECK(body.box.minY > 64.0 + 0.3);
}

TEST(letting_go_of_a_ladder_slides_rather_than_drops)
{
    // The first `isOnLadder` branch: `if (motionY < -0.15) motionY = -0.15`, so
    // a body on a ladder falls at a fifteenth of a block a tick whatever it was
    // doing before. Sixty ticks of free fall would be tens of blocks.
    LadderShaft shaft;
    PlayerBody body;
    body.setFeet(0.4, 76.0, 0.5);

    entity::PlayerInput idle;
    idle.yawDegrees = 270.0f;

    const double start = body.box.minY;
    for (int i = 0; i < 40; ++i) {
        body.tick(shaft.scene.w(), idle);
    }
    const double dropped = start - body.box.minY;
    CHECK(dropped > 0.0);
    // Forty ticks at the clamp is six blocks; free fall over the same forty is
    // well past twenty.
    CHECK(dropped < 8.0);
}

TEST(a_ladder_holds_the_chest_as_well_as_the_feet)
{
    // `ge.A()` reads the cell at `floor(boundingBox.minY)` **and the one above
    // it**. Without the second half the last block of every climb drops you:
    // the feet leave the ladder's top cell before the body does.
    // A shaft whose lowest rung is one block clear of the floor, so that
    // standing on the floor puts the feet in an empty cell with a ladder in the
    // cell above them -- which is exactly the case the second half exists for.
    SceneWorld scene(0, 0);
    for (i32 x = -4; x <= 4; ++x) {
        for (i32 z = -4; z <= 4; ++z) {
            scene.place(x, 63, z, BlockId(mcver::Block::Stone), 0);
        }
    }
    for (int y = 65; y <= 70; ++y) {
        scene.place(1, y, 0, BlockId(mcver::Block::Stone), 0);
        scene.place(0, y, 0, BlockId(mcver::Block::Ladder), 4);
    }

    PlayerBody body;

    // Feet two clear of the lowest rung: neither half answers, which is the
    // control.
    body.setFeet(0.4, 72.0, 0.5);
    CHECK(!body.onLadder(scene.w()));

    // Feet level with a rung: the first half answers.
    body.setFeet(0.4, 66.0, 0.5);
    CHECK(body.onLadder(scene.w()));

    // **Feet on the floor with the lowest rung at head height**: only the
    // second half answers, and it is the half that matters -- without it the
    // bottom of every ladder would be a step you cannot get on to and the top
    // would be a step that drops you.
    body.setFeet(0.4, 64.0, 0.5);
    CHECK(body.onLadder(scene.w()));
}

TEST(walking_into_a_plain_wall_does_not_climb_it)
{
    // The control for the climb: the same push against stone rather than a
    // ladder goes nowhere. Without it "climbed" could just mean "stepped up".
    SceneWorld scene(0, 0);
    for (i32 x = -4; x <= 4; ++x) {
        for (i32 z = -4; z <= 4; ++z) {
            scene.place(x, 63, z, BlockId(mcver::Block::Stone), 0);
        }
    }
    for (int y = 64; y <= 76; ++y) {
        scene.place(1, y, 0, BlockId(mcver::Block::Stone), 0);
    }

    PlayerBody body;
    body.setFeet(0.4, 64.0, 0.5);
    body.onGround = true;
    for (int i = 0; i < 60; ++i) {
        body.tick(scene.w(), intoTheWall());
    }
    CHECK(body.box.minY < 64.5);
}
