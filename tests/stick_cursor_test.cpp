// The circle pad read as a d-pad: which way a push counts as, and when it
// counts again.
//
// The console part of that is two lines and cannot be tested here; everything
// below is the part that decided how the control feels, which is why it was
// pulled out of the overlay in the first place.

#include "framework.hpp"

#include "core/gui/stick_cursor.hpp"

using namespace mc;
using namespace mc::gui;

TEST(a_centred_stick_is_no_direction_at_all)
{
    CHECK(stickDirection(0.0f, 0.0f) == StickStep::None);
    // Inside the deadzone on both axes, which is where a stick at rest sits --
    // it does not return to exactly zero.
    CHECK(stickDirection(0.05f, -0.05f) == StickStep::None);
    CHECK(stickDirection(-0.099f, 0.099f) == StickStep::None);
}

TEST(a_push_points_the_way_it_is_pushed_with_up_being_up)
{
    CHECK(stickDirection(1.0f, 0.0f) == StickStep::Right);
    CHECK(stickDirection(-1.0f, 0.0f) == StickStep::Left);
    // The pad's +y is up on the stick and up on the screen; nothing between
    // here and the grid flips it.
    CHECK(stickDirection(0.0f, 1.0f) == StickStep::Up);
    CHECK(stickDirection(0.0f, -1.0f) == StickStep::Down);
}

TEST(a_diagonal_resolves_to_one_direction_rather_than_stepping_twice)
{
    // The grids take one step at a time, so a corner has to mean one thing.
    // Whichever axis is pushed harder wins.
    CHECK(stickDirection(0.9f, 0.4f) == StickStep::Right);
    CHECK(stickDirection(0.4f, 0.9f) == StickStep::Up);
    CHECK(stickDirection(-0.9f, -0.4f) == StickStep::Left);
    CHECK(stickDirection(-0.4f, -0.9f) == StickStep::Down);

    // An exact corner is horizontal, because the rows these screens are made of
    // are wider than they are tall. What matters is that it is one of the two
    // and always the same one.
    CHECK(stickDirection(0.7f, 0.7f) == StickStep::Right);
}

TEST(one_axis_past_the_deadzone_counts_even_while_the_other_is_inside_it)
{
    // A push straight right is never exactly straight. The deadzone is about
    // the stick being at rest, not about either axis on its own.
    CHECK(stickDirection(0.6f, 0.02f) == StickStep::Right);
    CHECK(stickDirection(0.02f, 0.6f) == StickStep::Up);
}

TEST(a_new_direction_steps_on_the_frame_it_is_pushed)
{
    StickRepeat stick;
    // Not on the second frame, and not after a wait: a control that does
    // nothing for a third of a second reads as broken.
    CHECK(stick.step(1.0f, 0.0f, 1.0f / 60.0f) == StickStep::Right);
}

TEST(a_held_direction_waits_before_it_repeats_and_then_repeats_steadily)
{
    StickRepeat stick;
    constexpr float kFrame = 1.0f / 60.0f;

    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);

    // Nothing until the first repeat is due.
    float elapsed = 0.0f;
    int steps = 0;
    while (elapsed < kStickFirstRepeatSeconds - kFrame) {
        if (stick.step(1.0f, 0.0f, kFrame) != StickStep::None) {
            ++steps;
        }
        elapsed += kFrame;
    }
    CHECK_EQ(steps, 0);

    // Then one, and one per repeat interval after it. Counted over a second so
    // the answer is a rate rather than a single edge.
    steps = 0;
    for (float t = 0.0f; t < 1.0f; t += kFrame) {
        if (stick.step(1.0f, 0.0f, kFrame) != StickStep::None) {
            ++steps;
        }
    }
    const int expected = int(1.0f / kStickRepeatSeconds);
    CHECK(steps >= expected - 1);
    CHECK(steps <= expected + 1);
}

TEST(letting_the_stick_go_ends_the_repeat_and_the_next_push_is_a_fresh_one)
{
    StickRepeat stick;
    constexpr float kFrame = 1.0f / 60.0f;

    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);
    // Held long enough to be repeating.
    for (float t = 0.0f; t < 1.0f; t += kFrame) {
        stick.step(1.0f, 0.0f, kFrame);
    }

    CHECK(stick.step(0.0f, 0.0f, kFrame) == StickStep::None);
    CHECK(stick.held() == StickStep::None);

    // And the push after it steps at once rather than landing mid-repeat.
    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);
}

TEST(turning_the_stick_to_a_new_direction_steps_at_once_rather_than_mid_repeat)
{
    StickRepeat stick;
    constexpr float kFrame = 1.0f / 60.0f;

    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);
    for (float t = 0.0f; t < 0.5f; t += kFrame) {
        stick.step(1.0f, 0.0f, kFrame);
    }
    // Rolling a thumb from one edge of the gate to the other is a new step, not
    // a continuation of the old one's timer.
    CHECK(stick.step(0.0f, 1.0f, kFrame) == StickStep::Up);
}

TEST(a_reset_leaves_no_repeat_behind_for_the_next_screen)
{
    StickRepeat stick;
    constexpr float kFrame = 1.0f / 60.0f;

    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);
    for (float t = 0.0f; t < 0.5f; t += kFrame) {
        stick.step(1.0f, 0.0f, kFrame);
    }

    // The screen was left with the stick still over -- unfocusing, or a page
    // that has no cursor. The next screen starts from a push, not from whatever
    // the last one was in the middle of.
    stick.reset();
    CHECK(stick.held() == StickStep::None);
    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);
}

TEST(a_late_frame_does_not_stretch_the_repeat_or_fire_a_burst_to_catch_up)
{
    StickRepeat stick;
    constexpr float kFrame = 1.0f / 60.0f;

    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::Right);

    // One frame long enough to cover the wait and part of a repeat. It owes one
    // step, not the three it was late by.
    CHECK(stick.step(1.0f, 0.0f, kStickFirstRepeatSeconds) == StickStep::Right);
    CHECK(stick.step(1.0f, 0.0f, kFrame) == StickStep::None);

    // A stall -- a second of frozen console -- is not a reason to walk the
    // cursor nine cells when it comes back.
    StickRepeat stalled;
    CHECK(stalled.step(1.0f, 0.0f, kFrame) == StickStep::Right);
    CHECK(stalled.step(1.0f, 0.0f, 5.0f) == StickStep::Right);
    CHECK(stalled.step(1.0f, 0.0f, kFrame) == StickStep::None);
}
