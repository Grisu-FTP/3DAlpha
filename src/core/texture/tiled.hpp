#pragma once

// Where a texel lands in a PICA texture.
//
// The GPU does not store a texture as scanlines. It stores 8x8 tiles, laid out
// left to right and then top to bottom, and **Morton-ordered within each tile**
// -- so texel (1,0) is one word after (0,0) but texel (0,1) is two words after
// it, not eight. Anything the CPU writes into a texture has to go through this
// map or it comes out shredded into 8x8 squares.
//
// This lives in core rather than in platform/ctr because it is arithmetic and
// nothing else: no citro3d type appears in it, and the host test binary can
// therefore pin it. That is not a stylistic move. `platform/ctr/` is compiled
// only for the console, where nothing is sanitised and nothing is tested, and
// the atlas upload it used to hide had a defect in it for exactly that reason
// -- see docs/status.md.

#include "core/util/types.hpp"

namespace mc::texture {

// The Morton index of (x, y) within one 8x8 tile.
//
// Interleaves the low three bits of each: x supplies the even bits and y the
// odd ones, so the result runs 0..63 over the tile. The shifts are the standard
// bit-spreading trick rather than a loop, and the masks are what keep the two
// halves from colliding while they spread.
constexpr u32 mortonInterleave(u32 x, u32 y)
{
    u32 i = (x & 7) | ((y & 7) << 8);
    i = (i ^ (i << 2)) & 0x1313;
    i = (i ^ (i << 1)) & 0x1515;
    i = (i | (i >> 7)) & 0x3F;
    return i;
}

// The word offset of texel (x, y) in a tiled texture `width` texels across.
//
// Morton within the tile, then whole tiles: 64 words per tile across, and
// `width * 8` words per row of tiles.
constexpr u32 tiledOffset(u32 x, u32 y, u32 width)
{
    return mortonInterleave(x, y) + (x & ~7u) * 8 + (y & ~7u) * width;
}

// **`v = 0` samples the LAST row in memory, not the first.** Everything the CPU
// writes into a texture has to invert its row index; nothing warns you if it
// does not, and the failure is a picture that is upside down rather than a
// picture that is missing.
//
// Derived rather than asserted, because the two halves of the evidence are in
// different files. `platform/ctr/probe.cpp`'s `buildAtlas` -- M0, validated on
// hardware -- inverts the row by hand and says "texture origin is bottom-left".
// `mesh/vertex.hpp` gives tile 0 a v of ~0. The probe's atlas is right way up
// on a console, so those two only agree if v = 0 reads the last row.
//
// The lightmap did not do this and was inverted on hardware in the most legible
// way possible: caves and sea floors lit like open sky, open sky lit like a
// cave. The M0 probe validated the Morton path but not its orientation -- it
// drew one texture and never asked which way up it was.
constexpr u32 tiledOffsetFlipped(u32 x, u32 row, u32 width, u32 height)
{
    return tiledOffset(x, height - 1 - row, width);
}

}  // namespace mc::texture
