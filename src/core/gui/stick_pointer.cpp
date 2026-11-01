// The stick as a pointer: the snap, the flick and the free move. See stick_pointer.hpp.

#include "core/gui/stick_pointer.hpp"

#include <cmath>

namespace mc::gui {

namespace {

StickPointer::Result stepFor(float x, float y)
{
    StickPointer::Result result;
    if (std::fabs(x) > std::fabs(y)) {
        result.stepX = x > 0.0f ? 1 : -1;
    } else {
        result.stepY = y > 0.0f ? 1 : -1;
    }
    return result;
}

}  // namespace

void StickPointer::placeAt(float x, float y)
{
    x_ = x;
    y_ = y;
    shown_ = true;
}

void StickPointer::reset()
{
    *this = StickPointer{};
}

StickPointer::Result StickPointer::tick(float stickX, float stickY, float dt, float minX,
                                        float minY, float width, float height)
{
    float length = std::sqrt(stickX * stickX + stickY * stickY);
    if (length < kPointerDeadzone) {
        // Let go. A flick that never got firm and was quick is one step.
        Result result;
        if (active_ && peak_ < kPointerSnapTilt && held_ < kPointerSnapHold) {
            result = stepFor(peakX_, peakY_);
        }
        active_ = false;
        return result;
    }
    if (!active_) {
        active_ = true;
        peak_ = 0.0f;
        held_ = 0.0f;
        repeat_ = 0.0f;
    }
    held_ += dt;
    if (length > peak_) {
        peak_ = length;
        peakX_ = stickX;
        peakY_ = stickY;
    }
    if (length < kPointerSnapTilt) {
        // A light tilt held on: steps repeat, and the pointer stays where the last
        // step put it. Before the hold is up it may still turn out to be a flick.
        if (held_ >= kPointerSnapHold) {
            repeat_ -= dt;
            if (repeat_ <= 0.0f) {
                repeat_ = kPointerSnapRepeat;
                return stepFor(stickX, stickY);
            }
        }
        return Result{};
    }
    // Firm: the pointer moves freely, from wherever it is.
    shown_ = true;
    const float magnitude = length < 1.0f ? length : 1.0f;
    const float step = (kPointerBaseSpeed + kPointerRampSpeed * magnitude) * dt / length;
    x_ += stickX * step;
    y_ += stickY * step;
    x_ = x_ < minX ? minX : (x_ > minX + width ? minX + width : x_);
    y_ = y_ < minY ? minY : (y_ > minY + height ? minY + height : y_);
    Result result;
    result.moved = true;
    return result;
}

}  // namespace mc::gui
