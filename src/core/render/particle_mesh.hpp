#pragma once

// **A cloud of particles as camera-facing quads** -- `EntityFX.renderParticle`,
// which is four vertices around an interpolated position and nothing else.
//
// It sits beside core/render/outline.hpp and for the same reasons: the caller
// owns the storage, nothing here allocates, and the geometry comes out relative
// to an origin the caller chooses, because a float world coordinate out at the
// Far Lands cannot hold a fraction of a block.
//
// **The quad is built from the camera's own basis**, exactly as the original
// builds it: `right` is `(cos yaw, 0, sin yaw)` and `up` is
// `(-sin yaw sin pitch, cos pitch, cos yaw sin pitch)`. The original passes
// those five products in as five floats; two vectors say the same thing and can
// be checked by eye.
//
// **One call per sheet.** `EffectRenderer` keeps its particles in one list per
// texture and binds between them (core/entity/particle.hpp); this builds one
// sheet's worth per call so the caller can lay the three spans out in one
// buffer and draw them in three, the way the dropped items already do. The
// three spans share one `DrawCutoff`, settled by whichever is handed the whole
// buffer first.
//
// **Four things a kind can change about its quad**, and all four are here
// rather than in the tick because all four are functions of the *frame*:
//
//   * the **scale ramp** -- smoke and dust swell out of nothing over the first
//     thirty-second of their life, a flame and a lava pop shrink away over the
//     whole of theirs;
//   * the **tint**, which is a multiply and not a colour;
//   * the **brightness override** -- a lava pop ignores the world's light and a
//     flame starts full bright and settles onto it;
//   * whether the tile is shown **whole or in quarters**: a sprite off
//     `particles.png` is one tile, a fleck of a block or of an item icon is a
//     quarter of one.
//
// The vertex written is `mesh::DetailVertex` -- the same 16-byte format the
// torches and the fluids use -- so particles ride the shader that already
// lights and fogs the world, and every one of the three sheets is a texture
// that pass can bind. Four vertices per particle, in the order the shared index
// buffer expects.

#include "core/entity/particle.hpp"
#include "core/mesh/vertex.hpp"
#include "core/render/draw_budget.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `EntityFX.renderParticle`'s own `0.1F * particleScale`, the half-width of the
// quad in blocks.
inline constexpr float kParticleQuadScale = 0.1f;

// **`0.0624375F`**, and it is not `1/16`. A sprite off `particles.png` shows a
// whole tile of a 16 x 16 grid and the original steps *slightly* less than a
// sixteenth, which keeps the sample off the seam with the tile next door.
// Copied as the literal it is.
inline constexpr float kParticleTileSpan = 0.0624375f;

// **`0.015609375F`**, the same idea a quarter of the size. A digging or
// breaking fleck shows a quarter of a tile -- 1/64 of the sheet -- and steps
// slightly less than that.
inline constexpr float kParticleUvSpan = 0.015609375f;

// A camera basis: `right` spans the quad horizontally and `up` vertically.
struct Billboard {
    float rightX, rightY, rightZ;
    float upX, upY, upZ;
};

// Fills `out` with four vertices for each live particle **on `sheet`** and
// returns how many it wrote. `partial` is the fraction of a tick elapsed, as
// everything else in the frame path uses it; `origin` is subtracted in double
// before anything becomes a float.
//
// A particle further from the origin than the 16-bit position can express is
// **skipped rather than clamped**: a clamped one would be a fleck stuck to the
// edge of the world, which is worse than a missing one.
int buildParticles(const entity::ParticleSystem& system, const Billboard& camera,
                   double originX, double originY, double originZ, float partial,
                   entity::ParticleSheet sheet, mesh::DetailVertex* out, int max,
                   DrawCutoff* shared = nullptr);

// The quad's half-width for a particle at this point in its life, in blocks.
//
// Exposed because it is the one piece of the frame path with a rule per kind
// behind it, and a test can pin the four ramps without a camera.
float particleQuadScale(const entity::Particle& p, float partial);

// `(sky << 4) | block` as this particle should be lit -- the world's own byte
// for nine kinds, full brightness for a lava pop, and a fade from full onto the
// world's for a flame. See `entity::ParticleLight`.
u8 particleLight(const entity::Particle& p, float partial);

}  // namespace mc::render
