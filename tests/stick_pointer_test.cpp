// The stick over a container window: light tilts step, firm ones move.

#include "framework.hpp"

#include "core/gui/stick_pointer.hpp"

using namespace mc::gui;

namespace {

constexpr float kDt = 1.0f / 30.0f;

StickPointer::Result tickOnce(StickPointer& p, float x, float y)
{
    return p.tick(x, y, kDt, 0.0f, 0.0f, 320.0f, 240.0f);
}

}  // namespace

TEST(a_flick_is_one_step_when_it_is_let_go)
{
    StickPointer p;
    StickPointer::Result r = tickOnce(p, 0.2f, 0.0f);
    CHECK_EQ(r.stepX, 0);
    r = tickOnce(p, 0.0f, 0.0f);
    CHECK_EQ(r.stepX, 1);
    CHECK_EQ(r.stepY, 0);
    CHECK(!p.shown());
    // And only one.
    r = tickOnce(p, 0.0f, 0.0f);
    CHECK_EQ(r.stepX, 0);
}

TEST(a_light_tilt_held_repeats_on_the_stronger_axis)
{
    StickPointer p;
    int steps = 0;
    for (int i = 0; i < 60; ++i) {
        const StickPointer::Result r = tickOnce(p, 0.05f, -0.2f);
        CHECK_EQ(r.stepX, 0);
        CHECK(!r.moved);
        steps += r.stepY == -1 ? 1 : 0;
    }
    // Two seconds: the hold, then one every 0.16 s or so.
    CHECK(steps >= 8 && steps <= 13);
    // Letting go of a tilt that long is not also a flick.
    const StickPointer::Result r = tickOnce(p, 0.0f, 0.0f);
    CHECK_EQ(r.stepY, 0);
}

TEST(a_firm_tilt_moves_the_pointer_and_shows_it)
{
    StickPointer p;
    p.placeAt(100.0f, 100.0f);
    p.hide();
    const StickPointer::Result r = tickOnce(p, 1.0f, 0.0f);
    CHECK(r.moved);
    CHECK(p.shown());
    CHECK(p.x() > 100.0f);
    CHECK(p.y() == 100.0f);
    // A firm flick that is let go moved the pointer, and owes no step on top of it.
    const StickPointer::Result after = tickOnce(p, 0.0f, 0.0f);
    CHECK_EQ(after.stepX, 0);
}

TEST(a_firmer_tilt_is_faster_and_the_pointer_stays_on_the_page)
{
    StickPointer slow;
    StickPointer fast;
    slow.placeAt(10.0f, 10.0f);
    fast.placeAt(10.0f, 10.0f);
    tickOnce(slow, 0.4f, 0.0f);
    tickOnce(fast, 1.0f, 0.0f);
    CHECK(fast.x() > slow.x());
    for (int i = 0; i < 300; ++i) {
        tickOnce(fast, -1.0f, -1.0f);
    }
    CHECK(fast.x() == 0.0f);
    CHECK(fast.y() == 0.0f);
    for (int i = 0; i < 300; ++i) {
        tickOnce(fast, 1.0f, 1.0f);
    }
    CHECK(fast.x() == 320.0f);
    CHECK(fast.y() == 240.0f);
}

TEST(a_resting_stick_inside_the_deadzone_does_nothing)
{
    StickPointer p;
    for (int i = 0; i < 90; ++i) {
        const StickPointer::Result r = tickOnce(p, 0.05f, -0.05f);
        CHECK_EQ(r.stepX, 0);
        CHECK_EQ(r.stepY, 0);
        CHECK(!r.moved);
    }
}
