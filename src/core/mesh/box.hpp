#pragma once

// One axis-aligned box, textured the way `RenderBlocks` textures a standard
// block -- and the shared floor under every shape in the game that is not a
// full cube.
//
// **The cube stream cannot express any of this**, which is why this exists.
// `WorldVertex` stores a position as three bytes of block coordinate and
// `QuadVertex` stores a face index the geometry shader rebuilds a *unit* cube
// from (core/mesh/vertex.hpp). Neither can put a corner half a block up, so a
// slab drawn through them is a full cube -- which is exactly what a slab looked
// like, and why it was reported as "a slab and a double slab both place a
// double slab". They looked identical because they were drawn identically.
//
// So a box goes down the **detail** stream, whose vertices are 1/1024 of a
// block, the same stream torches and fluids already use. That costs 16 bytes a
// vertex against 8, which is why full cubes stay on the fast path and only
// blocks that are not one come here.
//
// **The UV rule is the original's and was read out of the class file**, not
// remembered -- `bc.a` through `bc.f`, one method per face:
//
//   * top and bottom take u from the box's x range and v from its z range;
//   * the four sides take u from the horizontal axis they face across, and v
//     from the box's **y** range measured downward from the tile's top.
//
// That last one is the interesting half. The box's *top* edge samples tile row
// `minY * 16`, so a slab -- 0 to 0.5 -- shows the tile's **top** half, and a box
// sitting in the upper half of a block shows the tile's bottom half at its top
// edge. It reads as wrong until you see that this is what makes an upside-down
// slab's texture look shifted in every version of the game.
//
// The two side faces of each axis also disagree about which way u runs: -Z and
// +X reverse it, +Z and -X do not. That falls out of `kFaceCornerUV` paired
// with `kFaceCorner` -- which the cube path already draws with, so nothing here
// re-derives it -- and it is why opposite faces of a Minecraft block are mirror
// images of each other rather than copies.

#include "core/mesh/mesher.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::mesh {

// A bit per face, in `mc::mesh::Face` order.
inline constexpr int kAllBoxFaces = 0x3F;

// Emits `bounds` -- in block-local coordinates, so a full cube is 0,0,0 to
// 1,1,1 -- at block (x, y, z) of the section.
//
// `tiles` is six atlas indices in face order, the same layout `BlockDef::faces`
// has, so a block table row can be passed straight in. `faceMask` is which of
// the six to emit; a caller that has already decided a face is hidden leaves
// its bit clear. `shaded` picks between the per-face brightness table and flat
// white, because a1.1.2 draws some shapes unshaded and it is a per-shape fact
// rather than a per-face one.
//
// `mirrorMask` is a bit per face whose two u ends swap, which is what
// `RenderBlocks` does with its `flipTexture` field (`bc.c`) when a block
// answers a *negative* tile index. Only the door does that in a1.1.2, and it is
// the whole of how a door shows which side its hinge is on.
void addBox(int x, int y, int z, const AABB& bounds, const u16 tiles[6], u8 light, bool shaded,
            int faceMask, MeshBuilder& out, DetailPass pass = DetailPass::Opaque,
            int mirrorMask = 0);

// The same, with one tile on all six faces.
void addBox(int x, int y, int z, const AABB& bounds, u16 tile, u8 light, bool shaded,
            int faceMask, MeshBuilder& out, DetailPass pass = DetailPass::Opaque);

}  // namespace mc::mesh
