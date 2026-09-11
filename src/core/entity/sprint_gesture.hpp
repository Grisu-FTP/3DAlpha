#pragma once

// Double-tap the stick forward to sprint, and let go to stop.
//
// **Nothing here is a1.1.2's.** That jar has no sprint at all: `EntityPlayer`
// carries no `sprinting` field, `moveEntityWithHeading` reads a constant `0.1f`
// where later versions read `landMovementFactor`, and the word does not appear
// in the client. Sprinting is Beta 1.8's, and the *gesture* -- two taps of the
// stick rather than a held button -- is the console editions', which is where
// it is being asked for from. So this file is invented, it is labelled as such,
// and no test in `tests/` compares it to a reference implementation. See
// core/item/creative_palette.hpp for the same argument made about Creative.
//
// **And it is offered in Creative only**, for that reason rather than a
// gameplay one. Creative is already openly not this version's; Survival here is
// meant to *be* a1.1.2, and a1.1.2's player walks at one speed. The gate lives
// in platform/ctr/main.cpp where the gamemode is known -- this file decides
// when a double tap has happened and nothing about who may make one.
//
// What is not invented is the speed: `PlayerInput::sprint` turns the ground
// term into Beta's `landMovementFactor * 1.3`, applied at the point in the tick
// Beta applies it. This file decides *when* to ask for that and nothing else.
//
// **Frames, not ticks.** A double tap is a gesture and its window has to mean
// the same thing at 30 fps as at 60, so `update` is handed a wall clock in
// seconds and is called once a frame -- the same treatment, and the same
// quarter-second, as the flight toggle in platform/ctr/main.cpp. The physics it
// feeds still runs at 20 Hz; only the decision is frame-timed.
//
// It holds three values and touches no world, so it costs nothing to run in the
// per-frame path and can be driven from a test on the host.

namespace mc::entity {

// A quarter of a second, which is what a double click means everywhere else in
// this project -- the flight toggle uses the same number for the same reason.
inline constexpr float kSprintTapSeconds = 0.25f;

// **Two thresholds rather than one, because the stick is analogue.** A single
// edge at some fixed deflection chatters when the pad rests near it, and a
// chattering edge reads as a double tap: you would break into a sprint by
// holding the stick still. Pressing needs four fifths of full deflection and
// releasing needs to fall below half, so the gap has to be crossed deliberately.
inline constexpr float kSprintPressThreshold = 0.8f;
inline constexpr float kSprintReleaseThreshold = 0.5f;

class SprintGesture {
public:
    // One frame. `forward` is the stick's forward axis exactly as
    // `PlayerInput::forward` takes it -- positive forward, -1..1 -- and `now` is
    // any wall clock in seconds that only goes up. Returns whether the player
    // is sprinting.
    bool update(float forward, float now)
    {
        if (pressed_) {
            // **Letting go is what ends a sprint**, and it is the only thing
            // here that does. Beta also ends it on hunger and on a horizontal
            // collision; there is no hunger in a1.1.2, and stopping dead at
            // every wall brush is worse on a stick than on a keyboard.
            if (forward < kSprintReleaseThreshold) {
                pressed_ = false;
                sprinting_ = false;
            }
            return sprinting_;
        }

        if (forward < kSprintPressThreshold) {
            return sprinting_;
        }

        pressed_ = true;
        if (lastPress_ >= 0.0f && now - lastPress_ < kSprintTapSeconds) {
            sprinting_ = true;
            // Consumed, so the third tap of a triple starts a fresh gesture
            // rather than re-triggering off the second.
            lastPress_ = -1.0f;
        } else {
            lastPress_ = now;
        }
        return sprinting_;
    }

    // Stops the sprint without waiting for the stick, and forgets the tap
    // behind it. Sneaking does this, and so does anything that takes the stick
    // away -- a focused bottom screen, a pause. Resuming needs a fresh double
    // tap, which is what stops a sprint surviving a menu the player forgot they
    // were in.
    void cancel()
    {
        sprinting_ = false;
        lastPress_ = -1.0f;
    }

    bool sprinting() const { return sprinting_; }

private:
    bool pressed_ = false;
    bool sprinting_ = false;
    // Negative means "no tap is pending", which is the state a consumed or
    // cancelled gesture goes back to. A clock that starts at zero is why this
    // cannot be zero.
    float lastPress_ = -1.0f;
};

}  // namespace mc::entity
