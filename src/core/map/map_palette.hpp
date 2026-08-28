#pragma once

// What colour a block is on the map, and the three brightnesses every one of
// them comes in.
//
// **Derived from the texture pack, not from a table.** Later versions key the
// map colour off `Material` -- `Material.grass` is green, `Material.ground` is
// brown -- and that cannot be borrowed here, because a1.1.2's material set is
// coarser than the one those colours were written against: grass, dirt and
// farmland share a material in this version, so a material-keyed table paints
// every meadow the colour of a ploughed field. Writing a per-block colour table
// by hand would work and is exactly the version-specific data CONTRIBUTING.md
// keeps out of `switch` statements and out of code.
//
// So the colour is the average of the block's **top face** in whatever
// `terrain.png` is loaded, which is the same picture the player is looking down
// at on the top screen. Three things fall out of that and all three are wanted:
// the map matches the world, a texture pack recolours the map for free, and a
// version this project has not met yet needs no new data to be mapped.
//
// **Alpha-weighted**, because a tile with a transparent margin -- a sapling, a
// cactus -- would otherwise average towards black through pixels that are not
// drawn at all.
//
// The three brightnesses are later versions' own numbers: 180, 220 and 255 out
// of 255, applied per channel. `MapData` picks between them from the height
// step to the north and from water depth; see map_render.hpp.

#include "core/block/registry.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/types.hpp"

namespace mc::map {

// Pixels are RGB565. It is what the console's bottom screen holds once
// `consoleInit` has had it -- so a map can be written straight into the
// framebuffer with no conversion in the loop -- and it is an ordinary 16-bit
// colour that a host test can pick apart. Nothing else about it is platform
// specific: `map_render` writes through a stride pair, so the caller decides
// the layout.
using MapPixel = u16;

inline constexpr MapPixel rgb565(int r, int g, int b)
{
    return MapPixel(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// How many brightnesses a colour comes in. Later versions carry four -- the
// fourth, 135, is only ever used by things this version does not have -- so the
// three that terrain and water actually select between are what is stored.
inline constexpr int kShadeCount = 3;

// 180, 220, 255 out of 255, from `MapColor.getMapColor`.
inline constexpr int kShadeNumerator[kShadeCount] = {180, 220, 255};

// One entry per block id the build knows about. Ids past the end of the table
// -- a modded world, a newer server -- are not in it and draw as unexplored,
// which is the same thing the block registry does with them.
inline constexpr int kPaletteSize = mcver::kBlockTableSize;

struct MapPalette {
    // Ready to write: `shaded[shade][id]`. Indexed this way round because the
    // render loop varies the id every pixel and the shade rarely.
    MapPixel shaded[kShadeCount][kPaletteSize] = {};

    // The unshaded colour, 0x00RRGGBB. Nothing in the render loop reads it;
    // markers, tests and anything that wants a true colour do.
    u32 base[kPaletteSize] = {};

    // `map::isMapWater`, precomputed. It is asked once per pixel and the
    // answer is a property of the block table, so it belongs here beside the
    // other two rather than behind a lookup in the inner loop.
    bool water[kPaletteSize] = {};

    // False for a block whose top face is not in the atlas at all. Kept so the
    // renderer can tell "this block is that colour" from "there is no colour
    // for this block" without a sentinel that some pack could legitimately
    // produce.
    bool known[kPaletteSize] = {};
};

// Builds the whole table from one atlas. Called when a world opens and again
// whenever the pack changes; it is 256 tiles of 256 pixels, so it costs about
// as much as one chunk sample and is not something to cache across packs.
void buildMapPalette(const texture::AtlasImage& atlas, MapPalette* out);

// The average colour of one atlas tile, alpha-weighted, as 0x00RRGGBB. Exposed
// because it is the whole of the derivation above and is worth being able to
// test on its own.
u32 averageTileColour(const texture::AtlasImage& atlas, int tile);

// Applies one of the three brightnesses to a 0x00RRGGBB colour.
u32 shadeColour(u32 rgb, int shade);

}  // namespace mc::map
