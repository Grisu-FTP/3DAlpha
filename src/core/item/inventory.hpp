#pragma once

// What the player is carrying: nine slots in the hand, twenty-seven behind
// them, four of armour, and the index of the one being held.
//
// **This replaces the nine-slot `Hotbar` that used to be here**, and the reason
// is the save file rather than the screen. a1.1.2 writes `Player.Inventory` as
// one list of slot-tagged stacks covering all forty, and `currentItem` indexes
// the first nine of them. A hotbar on its own could be drawn but not written:
// there was nowhere for the other twenty-seven to go and no reason for the nine
// to be a separate array. So the hotbar is a *view* of slots 0..8 here, exactly
// as it is in `InventoryPlayer`, and the Items page on the bottom screen is a
// view of 9..35.
//
// **It holds item ids, not block ids**, which is the other half of the same
// change. See core/item/item_def.hpp: a door is item 324 and the block it
// leaves is 64, so a hotbar of block ids could not be saved without writing a
// file the real client reads back as something else.
//
// **Slots this build does not model are kept, not dropped.** `readFromNBT` in
// the original throws away any entry whose slot is neither `< 36` nor in
// `100..103`, and a real a1.1.2 save in this project's own reference world has
// one at slot 81. Discarding it would be a load/save cycle destroying data,
// which is the thing this project promises not to do anywhere else
// (core/nbt/preserved.hpp), so those entries sit in `unmodelled` and go back
// out verbatim.
//
// Nothing here allocates except `unmodelled`, which is touched only when a
// world is opened or saved.

#include "core/item/item_def.hpp"
#include "core/item/item_stack.hpp"

#include "version_slots.hpp"

#include <vector>

namespace mc::item {

// The version's own numbering, from the `items` slot. Named here so nothing
// below has to spell out 36, 9 or 100.
inline constexpr int kMainSlots = mcver::ItemLayout::kMainSlots;
inline constexpr int kHotbarSlots = mcver::ItemLayout::kHotbarSlots;
inline constexpr int kArmourSlots = mcver::ItemLayout::kArmourSlots;

// The save file's number for armour slot 0. Exposed because the UI addresses
// every one of the forty through `at`, and the alternative is a second copy of
// the +100 rule on the screen's side of the seam.
inline constexpr int kArmourBase = mcver::ItemLayout::kArmourBase;

// Slots 9..35: what the Items page draws.
inline constexpr int kBackpackSlots = kMainSlots - kHotbarSlots;

struct Inventory {
    ItemStack main[kMainSlots];
    ItemStack armour[kArmourSlots];

    // `InventoryPlayer.currentItem`, an index into the first nine.
    int selected = 0;

    // Entries whose slot number this version does not model. See the header.
    std::vector<ItemStack> unmodelled;

    // The item in hand, or 0 for an empty slot. Zero rather than -1 because the
    // place path already refuses id 0, so an empty hand needs no second test at
    // the seam.
    ItemId selectedItem() const;

    // The block the held item puts down, or 0. This is what breaking and
    // placing asks, and it goes through the item table -- which is how holding
    // a door places block 64 without anything at the seam knowing that.
    u16 selectedPlacesBlock() const;

    // `eu.a(Lev;)Z` -- **InventoryPlayer.addItemStackToInventory**, which is
    // what walking over a dropped item runs. Takes an item and a count, fills
    // whatever it can, and answers **how many are left over** -- zero for a
    // stack that went in whole.
    //
    // The original takes an `ItemStack` and mutates its `stackSize`, returning
    // a bool; a count in and a count out says the same thing without a caller
    // having to know that its argument was edited. `true` in the original is
    // exactly `leftover == 0` here, and the one caller that cares -- the item
    // entity, which only disappears when it was taken whole -- reads it that
    // way.
    //
    // **Damaged stacks do not merge.** The first line of the method is
    // `if (itemstack.itemDamage == 0)`, so a partly-worn tool goes to a fresh
    // slot rather than stacking with another of its kind. Nothing in this build
    // wears out yet, so every stack takes the merging path -- but the rule is
    // here rather than assumed away.
    int addStack(ItemId id, int count, i16 damage = 0);

    // `j()` -- getFirstEmptyStack, over the thirty-six. -1 for a full one.
    int firstEmptySlot() const;

    // `InventoryPlayer.decrStackSize(currentItem, 1)`, which is what
    // `EntityPlayer.dropOneItem` spends before it spawns the entity: one comes
    // off the held stack, and a stack that reaches zero becomes an empty slot
    // rather than a stack of nothing.
    //
    // Returns the item taken, or 0 for an empty hand. **The caller is what
    // decides where it goes**, and it does now go somewhere:
    // `entity::ItemEntitySystem::dropFromPlayer` is the throw the original
    // makes. That entity is spawned *before* this is called, because a pool
    // that refused the spawn would otherwise have destroyed the item -- so
    // this name is honest and always has been the half that only spends.
    ItemId dropOne();

    // Moves the selection by `delta`, wrapping both ways. **Wrapping rather
    // than clamping**: the original's mouse wheel wraps, and on a console where
    // this is two shoulder buttons, a selection that stops dead at slot 9 makes
    // getting back to slot 1 eight presses instead of one.
    void cycle(int delta);

    // Reads and writes one of the forty by its save-file slot number, so the
    // UI can address the backpack and the hand the same way. Out of range
    // reads answer with an empty stack and writes are dropped.
    const ItemStack& at(int slot) const;
    void set(int slot, ItemId id, i8 count);

    // **The save-file slot a piece of armour belongs in**, or -1 for anything
    // that is not armour. `ItemArmor.armorType` counts from the head and
    // `armorInventory` from the feet, so this is the one place the two orders
    // meet -- see core/item/item_def.hpp's `armour`.
    static int armourSlotFor(ItemId id);

    // `lj.a(Lev;)Z` -- **SlotArmor.isItemValid**, generalised to every slot.
    //
    // The thirty-six main slots take anything, which is why this reads as a
    // question about armour: the four at 100..103 take **only the piece that
    // belongs there**, so a helmet cannot go on the feet and a sword cannot go
    // anywhere but the backpack. An empty stack is accepted everywhere, because
    // taking a piece *out* of an armour slot is the same swap run the other way
    // -- and "empty" is both 0, which `selectedItem` answers with, and
    // `kEmptyItemId`, which a stored stack carries.
    static bool accepts(int slot, ItemId id);

    // Swaps two slots, which is the only edit a touch-screen inventory makes:
    // picking a stack up and putting it down again is a swap with the cursor's
    // slot, and so is moving one from the backpack into the hand.
    //
    // **Refused outright when either end would not accept what the other holds**,
    // rather than half-done: a swap is one move and a slot that keeps its
    // contents while the other loses them would destroy a stack. The caller
    // sees no change, which is what a slot that will not take something looks
    // like in the original too -- the cursor keeps holding it.
    void swap(int a, int b);

    // Fills the nine hand slots from consecutive palette entries starting at
    // `firstPaletteIndex`, and leaves the selection where it is.
    //
    // **The starting hand is derived, not chosen.** A curated nine would be a
    // list of ids in a source file, which is the thing the palette exists to
    // avoid.
    void fillHandFromPalette(int firstPaletteIndex = 0);

    // Everything to empty, `unmodelled` included. What entering a world does
    // before the save file is read over the top.
    void clear();

    // `InventoryPlayer.readFromNBT` and `writeToNBT`, against the sparse
    // slot-tagged list core/world/level_data.hpp carries. `load` clears first,
    // so a second world does not inherit the first one's hand.
    void load(const std::vector<ItemStack>& stacks);
    void save(std::vector<ItemStack>* stacks) const;

    // Whether anything at all is in it. A world whose player was never given
    // one gets the palette's opening hand instead of an empty screen.
    bool empty() const;
};

}  // namespace mc::item
