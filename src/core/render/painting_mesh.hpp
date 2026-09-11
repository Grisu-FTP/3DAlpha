#pragma once

// **A painting as geometry** -- `bw.a(Ljc;IIII)V`, which is
// `RenderPainting.renderPainting`.
//
// It follows the rules core/render/item_entity_mesh.hpp and
// core/render/falling_block_mesh.hpp already set: the caller owns the storage,
// nothing here allocates, the geometry comes out relative to an origin the
// caller picks, and the vertex written is `mesh::DetailVertex` -- so a painting
// rides the shader that already lights and fogs the world.
//
// **A painting is a box, but not a box model.** It does not go through
// core/render/box_model.hpp, and that is the class file's decision rather than
// a shortcut: `RenderPainting` emits its quads directly because it needs
// something `ip` cannot do, which is to **subdivide the canvas into 16 x 16
// cells and light each one separately**. A four-block picture spanning a
// doorway is bright at one end and dark at the other, and one box would be
// flat.
//
// So the geometry is six faces *per cell*:
//
//   * the **front**, out of `art/kz.png` at the picture's own rectangle;
//   * the **back**, and the four **edges**, all out of one 16 x 16 tile of the
//     same sheet at texels (192, 0) -- the back-of-canvas wood. The edges are
//     degenerate on one axis, sampling a single line of it, which is the
//     original's own arithmetic and not a bug here.
//
// **Everything is in model units of 1/16 block**, because `doRender` does
// `glScalef(0.0625F, 0.0625F, 0.0625F)` before calling this. The canvas is one
// model unit thick -- z from -0.5 to +0.5 -- which is a thirty-second of a
// block, matching the bounding box `setPaintingDirection` builds.
//
// **The art sheet is its own texture** and is bound for this pass alone. See
// core/texture/entity_skins.hpp: the four small entity sheets share a 128 x 64
// page sheet and `kz.png` does not, because it is already 256 x 256 and `er`
// indexes it in absolute texels.

#include "core/entity/painting.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// The art sheet's edge, which is `er`'s own coordinate space.
inline constexpr int kArtSheetEdge = 256;

// Where the back and the four edges take their texels from: the 16 x 16 tile at
// (192, 0) of the art sheet. These are the class file's literals -- `0.75F`,
// `0.8125F`, `0.0F`, `0.0625F` -- written as the texels they are.
inline constexpr int kCanvasBackU = 192;
inline constexpr int kCanvasBackV = 0;
inline constexpr int kCanvasBackSize = 16;

// Six faces of four corners per 16 x 16 cell, and at most sixteen cells.
inline constexpr int kPaintingMaxVertices = entity::Painting::kMaxCells * 6 * 4;

// Fills `out` with the quads for every live painting and returns how many
// vertices it wrote.
//
// A painting further from the origin than a 16-bit detail position can express
// is **skipped rather than clamped**, exactly as an item entity is: a clamped
// one would be a picture stuck to the edge of the world.
int buildPaintings(const entity::PaintingSystem& system, double originX, double originY,
                   double originZ, mesh::DetailVertex* out, int max);

}  // namespace mc::render
