#pragma once

// **The water over the view while the head is under it** -- `jh.c(F)V`, the
// last of `ItemRenderer.renderOverlays` (`jh.b(F)V`), which runs it when
// `player.isInsideOfMaterial(Material.water)`. The port put the fog on and drew
// nothing over the screen; this is the sheet of `water.png` the original lays
// over the world, the hand and the flames.
//
// One quad in camera space with the modelview at identity, and all of it is:
//
//     float b = player.getEntityBrightness(partialTicks);
//     glColor4f(b, b, b, 0.5F);                       // blended
//     float du = -player.rotationYaw / 64.0F;
//     float dv = player.rotationPitch / 64.0F;
//     (-1, -1, -0.5)  uv (4 + du, 4 + dv)
//     ( 1, -1, -0.5)  uv (0 + du, 4 + dv)
//     ( 1,  1, -0.5)  uv (0 + du, 0 + dv)
//     (-1,  1, -0.5)  uv (4 + du, 0 + dv)
//
// Two units across at half a block from the eye is well past the edge of the
// screen at any field of view the options offer, so the quad simply covers the
// view. The image repeats four times across it, **mirrored in u**, and slides
// a sixty-fourth of a repeat for every degree the player turns -- which is what
// makes the water seem to stay still while the head moves.
//
// **The UVs are into the 64 x 64 sheet** core/texture/water_overlay_image.hpp
// builds, where one unit is all four repeats. The scroll is reduced to its
// fraction of one repeat first: yaw is not wrapped and grows without bound,
// and the texture repeats every whole unit of the original's UV anyway.
//
// **The brightness is the vertex's light byte**, not its colour: the detail
// shader looks the byte up in the same lightmap the world uses, which is
// `getLightBrightness` at that cell with the day's darkening already in it --
// exactly `getEntityBrightness`. The 0.5 is not in the vertex either (the
// vertex alpha carries the fog amount); the platform applies
// `kWaterOverlayAlpha` in the combiner, as the fire overlay does its 0.9.

#include "core/mesh/vertex.hpp"

namespace mc::render {

inline constexpr int kWaterOverlayVertices = 4;

// `glColor4f(b, b, b, 0.5F)`.
inline constexpr float kWaterOverlayAlpha = 0.5f;

// The quad, in the detail format, for a player looking along `yawDegrees` /
// `pitchDegrees` (a1.1.2's `rotationYaw` / `rotationPitch`) with `light`
// packed `(sky << 4) | block` at the player's `getEntityBrightness` cell.
// Returns 4, or 0 when `max` cannot hold it.
int buildWaterOverlayQuad(float yawDegrees, float pitchDegrees, u8 light,
                          mesh::DetailVertex* out, int max);

}  // namespace mc::render
