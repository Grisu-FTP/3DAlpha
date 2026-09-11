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
// The vertex written is `mesh::DetailVertex` -- the same 16-byte format the
// torches and the fluids use -- so particles ride the shader that already
// lights and fogs the world, and the atlas is already bound. Four vertices per
// particle, in the order the shared index buffer expects.

#include "core/entity/particle.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `EntityFX.renderParticle`'s own `0.1F * particleScale`, the half-width of the
// quad in blocks.
inline constexpr float kParticleQuadScale = 0.1f;

// **`0.015609375F`**, and it is not `1/64`. A digging particle shows a quarter
// of a 16x16 tile -- 1/64 of the atlas -- and the original steps *slightly*
// less than that, which keeps the sample off the seam with the quarter next
// door. Copied as the literal it is.
inline constexpr float kParticleUvSpan = 0.015609375f;

// A camera basis: `right` spans the quad horizontally and `up` vertically.
struct Billboard {
    float rightX, rightY, rightZ;
    float upX, upY, upZ;
};

// Fills `out` with `4 * min(system.count(), max / 4)` vertices and returns how
// many it wrote. `partial` is the fraction of a tick elapsed, as everything
// else in the frame path uses it; `origin` is subtracted in double before
// anything becomes a float.
//
// A particle further from the origin than the 16-bit position can express is
// **skipped rather than clamped**: a clamped one would be a fleck stuck to the
// edge of the world, which is worse than a missing one.
int buildParticles(const entity::ParticleSystem& system, const Billboard& camera,
                   double originX, double originY, double originZ, float partial,
                   mesh::DetailVertex* out, int max);

}  // namespace mc::render
