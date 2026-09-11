#pragma once

// **A falling block as geometry** -- `RenderFallingSand.doRender`, which is one
// call to `RenderBlocks.renderBlockFallingSand` and nothing else.
//
// It sits beside core/render/item_entity_mesh.hpp and follows the same rules:
// the caller owns the storage, nothing here allocates, the geometry comes out
// relative to an origin the caller picks, and the vertex written is
// `mesh::DetailVertex` -- so a block on its way down rides the shader that
// already lights and fogs the world.
//
// **It is drawn as a full cube, at full size, and not spun.** That is the whole
// difference from a dropped item: a dropped block is a quarter size and turns,
// because it is an *item* being shown as a block; this one is the block itself,
// mid-move. So there is no bob, no spin and no stacking -- one cube per entity.
//
// **The face shading is the world's**, `mesh::kFaceShade`, and not the flat 255
// an item entity uses. A falling sand block passes within a few pixels of the
// static ones beside it, and one of them lit differently from the other is the
// kind of small wrongness that reads as a rendering bug.
//
// The one thing the original does that this does not is draw the block through
// `renderBlockByRenderType`, so a falling block of some shape other than a cube
// would come out that shape. Sand and gravel are the only two blocks in a1.1.2
// that fall and both are cubes; a version whose falling block is not would need
// the shape table here, and there is a `static_assert` in the source saying so
// rather than a comment.

#include "core/entity/falling_block.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Six faces of four vertices.
inline constexpr int kFallingBlockVerticesEach = 6 * 4;

// **What one frame draws**, which is not what exists: the pool has no cap,
// and past this many in range the nearest are drawn -- see draw_budget.hpp.
inline constexpr int kFallingBlockDrawBudget = 64;
inline constexpr int kFallingBlockMaxVertices =
    kFallingBlockDrawBudget * kFallingBlockVerticesEach;

// Fills `out` with the quads for every live entity -- the nearest, when `max`
// cannot hold them all -- and returns how many vertices it wrote. `partial` is
// the fraction of a tick elapsed.
//
// An entity further from the origin than a 16-bit detail position can express
// is **skipped rather than clamped**, exactly as an item and a particle are: a
// clamped one would be a block stuck to the edge of the world.
int buildFallingBlocks(const entity::FallingBlockSystem& system, double originX,
                       double originY, double originZ, float partial,
                       mesh::DetailVertex* out, int max);

}  // namespace mc::render
