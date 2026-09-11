#pragma once

// What one item looks like in a slot: a flat sprite for most things, and a
// **cube seen from the corner** for anything that is a plain block.
//
// **The cube is why a hotbar full of stone stopped looking like a hotbar full
// of wallpaper.** a1.1.2 draws inventory blocks in 3D -- `RenderItem` puts the
// block through `RenderBlocks` with the GUI's own transform, so a slot shows a
// top face and two sides at three different brightnesses -- and drawing one
// tile flat instead makes every cube in the game a featureless square. The
// three faces are what tell grass from dirt, a log's end from its bark, and a
// furnace's mouth from its back.
//
// **It is rasterised here rather than rendered.** The bottom screen has no GPU
// behind it (core/gui/paint.hpp says why), so the cube is three parallelograms
// filled by inverse-mapping each destination pixel back into its face's tile.
// The projection is the flat 2:1 one every isometric sprite uses, not the
// original's 30-degree matrix: at sixteen pixels across, the difference between
// the two is under half a pixel and the 2:1 version lands its edges exactly on
// pixel boundaries, so the silhouette has no stair-stepping to dither away.
//
// The shades **are** the original's, and are the same table the world mesher
// uses -- `mesh::kFaceShadeFloat`. A slot lit differently from the world would
// be the kind of small wrongness that is felt rather than seen.
//
// **Which sheet an icon comes from is a1.1.2's rule and not a choice here**:
// ids below 256 index terrain.png and ids at or above it index gui/items.png.
// That is the whole reason the door in the hand used to be the *bottom half of
// a door* -- block 64's terrain tile is the lower panel, and the door you carry
// is item 324, tile 43 of items.png. See core/item/item_def.hpp.
//
// A pack with no items.png is not an error: the icon falls back to the terrain
// tile of whatever block the item places, which is exactly what this drew
// before there was a second sheet.

#include "core/gui/paint.hpp"
#include "core/item/item_def.hpp"
#include "core/util/types.hpp"

namespace mc::gui {

// The two 256x256 RGBA sheets, borrowed rather than owned. Either may be null;
// a null terrain sheet draws nothing at all, which is the state before a pack
// has been handed over.
struct IconSheets {
    const u8* terrain = nullptr;
    const u8* items = nullptr;

    // **One tile of the items sheet, replaced live** -- which is the whole of
    // how a compass works. See core/texture/compass_fx.hpp: a1.1.2's compass is
    // not an item behaviour but a `TextureFX` rewriting 256 texels every tick,
    // and a slot holding one has to draw the current face rather than the
    // pack's still one.
    //
    // **An override rather than a write into the sheet.** The original really
    // does overwrite `gui/items.png` in place, and this could too -- the sheet
    // is a decoded copy. It does not, because that buffer belongs to the Menu
    // and outlives the world: a compass would leave its last needle baked into
    // the pack, visible on the pack screen and inherited by the next session.
    // One pointer and one compare in the blit is the cheaper of the two.
    //
    // `animatedItems` is 16 x 16 RGBA, the same layout as one tile of a sheet,
    // or null when nothing is animating. `animatedItemsTile` is which tile it
    // stands in for, and -1 matches nothing.
    const u8* animatedItems = nullptr;
    int animatedItemsTile = -1;
};

// Draws item `id` as a `size` x `size` icon with its top-left at (x, y).
// Clips against the surface. Id 0 -- an empty slot -- draws nothing.
void drawItemIcon(const Surface& surface, int x, int y, int size, const IconSheets& sheets,
                  item::ItemId id);

// Whether this item is drawn as a cube. Exposed because the layout wants to
// know: a cube fills its box corner to corner and a flat sprite does not, so a
// row of mixed icons reads better when the flat ones are inset by a pixel.
bool itemDrawsAsCube(item::ItemId id);

}  // namespace mc::gui
