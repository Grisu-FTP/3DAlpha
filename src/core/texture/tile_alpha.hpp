#pragma once

// Which tiles of a terrain atlas have holes: texels a1.1.2's alpha test drops.
//
// Two optimisations are only exact while a tile has none, and both are about
// what a texture pack chose to do, so neither can be answered from the block
// table. A solid cube drawn with the alpha test off would show a hole as a
// black texel, and the block under a snow layer is only hidden if the snow has
// no holes to see it through. The vanilla atlas has no such tile; the checks
// exist so that a pack which does gets exactly the picture it always got.

#include "core/util/types.hpp"

namespace mc::texture {

// `glAlphaFunc(GL_GREATER, 0.1F)` over eight bits: 25/255 is under 0.1 and
// 26/255 is over it.
inline constexpr u8 kAlphaTestDropsAtOrBelow = 25;

// `atlasRgba` is kAtlasBytes of R,G,B,A, top row first.
bool tileHasHoles(const u8* atlasRgba, int tile);

// Whether any tile the cube atlas treats as solid has a hole, in which case the
// solid part of the cube range still needs the alpha test.
bool solidCubeTilesHaveHoles(const u8* atlasRgba);

// Whether this pack's snow tile has a hole, in which case a snow layer does not
// hide the block under it. See mesh::MeshBuilder::setSnowHidesFloor.
bool snowLayerHasHoles(const u8* atlasRgba);

}  // namespace mc::texture
