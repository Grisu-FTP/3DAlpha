#include "core/settings/sensitivity.hpp"

#include <cstdio>

namespace mc::settings {

int clampSensitivity(int percent)
{
    if (percent < kMinSensitivity) {
        return kMinSensitivity;
    }
    if (percent > kMaxSensitivity) {
        return kMaxSensitivity;
    }
    // To a whole step, so a hand-edited 97 does not leave the row unable to
    // reach either end or 100% again.
    const int steps = (percent + kSensitivityStep / 2) / kSensitivityStep;
    return steps * kSensitivityStep;
}

float sensitivityGain(int percent)
{
    // The jar's `fr.c`, rebuilt from the percentage the row shows: `c` is 0..1
    // and the label is `c * 200`.
    const float c = float(clampSensitivity(percent)) / float(kMaxSensitivity);
    const float f = c * 0.6f + 0.2f;
    return f * f * f * 8.0f;
}

void sensitivityLabel(int percent, char* out, usize size)
{
    const int value = clampSensitivity(percent);
    // **The jar's two named ends**, which are the labels and not a flourish:
    // `fr` returns them instead of a number when the slider is hard against
    // either stop.
    if (value == kMinSensitivity) {
        std::snprintf(out, size, "*yawn*");
        return;
    }
    if (value == kMaxSensitivity) {
        std::snprintf(out, size, "HYPERSPEED!!!");
        return;
    }
    std::snprintf(out, size, "%d%%", value);
}

}  // namespace mc::settings
