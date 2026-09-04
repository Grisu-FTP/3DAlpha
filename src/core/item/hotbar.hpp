#pragma once

// The nine slots along the bottom of the screen, and which one is in hand.
//
// **Nine because a1.1.2's is nine** -- `InventoryPlayer` holds 36 slots and
// draws the first nine as the hotbar, and `currentItem` is the index into them.
// That much is the original's and is not invented. What *is* invented is
// everything Creative does to it: an infinite stack, picking straight out of a
// palette, and a slot that never empties. See core/item/creative_palette.hpp
// for why there is no oracle for any of that.
//
// **It holds ItemStacks, not block ids**, even though Creative only ever puts
// blocks in it. Survival is the next milestone and it needs counts and damage;
// a hotbar of bare ids would have to be rewritten rather than extended, and
// ItemStack is already the shape the `items` slot will encode.
//
// **Not saved yet.** a1.1.2 writes the player's inventory into `level.dat`'s
// `Player` compound as a list of slot-tagged stacks, and this project preserves
// that compound verbatim rather than parsing it (core/nbt/preserved.hpp). A
// hotbar written back out would have to go through the `items` slot, which is
// empty for this version -- so the contents live for the session and start from
// the palette each time. That is a gap and it is named here rather than
// discovered later.
//
// Nothing here allocates and nothing here touches the world: it is a fixed
// array and an index, so it can sit in the per-frame path and be tested on the
// host without a console.

#include "core/block/block_def.hpp"
#include "core/item/item_stack.hpp"

namespace mc::item {

// `InventoryPlayer.mainInventory` is 36 long and the hotbar is its first row.
inline constexpr int kHotbarSlots = 9;

struct Hotbar {
    ItemStack slots[kHotbarSlots];
    int selected = 0;

    // The block in hand, or air for an empty slot. Air is what the placement
    // path already refuses, so an empty slot needs no second test at the seam.
    block::BlockId selectedBlock() const;

    // Moves the selection by `delta`, wrapping both ways. **Wrapping rather
    // than clamping**: the original's mouse wheel wraps, and on a console where
    // this is two shoulder buttons, a selection that stops dead at slot 9 makes
    // getting back to slot 1 eight presses instead of one.
    void cycle(int delta);

    // Puts a block in a slot as a stack of one. Air clears the slot, which is
    // what makes "pick the empty cell" a way to empty your hand rather than a
    // no-op.
    void set(int slot, block::BlockId id);

    // Fills all nine from consecutive palette entries starting at
    // `firstPaletteIndex`, and leaves the selection where it is.
    //
    // **The starting hand is derived, not chosen.** A curated nine would be a
    // list of block ids in a source file, which is the thing the palette exists
    // to avoid; the first nine palette entries are stone, grass, dirt,
    // cobblestone, planks, sapling, bedrock and the two waters, which is a
    // defensible opening hand and, more to the point, is whatever the registry
    // says rather than whatever somebody typed.
    void fillFromPalette(int firstPaletteIndex = 0);
};

}  // namespace mc::item
