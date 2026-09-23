#pragma once

// **What the fog does when the player's head is in water or lava**, and the
// fog brightness that dims it every other time -- the two parts of
// `EntityRenderer` that `daylight.hpp`'s viewFogColour leaves out.
//
// Recovered from `iq.class` in minecraft-a1.1.2_01-client.jar:
//
//   FogBrightness::tick   iq.a()V       -- updateRenderer: `n = o; o += ...`
//   viewFog (colour)      iq.i(F)V      -- updateFogColor, after the sky lerp
//   viewFog (density)     iq.a(I)V      -- setupFog
//
// **Under water the fog stops being a line and becomes a curve.** Above the
// surface `setupFog` asks for `GL_LINEAR` from a quarter of the far plane to
// the far plane. With the head in water it asks for `GL_EXP` at density 0.1 --
// `1 - e^(-0.1 * d)` -- and in lava `GL_EXP` at 2.0, and the colour is replaced
// outright: (0.02, 0.02, 0.2) in water, (0.6, 0.1, 0.0) in lava. Neither
// depends on the time of day or the render distance; both are then scaled by
// the fog brightness below, which is what makes a deep sea go black.
//
// **The fog brightness is the light at the player's head, eased.** Every tick
// `iq.a()` reads `getLightBrightness` at the player's cell, lifts it towards 1
// by an amount that grows with the render distance -- `(3 - option) / 3`, so
// that at Far it is always 1 and the fog never darkens at all, and at Tiny it
// is the light and nothing else -- and moves a tenth of the way there. The
// fog colour, the clear colour and so the whole horizon are multiplied by it,
// in water and out of it: this is what makes the fog go dark in a cave. It
// starts at zero, so a world fades in from black over the first second or two.
//
// The anaglyph branch `iq.i(F)` ends with is not reproduced; this port has no
// anaglyph mode.

#include "core/util/types.hpp"
#include "core/world/daylight.hpp"

namespace mc::world {

// What the camera is inside, in the order `iq` asks: water first, then lava.
enum class FogMedium : u8 { Air, Water, Lava };

// `iq.n` and `iq.o`, the fog brightness a tick ago and now.
struct FogBrightness {
    float previous = 0.0f;
    float current = 0.0f;

    // `iq.a()V`, once a tick. `lightBrightness` is `cn.c(III)F` at the floor
    // of the player's position -- world::lightBrightness of the cell's level.
    void tick(float lightBrightness, int renderDistanceChunks);

    // `n + (o - n) * partialTicks`, which is what `iq.i(F)` multiplies by.
    float at(float partialTicks) const
    {
        return previous + (current - previous) * partialTicks;
    }
};

// `GL_EXP` densities from `iq.a(I)V`, for the two media that use one.
inline constexpr float kWaterFogDensity = 0.1f;
inline constexpr float kLavaFogDensity = 2.0f;

struct ViewFog {
    SkyColour colour;
    // The `GL_EXP` density, or **0 for the ordinary linear fog**.
    float density;
};

// The fog colour and mode for one frame: viewFogColour's lerp, or the medium's
// flat colour in its place, times `brightness` (FogBrightness::at).
ViewFog viewFog(i64 timeTicks, float partialTicks, int renderDistanceChunks, FogMedium medium,
                float brightness);

}  // namespace mc::world
