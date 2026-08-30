#pragma once

// The 20 Hz clock: how many whole world ticks a rendered frame owes, and how
// far between two of them the frame is being drawn.
//
// Transcribed from `ir.class` (Timer) in minecraft-a1.1.2_01-client.jar. The
// shape matters more than it looks: the game does **not** run one tick per
// frame. It accumulates real time into a float count of ticks, runs the whole
// part of it, and hands the fraction to the renderer as `partialTicks` so that
// entities and the sky interpolate smoothly between two simulation steps at
// any frame rate. Getting that wrong is what makes a port feel like a
// different game -- a tick-per-frame port runs at half speed on a 3DS and at
// triple speed on a desktop.
//
// Three details are load-bearing and all three are in the original:
//
//   * The per-call delta is clamped to **one second** before it is scaled, so
//     a stall cannot inject a hundred ticks at once.
//   * The whole part is taken, subtracted from the accumulator, and only
//     *then* clamped to **10**. Ticks beyond the tenth are dropped, not
//     deferred: a1.1.2 lets the world fall behind rather than spiral, and a
//     console that misses a frame should behave the same way.
//   * The fraction survives across calls, which is what keeps the day length
//     right even when the frame rate is not a divisor of 20.
//
// **One deviation, deliberate.** The original also corrects `System.nanoTime`
// against `System.currentTimeMillis` once a second and folds the ratio into a
// smoothed `timeSyncAdjustment`, because those are two different clocks on a
// 2010 PC and the fast one drifts. A 3DS has one clock -- `svcGetSystemTick`
// off a fixed 268 MHz oscillator -- and the host harness has
// `std::chrono::steady_clock`. There is no second clock to correct against, so
// the correction is not implemented rather than implemented as a no-op; a
// smoothing filter over a ratio that is always exactly 1.0 is not fidelity.
// Everything the adjustment multiplies is otherwise identical.

#include "core/util/types.hpp"

namespace mc::tick {

class TickTimer {
public:
    // 20.0 in every version this project targets. It is a parameter because
    // the original made it one, and because a test that wants a tick per call
    // says so rather than sleeping.
    explicit TickTimer(float ticksPerSecond = 20.0f) : ticksPerSecond_(ticksPerSecond) {}

    // Hand it a monotonic clock in seconds. The first call establishes the
    // origin and owes nothing, which is what stops a world that took four
    // seconds to load from running eighty ticks on its first frame.
    void advance(double nowSeconds);

    // Whole ticks the world owes for the frame just begun: 0..10.
    int elapsedTicks() const { return elapsed_; }

    // How far past the last whole tick we are, 0.0 .. 1.0. The renderer's
    // interpolation factor.
    float partialTicks() const { return partial_; }

    // The original's `timerSpeed`, which nothing in a1.1.2 changes and which
    // exists here for the same reason `ticksPerSecond` is a parameter.
    void setSpeed(float speed) { speed_ = speed; }

    // Ticks the clamp threw away since the world opened. Not part of the
    // original -- it is how "the console is behind" stops being invisible.
    i64 droppedTicks() const { return dropped_; }

private:
    float ticksPerSecond_;
    float speed_ = 1.0f;
    float accumulator_ = 0.0f;
    double lastSeconds_ = 0.0;
    bool started_ = false;
    int elapsed_ = 0;
    float partial_ = 0.0f;
    i64 dropped_ = 0;
};

}  // namespace mc::tick
