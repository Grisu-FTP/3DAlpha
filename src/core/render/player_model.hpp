#pragma once

// **The player's body as a model**: `cr` -- ModelBiped -- built and posed the
// way `bu` (RenderPlayer) poses it, for the main menu's Skins screen.
//
// The seven boxes were already here, file-local in mob_mesh.cpp, because a
// zombie and a skeleton are the same biped. What was not here is the *player's*
// pose. A zombie is posed by `cb`, which throws `cr`'s arm swing away and holds
// both arms out; a player is posed by `cr.a(FFFFFF)` alone, so its arms swing
// against its legs. So the model moved here, where both can share it, and the
// player's pose is transcribed beside it.
//
// **`cr.a(FFFFFF)` for a player holding nothing, standing, and not swinging**,
// read out of the jar with javap:
//
//     head   angleY = headYaw / 57.295776,  angleX = headPitch / 57.295776
//     hat    copies the head
//     armR   angleX = cos(swing * 0.6662 + pi) * 2 * amount * 0.5
//     armL   angleX = cos(swing * 0.6662)      * 2 * amount * 0.5
//     legR   angleX = cos(swing * 0.6662)      * 1.4 * amount
//     legL   angleX = cos(swing * 0.6662 + pi) * 1.4 * amount
//     armR   angleZ += cos(age * 0.09) * 0.05 + 0.05,  angleX += sin(age * 0.067) * 0.05
//     armL   angleZ -= cos(age * 0.09) * 0.05 + 0.05,  angleX -= sin(age * 0.067) * 0.05
//
// The swing-progress block between them runs with `swingProgress` at zero and
// contributes exactly nothing but the arms' rotation points, which it puts
// back at x = -5 and 5 -- where the constructor already has them. The sneak,
// ride and held-item branches are off.

#include "core/mesh/vertex.hpp"
#include "core/render/box_model.hpp"
#include "core/util/types.hpp"

namespace mc::render {

enum BipedPart {
    kBipedHead = 0,
    kBipedHat,
    kBipedBody,
    kBipedArmRight,
    kBipedArmLeft,
    kBipedLegRight,
    kBipedLegLeft,
    kBipedParts,
};

inline constexpr int kPlayerPreviewVertices = kBipedParts * kBoxVertices;

// `cr(0, 0)` -- the seven boxes, unposed.
void bipedModel(ModelPart* out);

// `cr.a(FFFFFF)` as a player is posed by it; see the header.
void posePlayer(ModelPart* parts, float limbSwing, float limbAmount, float ageTicks,
                float headYaw, float headPitch);

// The whole player at `place`, textured from a 64 x 32 page whose top-left
// texel is (`pageX`, `pageY`) in a 256 x 256 sheet -- the entity sheet's own
// size, so the UVs are `buildBox`'s moved to another page. Returns the vertex
// count, 0 when `max` cannot hold all of them.
int buildPlayerPreview(const ModelPart* parts, const Placement& place, int pageX, int pageY,
                       mesh::DetailVertex* out, int max);

}  // namespace mc::render
