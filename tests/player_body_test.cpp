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
