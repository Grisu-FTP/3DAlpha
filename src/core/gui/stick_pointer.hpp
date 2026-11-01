#pragma once

// A thumbstick read as a pointer over a container window.
//
// **Not a1.1.2's**, which has a mouse here; the feel is the console editions'. A light
// tilt snaps slot to slot -- a flick is one step and holding it repeats -- and a firm
// tilt moves the pointer freely, faster the further it is pushed. The caller says what
// a step and a position mean, since those are the screen's: this is only the rule, and
// the rule is worth a test. See core/gui/stick_cursor.hpp for the plain d-pad reading
// the other screens use.

namespace mc::gui {

// Under this much of a full tilt the stick steps rather than moves.
inline constexpr float kPointerSnapTilt = 0.3f;
// A tilt let go within this many seconds is a flick: one step. Longer, it repeats.
inline constexpr float kPointerSnapHold = 0.30f;
inline constexpr float kPointerSnapRepeat = 0.16f;
// Pixels a second at the first firm tilt, and how much more a full one adds. A 320-wide
// page wants less than the 176-wide window this was tuned on would suggest per pixel,
// because the page's slots are wider: 31 pixels against 18.
inline constexpr float kPointerBaseSpeed = 70.0f;
inline constexpr float kPointerRampSpeed = 170.0f;
// Inside this much of a full tilt the stick reads as centred. Sticks rest off centre.
inline constexpr float kPointerDeadzone = 0.1f;

class StickPointer {
public:
    struct Result {
        // A step for the screen to take, on one axis, with +y down the screen.
        int stepX = 0;
        int stepY = 0;
        // The pointer moved under the stick: `x()` and `y()` have changed.
        bool moved = false;
    };

    // `stickX` right and `stickY` down, each -1..1 before the deadzone. The pointer
    // stays inside `0..width` by `0..height` from (`minX`, `minY`).
    Result tick(float stickX, float stickY, float dt, float minX, float minY, float width,
                float height);

    float x() const { return x_; }
    float y() const { return y_; }
    bool shown() const { return shown_; }

    // Puts the pointer somewhere, which is where a step left it, and shows it.
    void placeAt(float x, float y);
    // Away, until the stick next moves it: the d-pad or a touch has taken over.
    void hide() { shown_ = false; }
    // A new screen: hidden, and nothing about the stick carried over.
    void reset();

private:
    float x_ = 0.0f;
    float y_ = 0.0f;
    bool shown_ = false;
    bool active_ = false;
    float peak_ = 0.0f;
    float peakX_ = 0.0f;
    float peakY_ = 0.0f;
    float held_ = 0.0f;
    float repeat_ = 0.0f;
};

}  // namespace mc::gui
