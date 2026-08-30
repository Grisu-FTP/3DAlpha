#include "core/tick/tick_timer.hpp"

namespace mc::tick {

void TickTimer::advance(double nowSeconds)
{
    if (!started_) {
        started_ = true;
        lastSeconds_ = nowSeconds;
        elapsed_ = 0;
        return;
    }

    double delta = nowSeconds - lastSeconds_;
    lastSeconds_ = nowSeconds;

    // The original clamps here, before scaling, so the bound is one second of
    // real time rather than twenty ticks of world time. They are the same
    // number today and would stop being the same the moment a version changes
    // its tick rate.
    if (delta < 0.0) delta = 0.0;
    if (delta > 1.0) delta = 1.0;

    accumulator_ = float(double(accumulator_) + delta * double(speed_) * double(ticksPerSecond_));

    // Truncation, then subtraction, then the cap -- in that order. Capping
    // first would defer the surplus to the next frame; the original discards
    // it, and a world that is behind stays behind.
    elapsed_ = int(accumulator_);
    accumulator_ -= float(elapsed_);
    if (elapsed_ > 10) {
        dropped_ += elapsed_ - 10;
        elapsed_ = 10;
    }
    partial_ = accumulator_;
}

}  // namespace mc::tick
