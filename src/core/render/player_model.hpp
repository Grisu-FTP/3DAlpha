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
// And between the limbs and the sway, **the swing block**, on `fo.k` -- the
// swing progress, 0 to 1 -- read out of the same method:
//
//     body   angleY  = sin(sqrt(k) * pi * 2) * 0.2
//     armR   point   = (-cos(body.angleY) * 5, _,  sin(body.angleY) * 5)
//     armL   point   = ( cos(body.angleY) * 5, _, -sin(body.angleY) * 5)
//     armR   angleY += body.angleY;  armL angleY += body.angleY;  armL angleX += body.angleY
//     t = 1 - (1 - k)^4
//     armR   angleX -= sin(t * pi) * 1.2 + sin(k * pi) * -(head.angleX - 0.7) * 0.75
//     armR   angleY += body.angleY * 2
//     armR   angleZ  = sin(k * pi) * -0.4
//
// At zero it contributes nothing but the arms' rotation points, which it puts
// back at x = -5 and 5 -- where the constructor already has them.
//
// Then **the sneak branch**, on `cr.j`, which `bu` sets from `dm.o()`:
//
//     body   angleX  = 0.5                       (standing: 0)
//     armR   angleX += 0.4;  armL angleX += 0.4
//     legR, legL     point = (_, 9, 4)           (standing: (_, 12, 0))
//     head   point   = (_, 1, _)                 (standing: (_, 0, _))
//
// In a1.1.2 it is dead code -- `o()` is a hard-wired false -- and this port
// sneaks, so it is live here. Its legs end three pixels short of the ground;
// b1.2's `RenderPlayer` lowers a sneaking model by 0.125 of a block to meet
// it, which is the placement's business rather than the pose's. The ride and
// held-item branches are off.

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
                float headYaw, float headPitch, float swingProgress = 0.0f,
                bool sneaking = false);

// b1.2's `RenderPlayer`: a sneaking body is drawn this much lower, in blocks.
inline constexpr double kSneakModelDrop = 0.125;

// The whole player at `place`, textured from a 64 x 32 page whose top-left
// texel is (`pageX`, `pageY`) in a 256 x 256 sheet -- the entity sheet's own
// size, so the UVs are `buildBox`'s moved to another page. Returns the vertex
// count, 0 when `max` cannot hold all of them.
int buildPlayerPreview(const ModelPart* parts, const Placement& place, int pageX, int pageY,
                       mesh::DetailVertex* out, int max);

}  // namespace mc::render
