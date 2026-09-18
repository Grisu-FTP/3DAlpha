#pragma once

// A thumbstick read as a d-pad: which way it is pushed, and when that counts as
// another press.
//
// **Why this is not in the overlay.** The console part of it is two lines --
// `hidCircleRead` and a `KEY_D*` bit -- and the rest is a rule: a diagonal has
// to resolve to one direction because the grids it drives take one at a time,
// the first step has to land the moment the stick leaves centre, and the
// repeats after it have to be slow enough to stop on. None of that needs a 3DS
// and all of it is worth a test, which is the line this project draws. See
// docs/architecture.md.
//
// **Nothing here is a1.1.2's.** The original has a mouse on these screens and
// no second pointing device to reconcile with the first; the timings below are
// this port's, picked to sit where the console's own menus sit.

#include "core/util/types.hpp"

namespace mc::gui {

enum class StickStep {
    None,
    Left,
    Right,
    Up,
    Down,
};

// How long a direction is held before it starts repeating, and how fast it
// repeats afterwards. The first is long enough that one step does not need a
// quick hand; the second is short enough that crossing a twenty-seven cell grid
// is not a chore.
inline constexpr float kStickFirstRepeatSeconds = 0.35f;
inline constexpr float kStickRepeatSeconds = 0.11f;

// Below this the stick is centred. The same fraction of full deflection the
// rest of the shell uses for the pad, kept here so the rule and the number that
// decides when it applies are in one place.
inline constexpr float kStickDeadzone = 0.1f;

// Which way a deflection points, or `None` inside the deadzone.
//
// **One direction at a time, whichever axis is pushed harder.** The d-pad these
// screens were written for cannot report a diagonal and none of the grids takes
// one, so a stick held into a corner has to mean a single step -- otherwise it
// steps twice, once each way, and lands somewhere nobody aimed.
//
// `y` is the stick's own sign: positive is up, which is up on the screen.
StickStep stickDirection(float x, float y);

// The repeat, as a thing with a memory.
//
// Fed the stick once a frame, it answers with the step to take now: the
// direction itself on the frame it changes, `None` while the wait runs down,
// and the direction again on every repeat after that.
class StickRepeat {
public:
    // `dt` is seconds. Returns what the focused screen should be told this
    // frame.
    StickStep step(float x, float y, float dt);

    // Back to centre without a stick reading -- for leaving the screen the
    // cursor was on, so the next one starts from a fresh push rather than from
    // a repeat left over from the way out.
    void reset();

    StickStep held() const { return held_; }

private:
    StickStep held_ = StickStep::None;
    float untilRepeat_ = 0.0f;
};

}  // namespace mc::gui
