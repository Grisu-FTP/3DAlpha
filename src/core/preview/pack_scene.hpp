#pragma once

// **The little scene the Texture Pack screen draws**: a patch of grass with a
// tree, a sand corner, a workbench and a few of the blocks a pack is judged by,
// meshed once and drawn with whichever pack the cursor is on.
//
// The geometry never depends on the pack -- a pack is a texture, and the tiles
// each face samples come from the block table -- so switching packs is binding
// a different atlas under the same vertices. That is the whole of why scrolling
// the pack list can swap the scene with no delay: the atlases either side of
// the cursor are decoded in the background, and the scene itself is built here
// exactly once.
//
// Only cubes, and nothing whose texture is generated rather than read: water,
// lava and fire are `TextureFX` tiles in a1.1.2, identical in every pack.

#include "core/mesh/mesher.hpp"

namespace mc::preview {

// Blocks across the scene on both horizontal axes, and how tall it gets.
inline constexpr int kPackSceneEdge = 6;
inline constexpr int kPackSceneHeight = 5;

// Meshes the scene into `out`'s detail stream, block (0, 0, 0) at the origin.
// Faces against an opaque neighbour inside the scene are left out, the way the
// world's mesher leaves them out.
void buildPackScene(mesh::MeshBuilder* out);

}  // namespace mc::preview
