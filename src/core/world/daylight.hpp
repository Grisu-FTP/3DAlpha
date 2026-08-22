#pragma once

// a1.1.2's day/night, transcribed from the client jar rather than eyeballed.
//
// Alpha does not dim the world smoothly. It computes one **integer 0..11** from
// the time of day and subtracts it from every block's stored sky light, then
// takes the brighter of that and the block light and looks the result up in a
// fixed 16-entry brightness table. Everything visible about dusk falls out of
// those three steps, including the stepping itself -- the world darkens in
// eleven discrete jumps over about eighty seconds, and that is what the
// original looks like.
//
// The shape this replaced was a squared sine, invented, and it was wrong in the
// way that matters most: it peaked only at noon, so a world saved in the
// morning rendered at a third of its brightness. Alpha is at **full brightness
// for half the day** -- ticks 0 to 12040 -- and only then begins to fall.
//
// Recovered from `cn.class` (World) in minecraft-a1.1.2_01-client.jar:
//
//   celestialAngle       cn.c(F)F      -- getCelestialAngle
//   skyLightSubtracted   cn.a(F)I      -- calculateSkylightSubtracted
//   lightBrightness      cn.<clinit>   -- the float[16] built into field `i`
//
// Obfuscated names are version-specific; these are for a1.1.2_01 only.

#include "core/util/types.hpp"

namespace mc::world {

// Where the sun is, as a fraction of a full circle. Not a linear ramp: Alpha
// bends the raw fraction a third of the way towards a cosine ease, which is
// what makes the sun hang near the horizon and cross the sky quickly.
float celestialAngle(i64 timeTicks, float partialTicks = 0.0f);

// How much to take off every block's stored sky light, 0 at noon through 11 at
// night. An integer, deliberately: the steps are visible in the original.
int skyLightSubtracted(i64 timeTicks, float partialTicks = 0.0f);

// Alpha's brightness table for a final light level 0..15. 0.05 at black --
// the ambient floor that keeps caves readable -- rising to 1.0 at level 15,
// and **monochrome**: a1.1.2 has one table, so torchlight is not warmer than
// sunlight. Any tint in a lightmap built from this is invention.
float lightBrightness(int level);

// The final level a block renders at, before the table: sky light less the
// day's subtraction, or block light, whichever is brighter.
inline int effectiveLightLevel(int skyLight, int blockLight, int subtracted)
{
    const int sky = skyLight - subtracted;
    const int lit = sky > blockLight ? sky : blockLight;
    return lit < 0 ? 0 : (lit > 15 ? 15 : lit);
}

}  // namespace mc::world
