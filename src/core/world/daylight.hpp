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
//   skyColour            cn.b(F)Laj;   -- getSkyColor
//   fogColour            cn.e(F)Laj;   -- getFogColor
//   starBrightness       cn.f(F)F      -- getStarBrightness
//   viewFogColour        iq.h(F)V      -- EntityRenderer.updateFogColor
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

// **The three colours of the sky, and none of them depends on where you are.**
//
// a1.1.2 predates biome tint entirely: `getSkyColor` reads one *world* field --
// `cn.D`, initialised to 8961023 and never written again -- and scales it by
// the time of day. The per-biome sky and the temperature lookup that later
// versions mix in are not in this class file, so a sky that changed colour with
// the ground under it would be invention. Three base colours, packed 0xRRGGBB:
//
//   sky    cn.D   8961023   = 0x88BBFF
//   fog    cn.E   12638463  = 0xC0D8FF
//   cloud  cn.F   16777215  = 0xFFFFFF
//
// All three are dimmed by the same daylight fraction, and the fraction is the
// one thing worth naming: `cos(celestialAngle * 2pi) * 2 + 0.5`, clamped to
// 0..1. It is **flat at 1 for most of the day** -- the cosine only has to reach
// 0.25 -- and it falls to 0 over the same eighty seconds the block light steps
// down in, which is why dusk is a fade in the sky and a staircase on the ground.
struct SkyColour {
    float r, g, b;
};

// cn.b(F): the base sky colour scaled by the daylight fraction. Black at night
// rather than dark blue -- what keeps a night sky from being a void is the
// stars and the horizon, not this.
SkyColour skyColour(i64 timeTicks, float partialTicks = 0.0f);

// cn.e(F): the world's fog colour, which never goes fully black -- the fraction
// is applied as `f * 0.94 + 0.06` on red and green and `f * 0.91 + 0.09` on
// blue, so midnight fog keeps a sixteenth of its blue. This is the colour
// *before* the render distance has its say; see viewFogColour.
SkyColour fogColour(i64 timeTicks, float partialTicks = 0.0f);

// cn.f(F): how brightly the stars are drawn, 0 by day. `1 - (cos * 2 + 0.75)`
// clamped, then squared and halved -- so they appear later than the sky
// darkens, ramp in over a few seconds, and never exceed half brightness.
float starBrightness(i64 timeTicks, float partialTicks = 0.0f);

// iq.h(F): what `glFogfv(GL_FOG_COLOR)` and `glClearColor` actually get.
//
// The fog colour lerped **towards the sky colour** by an amount that depends
// only on the render distance: `1 - pow(1 / (4 - renderDistance), 0.25)`, which
// is 0.293 at Far, 0.240 at Normal, 0.159 at Short and **exactly 0 at Tiny**.
// The nearer the fog, the less of the sky is in it -- at the shortest setting
// the fog is `cn.E` and nothing else.
//
// **The original's `renderDistance` is one of four settings and this port's is
// a chunk count**, so the option is recovered as `4 - log2(chunks)`: 16 chunks
// is Far, 8 Normal, 4 Short, 2 Tiny, and everything between lands where it
// belongs on the same curve. Clamped to the four settings' range, because the
// expression divides by `4 - option`.
SkyColour viewFogColour(i64 timeTicks, float partialTicks, int renderDistanceChunks);

}  // namespace mc::world
