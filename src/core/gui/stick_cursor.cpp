#include "core/gui/stick_cursor.hpp"

namespace mc::gui {

namespace {

float magnitude(float value)
{
    return value < 0.0f ? -value : value;
}

}  // namespace

StickStep stickDirection(float x, float y)
{
    const float ax = magnitude(x);
    const float ay = magnitude(y);
    if (ax < kStickDeadzone && ay < kStickDeadzone) {
        return StickStep::None;
    }
    // Ties go to the horizontal. It has to go one way or the other and the
    // rows these screens are made of are wider than they are tall.
    if (ax >= ay) {
        return x > 0.0f ? StickStep::Right : StickStep::Left;
    }
    return y > 0.0f ? StickStep::Up : StickStep::Down;
}

StickStep StickRepeat::step(float x, float y, float dt)
{
    const StickStep direction = stickDirection(x, y);

    if (direction == StickStep::None) {
        held_ = StickStep::None;
        untilRepeat_ = 0.0f;
        return StickStep::None;
    }

    if (direction != held_) {
        // **A new direction moves at once.** Pushing a stick and seeing nothing
        // happen for a third of a second is how a control comes to feel broken.
        // This is also what makes a corner-to-corner flick one step each way
        // rather than one step and a pause.
        held_ = direction;
        untilRepeat_ = kStickFirstRepeatSeconds;
        return direction;
    }

    untilRepeat_ -= dt;
    if (untilRepeat_ > 0.0f) {
        return StickStep::None;
    }
    // **Carried, not reset to the full interval.** A frame that arrives late --
    // and on this console they do -- would otherwise stretch every repeat after
    // it, so a long frame owes the next repeat whatever it overshot by.
    untilRepeat_ += kStickRepeatSeconds;
    if (untilRepeat_ < 0.0f) {
        // Unless it overshot by more than a whole interval, which is a stall
        // and not a late frame. The debt is dropped and the next repeat is a
        // full one: a console that froze for a second should come back with the
        // cursor where it was, not walk it nine cells to catch up.
        untilRepeat_ = kStickRepeatSeconds;
    }
    return direction;
}

void StickRepeat::reset()
{
    held_ = StickStep::None;
    untilRepeat_ = 0.0f;
}

}  // namespace mc::gui
