#pragma once

// **An arrow in flight as geometry** -- `gk.a(Lkg;DDDFF)V`, which is
// `RenderArrow.doRender`.
//
// It follows the rules the other entity builders here set: the caller owns the
// storage, nothing allocates, the geometry is relative to an origin the caller
// picks, and the vertex is `mesh::DetailVertex`.
//
// **Six quads, and not a box model.** `RenderArrow` emits its own quads rather
// than using `ip`, and the shape is not a cuboid:
//
//   * a **flat square cap** at the tail, drawn twice with opposite normals so
//     it is visible from both sides, and
//   * **four long fins** spanning the shaft, each turned ninety degrees about
//     the arrow's own axis from the last -- which is two crossed planes drawn
//     twice over, the shape every Minecraft arrow has ever been.
//
// **Its texture page is 32 x 32, not 64 x 32.** `item/arrows.png` is the only
// one of the four entity sheets that is square, and `RenderArrow` divides its
// UVs by 32 in both axes rather than by the 64 and 32 a box model uses. That is
// why the arrow's page in the shared sheet uses only half its slot -- see
// core/texture/entity_skins.hpp.
//
// **The wobble is the original's.** An arrow that has just struck something
// carries `arrowShake`, counted down from 7, and the renderer rolls it by
// `-sin(shake * 3) * shake` degrees about its own axis. It is the only thing
// `arrowShake` is for.
//
// The model's **+x is forward**: the tail cap sits at x = -7 and the fins span
// -8 to +8, all shifted back by 4 before the 0.05625 scale, so the tip is just
// past the entity's own position and the fletching trails it.

#include "core/entity/arrow.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `0.05625F`, the arrow's own scale -- model units to blocks, and it is not the
// 1/16 a box model uses.
inline constexpr float kArrowScale = 0.05625f;

// Two cap quads plus four fins.
inline constexpr int kArrowQuads = 6;
inline constexpr int kArrowVerticesEach = kArrowQuads * 4;
// **What one frame draws**, which is not what exists: the pool has no cap,
// and past this many in range the nearest are drawn -- see draw_budget.hpp.
inline constexpr int kArrowDrawBudget = 128;
inline constexpr int kArrowMaxVertices = kArrowDrawBudget * kArrowVerticesEach;

// Fills `out` with the quads for every live arrow -- the nearest, when `max`
// cannot hold them all -- and returns how many vertices it wrote. `partial` is the fraction of a tick elapsed, used for the position
// and both angles exactly as `doRender` uses it.
//
// An arrow further from the origin than a 16-bit detail position can express is
// **skipped rather than clamped**, as every entity here is.
int buildArrows(const entity::ArrowSystem& system, double originX, double originY,
                double originZ, float partial, mesh::DetailVertex* out, int max);

}  // namespace mc::render
