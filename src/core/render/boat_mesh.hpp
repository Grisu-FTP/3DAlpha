#pragma once

// **A boat as geometry** -- `cl` (ModelBoat) through `cp` (RenderBoat), and the
// first thing in this project drawn by core/render/box_model.hpp.
//
// **Five boxes, and four of them are the same box.** The hull is one 24 x 16 x 4
// plate stood on its back by a quarter turn about x, and the four sides are one
// 20 x 6 x 2 plank placed four times at four different rotation points and four
// different quarter-turns about y. That is why `ModelRenderer` holding exactly
// one box costs nothing: a model is a list of parts, not a list of boxes per
// part.
//
// The numbers are `cl`'s constructor read literally -- it builds them from
// `i = 24`, `j = 6`, `k = 20` and `l = 4`, and every coordinate below is one of
// those four in an expression.
//
// **`glScalef(-1.0F, -1.0F, 1.0F)`**, which `RenderBoat` applies before the
// model and which every Minecraft entity model gets: the models are authored
// upside down and mirrored, and this is what puts them the right way up. It is
// folded into the placement's axes here rather than applied per vertex.
//
// **The hull rocks when the boat is damaged.** `RenderBoat` rotates by
// `sin(timeSinceHit) * timeSinceHit * damage / 10 * forwardDirection` about x.
// Nothing in this build damages a boat, so the rotation is transcribed and is
// always zero -- see core/entity/boat.hpp.
//
// One thing in `RenderBoat` is **not** transcribed and is worth naming so it is
// not looked for: it binds `/terrain.png`, scales by 0.75, scales by 1/0.75 and
// then binds `/item/boat.png`. The two scales cancel and the first bind is
// overwritten before anything is drawn. It is dead code in the class file.

#include "core/entity/boat.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Five parts, six quads each.
inline constexpr int kBoatParts = 5;
inline constexpr int kBoatVerticesEach = kBoatParts * 6 * 4;
// **What one frame draws**, which is not what exists: the pool has no cap,
// and past this many in range the nearest are drawn -- see draw_budget.hpp.
inline constexpr int kBoatDrawBudget = 32;
inline constexpr int kBoatMaxVertices = kBoatDrawBudget * kBoatVerticesEach;

// Fills `out` with the quads for every live boat -- the nearest, when `max`
// cannot hold them all -- and returns how many vertices it wrote. `partial` is the fraction of a tick elapsed, used for the position
// and the yaw exactly as `doRender` uses it.
int buildBoats(const entity::BoatSystem& system, double originX, double originY,
               double originZ, float partial, mesh::DetailVertex* out, int max);

}  // namespace mc::render
