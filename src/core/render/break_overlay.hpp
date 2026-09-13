#pragma once

// **The crack over a block being broken** -- `RenderGlobal.drawBlockBreaking`,
// `e.a(Ldm;Lmf;ILev;F)V`'s first branch.
//
// While the controller's damage is above zero the original draws the struck
// block **again, in its own shape, with every face textured from one terrain
// tile**: `240 + (int)(damagePartialTime * 10)`, the ten destroy stages in the
// last row of terrain.png. It does that through `renderBlockUsingTexture`, so a
// slab cracks as a slab and a torch as a torch; and a cell that has already gone
// to air borrows stone's cube. The colour is `(1, 1, 1, 0.5)` and the blend is
// `GL_DST_COLOR, GL_SRC_COLOR` -- twice the product of the two, which leaves a
// mid-grey texel alone and darkens where the crack is dark -- with the alpha test
// off.
//
// **One deviation, and it is forced.** The original lifts the overlay off the
// block with `glPolygonOffset(-3, -3)`. The PICA200 has no polygon offset, so
// each box is grown by `kBreakOverlayExpand` instead -- the selection outline's
// own 0.002, which is already what stops that pass z-fighting the same faces.
//
// Positions are block-local (the box's own 0..1, grown), in `DetailVertex`
// units; the renderer translates them to the block. See core/render/outline.hpp
// for the expansion and core/block/model.hpp for the shapes.

#include "core/block/block_def.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `RenderGlobal`'s 0.002f, as `render::kOutlineExpand` holds it.
inline constexpr double kBreakOverlayExpand = 0.0020000000949949026;

// The first destroy stage's tile; the stage is added to it.
inline constexpr int kBreakOverlayFirstTile = 240;
inline constexpr int kBreakOverlayStages = 10;

// Nine boxes (a fully connected fence) of six faces, four vertices each.
inline constexpr int kBreakOverlayMaxVertices = 9 * 6 * 4;

// Writes the overlay for `id` at `metadata` at crack `stage` (0..9) and returns
// the vertices written -- a multiple of four, or 0 for a stage out of range or
// a buffer too small.
int buildBreakOverlay(block::BlockId id, u8 metadata, int stage, mesh::DetailVertex* out,
                      int maxVertices);

}  // namespace mc::render
