#pragma once

// How fast the view turns, as a1.1.2's own slider.
//
// **The curve is the jar's, read off `iq.a(F)`** -- `EntityRenderer`'s camera
// update, where the mouse delta is scaled before it reaches the player:
//
//     float f  = settings.c * 0.6F + 0.2F;   // fr.c, the Sensitivity slider
//     float f1 = f * f * f * 8.0F;
//     turn(mouse.dx * f1, mouse.dy * f1);
//
// `fr.c` is 0..1 and defaults to **0.5**, and the label `fr` prints for it is
// `(int)(c * 200)` with a `%` -- so the slider a player sees runs 0% to 200%
// and sits at 100%. The two ends have names rather than numbers: `*yawn*` at
// zero and `HYPERSPEED!!!` at the top.
//
// **At 100% the gain is exactly 1.0**: `0.5 * 0.6 + 0.2 = 0.5`, and
// `0.5^3 * 8 = 1`. That is the whole reason this port can adopt the curve
// unchanged -- the touch and C-stick rates in `platform/ctr/main.cpp` were
// tuned on this console and they stay exactly what they were, with the slider
// scaling around them. The ends work out at 6.4% and 409.6%.
//
// It is core rather than platform code because it is a rule derived from the
// jar, and a rule derived from the jar is one worth a test: the console cannot
// run one. See docs/architecture.md.

#include "core/util/types.hpp"

namespace mc::settings {

// The slider's own units, which are the ones a1.1.2 prints.
inline constexpr int kMinSensitivity = 0;
inline constexpr int kMaxSensitivity = 200;
inline constexpr int kDefaultSensitivity = 100;

// What Left and Right move it by. Not the jar's: `fu` is dragged with a mouse
// and has no step at all, so this is the port's, picked to reach both ends and
// 100% exactly.
inline constexpr int kSensitivityStep = 5;

// Anything to whole steps inside the range. A value out of an edited `3ds.ini`
// goes through here before it is used or shown.
int clampSensitivity(int percent);

// **What the look rate is multiplied by.** 1.0 at `kDefaultSensitivity`, so a
// caller that has not been told anything behaves exactly as it did before this
// slider existed.
float sensitivityGain(int percent);

// a1.1.2's own text for the row's value: `*yawn*`, `HYPERSPEED!!!`, or the
// percentage. The buffer is the caller's because this runs on a console with no
// spare heap in a menu frame.
void sensitivityLabel(int percent, char* out, usize size);

}  // namespace mc::settings
