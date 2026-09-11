// The double-tap-forward sprint gesture, and the render interpolation the
// camera reads off the body.
//
// **Neither has an oracle**, and for opposite reasons. Sprint is not in a1.1.2
// at all -- see core/entity/sprint_gesture.hpp -- so there is nothing to
// compare it against and what is checked here is that the state machine cannot
// be tricked into a sprint by a stick that is merely being held. Interpolation
// *is* the original's (`EntityRenderer.orientCamera` reads
// `prevPos + (pos - prevPos) * partialTicks`), but it is a rendering detail the
// physics oracle in player_body_test.cpp deliberately does not see, so it is
// checked here as arithmetic instead.

#include "core/entity/player_body.hpp"
#include "core/entity/sprint_gesture.hpp"
#include "framework.hpp"

#include <cmath>

using namespace mc;
using mc::entity::PlayerBody;
using mc::entity::SprintGesture;
using mc::entity::kSprintTapSeconds;

namespace {

// The framework has CHECK and CHECK_EQ and no float comparison, which is
// deliberate -- every other numeric test in this suite compares bit patterns
// against an oracle. Interpolation has no oracle and is arithmetic on doubles,
// so it gets a tolerance, kept local rather than added to the framework.
bool near(double a, double b, double epsilon) { return std::fabs(a - b) <= epsilon; }

}  // namespace

TEST(sprint_needs_two_taps_not_one)
{
    SprintGesture gesture;
    // A held stick, for a whole second of frames at 60 fps. One press, however
    // long it lasts, is a walk.
    for (int frame = 0; frame < 60; ++frame) {
        CHECK(!gesture.update(1.0f, float(frame) / 60.0f));
    }
    CHECK(!gesture.sprinting());
}

TEST(sprint_starts_on_a_second_tap_inside_the_window)
{
    SprintGesture gesture;
    CHECK(!gesture.update(1.0f, 0.00f));  // first press
    CHECK(!gesture.update(0.0f, 0.05f));  // released
    CHECK(gesture.update(1.0f, 0.10f));   // second press, inside the window
    CHECK(gesture.sprinting());
}

TEST(sprint_does_not_start_on_a_tap_outside_the_window)
{
    SprintGesture gesture;
    CHECK(!gesture.update(1.0f, 0.0f));
    CHECK(!gesture.update(0.0f, 0.1f));
    // Deliberately just past the edge rather than far past it.
    CHECK(!gesture.update(1.0f, kSprintTapSeconds + 0.01f));
    CHECK(!gesture.sprinting());
}

TEST(sprint_ends_when_the_stick_comes_back)
{
    SprintGesture gesture;
    gesture.update(1.0f, 0.00f);
    gesture.update(0.0f, 0.05f);
    CHECK(gesture.update(1.0f, 0.10f));
    // Half deflection is below the release threshold, so a stick eased off
    // stops the sprint rather than dragging it along at a walk's speed.
    CHECK(!gesture.update(0.4f, 0.20f));
    // ...and one press afterwards does not resume it: the pending tap went with
    // the sprint.
    CHECK(!gesture.update(1.0f, 0.22f));
}

TEST(sprint_ignores_a_stick_that_never_crosses_the_press_threshold)
{
    SprintGesture gesture;
    // A pad resting a little off centre, jittering either side of the release
    // threshold. This is the case the two thresholds exist for.
    for (int frame = 0; frame < 40; ++frame) {
        const float forward = (frame % 2) == 0 ? 0.55f : 0.45f;
        CHECK(!gesture.update(forward, float(frame) / 60.0f));
    }
}

TEST(sprint_cancel_needs_a_fresh_double_tap)
{
    SprintGesture gesture;
    gesture.update(1.0f, 0.00f);
    gesture.update(0.0f, 0.05f);
    CHECK(gesture.update(1.0f, 0.10f));
    gesture.cancel();
    CHECK(!gesture.sprinting());
    // Still held, and still not sprinting: cancelling means the stick has to be
    // let go and tapped twice again.
    CHECK(!gesture.update(1.0f, 0.15f));
    CHECK(!gesture.update(0.0f, 0.20f));
    CHECK(!gesture.update(1.0f, 0.22f));
}

TEST(render_position_interpolates_between_the_last_two_ticks)
{
    PlayerBody body;
    body.setFeet(10.0, 64.0, -20.0);
    // A tick's worth of travel, applied the way a tick would: prev is snapped
    // first and the position moves afterwards.
    body.snapRenderPosition();
    body.x += 0.2;
    body.z -= 0.4;
    body.posY += 0.1;

    CHECK(near(body.renderX(0.0f), 10.0, 1e-12));
    CHECK(near(body.renderX(1.0f), 10.2, 1e-12));
    CHECK(near(body.renderX(0.5f), 10.1, 1e-12));
    CHECK(near(body.renderZ(0.5f), -20.2, 1e-12));
    CHECK(near(body.renderEyeY(0.5f), 64.0 + double(entity::kEyeHeight) + 0.05, 1e-9));
}

TEST(setting_the_feet_leaves_nothing_to_interpolate)
{
    PlayerBody body;
    body.setFeet(0.0, 64.0, 0.0);
    // A teleport, which is what a gamemode change does. Every partial has to
    // answer with the destination, or the frame after one draws the camera
    // sliding across the world.
    body.setFeet(500.0, 70.0, -300.0);
    for (float partial : {0.0f, 0.25f, 0.5f, 1.0f}) {
        CHECK(near(body.renderX(partial), 500.0, 1e-12));
        CHECK(near(body.renderZ(partial), -300.0, 1e-12));
        CHECK(near(body.renderEyeY(partial), body.eyeY(), 1e-12));
    }
}
