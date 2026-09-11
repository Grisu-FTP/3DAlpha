#pragma once

// Turning a texture pack into the one image the renderer samples.
//
// Two of a pack's files have consumers: `terrain.png`, which the world is drawn
// with, and `gui/items.png`, which the slots on the bottom screen are. A pack's
// `mob/`, its `default.png` and the other 56 files are carried, counted and
// left alone -- the menu draws with the 3DS system font and there are no mobs,
// so there is nothing to point them at yet. That gap is stated on the
// texture-pack screen rather than papered over.
//
// **items.png is optional and terrain.png is not.** A pack without one is a
// pack; every icon that would have come from it falls back to the terrain tile
// of the block the item places, which is what this project drew before the
// second sheet existed. A pack without a terrain.png is not a texture pack.
//
// **The atlas is always 256x256**, whatever the pack's tiles are. That is a
// decision with a measurement behind it: VRAM is 6 MB, render targets already
// take 0.8-1.5 MB, and a 64x pack's 1024x1024 atlas is 4 MB at RGBA8. Rather
// than make the atlas edge a runtime value that every VRAM number in
// docs/3ds-performance.md would have to be re-measured against, an HD pack is
// box-filtered down to 256 on load. The mesher's UVs are in 1/16384 of the
// whole atlas (core/mesh/vertex.hpp), so nothing downstream can tell.
//
// Row order is fixed by the pipeline and is **not** a free choice. Linear row 0
// must be terrain.png's *top* row: platform/ctr/textures.cpp tiles the upload
// through `tiledOffsetFlipped`, which sends source row 0 to the texture's last
// row, and the mesher gives tile 0 a v of ~0, which samples that last row. So
// no flip belongs here.

#include "core/io/file_system.hpp"
#include "core/mesh/vertex.hpp"
#include "core/texture/png.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::texture {

// 16 tiles of 16 px. The tile count is the mesher's, not ours to pick.
inline constexpr int kAtlasTilesPerEdge = mesh::kAtlasTilesPerEdge;
inline constexpr int kAtlasTilePixels = 16;
inline constexpr int kAtlasEdge = kAtlasTilesPerEdge * kAtlasTilePixels;
inline constexpr usize kAtlasBytes = usize(kAtlasEdge) * kAtlasEdge * 4;

// **The ceilings are set by the console's heap, not by what a zip could hold.**
// The 3DS build has -fno-exceptions, so a failed allocation is an abort rather
// than a caught bad_alloc: anything that might not fit has to be refused before
// it is asked for. The newlib heap is 16-40 MB depending on the model and the
// launch method (platform/ctr/heap.cpp), so the smallest console has around
// 20 MB and the peak here is the decoded image plus the filtered scanlines it
// came from, which is twice the decoded size.
//
//   * A 64x terrain.png is 1024x1024 -- the largest tile size any pack in the
//     wild uses -- and costs 4 MB decoded plus 4 MB filtered. That is the
//     ceiling, and anything above it is refused with TooLarge rather than
//     attempted.
//   * A pack zip at that resolution is a few megabytes; 16 MB is generous.
//   * An a1.1.2 client jar is under one megabyte, and every alpha- and
//     beta-era jar is under five. 8 MB refuses a modern 25 MB client jar --
//     which is not an a1.1.2 pack source anyway -- instead of aborting on it.
inline constexpr usize kMaxTerrainPixels = 1024u * 1024u;
inline constexpr usize kMaxPackBytes = 16u << 20;
inline constexpr usize kMaxJarBytes = 8u << 20;

enum class PackError {
    Ok,
    NotFound,     // nothing at that path
    NotAPack,     // a zip that will not open, or a directory with no terrain.png
    NoTerrain,    // the archive opened and has no terrain.png in it
    ReadFailed,   // the card would not give the bytes up
    BadPng,       // terrain.png will not decode; png.hpp says why
    NotSquare,    // terrain.png is not square
    NotTileGrid,  // its edge is not a multiple of 16, so it is not a tile grid
    TooLarge,     // beyond what will be decoded at all
    WriteFailed,
};

const char* packErrorText(PackError error);

struct AtlasImage {
    // kAtlasEdge^2 * 4 bytes, R,G,B,A in memory order, top row first. Empty
    // until something fills it.
    std::vector<u8> rgba;
    // The edge of the terrain.png this came from, before any scaling. 0 for
    // Dev Art. Reported on the pack screen so "my 64x pack looks soft" has an
    // answer on the same line as the pack's name.
    int sourceEdge = 0;

    // `gui/items.png`, scaled the same way into the same 16x16 tile grid.
    // **Empty for a pack that has none**, which is not an error -- see the
    // header. Nothing on the GPU ever sees this one: the icons it feeds are
    // drawn by the CPU into the bottom screen's framebuffer, so it costs 256 KB
    // of heap and no VRAM at all.
    std::vector<u8> itemsRgba;

    // **The entity sheet**, 256 x 64, holding `item/boat.png`,
    // `item/cart.png`, `item/sign.png`, `item/arrows.png` and the player skin
    // `char.png` in five fixed pages. See core/texture/entity_skins.hpp for the
    // layout and for why the page size is 64 x 32 rather than anything chosen
    // here.
    //
    // Unlike the two above it this one is **never empty for a valid pack**: a
    // page a pack does not carry keeps its generated stand-in, so the sheet is
    // always whole. A boat with no texture would be a black boat -- and the
    // player's page is the one that *is* black on purpose when a pack has no
    // skin in it, which that header argues.
    std::vector<u8> entityRgba;

    // **The painting sheet**, `art/kz.png`, 256 x 256 and its own plane
    // because it already is one -- `er` indexes it in absolute texels. Also
    // never empty; the stand-in is a grid of framed cells.
    std::vector<u8> artRgba;

    bool empty() const { return rgba.size() != kAtlasBytes; }
    bool hasItems() const { return itemsRgba.size() == kAtlasBytes; }
};

// Rescales a square source into an `edge` x `edge` RGBA buffer.
//
// Larger sources are area-averaged over the covering source rectangle.
// **The average is premultiplied by alpha**, or every cutout tile -- a torch,
// a sapling, the rim of a leaf -- picks up a dark halo from the transparent
// texels beside it, whose RGB is usually black. Sources smaller than the target
// are replicated nearest-neighbour, so a 8x pack stays crisp rather than blurry.
//
// The size is a parameter rather than kAtlasEdge because the font sheet is
// scaled by the same two rules into a 128x128 buffer; see core/texture/font.hpp.
void scaleSquare(const Image& source, int edge, std::vector<u8>* out);

// scaleSquare into the atlas's own size, which is what a terrain.png wants.
void scaleToAtlas(const Image& source, std::vector<u8>* out);

// Reads one file out of a pack, whether the pack is a `.zip` or a directory
// holding the tree loose.
//
// **NotFound means the pack has no such file**, which for anything but
// terrain.png is a pack that simply does not carry it rather than a broken one.
// The other errors are the pack's: unopenable, unreadable, absent.
//
// Each call opens the pack again. That is one card read per file and it is
// deliberate: the menu asks for three files at the moment a pack is chosen, a
// pack is a megabyte or so, and holding an open archive across the three would
// mean an object with a lifetime for the sake of two reads a player waits on
// once.
PackError readPackFile(io::FileSystem& fs, std::string_view packPath, std::string_view name,
                       std::vector<u8>* out);

// Builds the atlas for a pack. An empty `packPath` means Dev Art.
//
// `packPath` may be a `.zip` or a directory holding the pack's tree loose. Both
// are accepted because a card is mounted on a PC as often as on a console, and
// a player who unzipped a pack in place has not done anything wrong.
PackError buildAtlas(io::FileSystem& fs, std::string_view packPath, AtlasImage* out);

}  // namespace mc::texture
