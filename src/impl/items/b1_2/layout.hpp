#pragma once

// What a slot number in `Player.Inventory` means.
//
// This is the whole of the `items` slot for now, and it is small on purpose:
// the *encoding* of a stack -- `{Slot, id, Count, Damage}` -- lives with the
// level format in impl/storage/, because that is the file it is a part of and
// core/world/level_data.hpp already carries the decoded list. What is version
// business, and what is here, is the **numbering**: which of those slot indices
// is the hand, which is the backpack, and which is armour.
//
// **Named b1_2 because the numbering is, not because a1.1.2 is Beta 1.2.**
// `InventoryPlayer` writes `mainInventory` at its own index and `armorInventory`
// at index + 100, and that arrangement holds unchanged from the alpha era
// through Beta 1.2 and past it. The slot is named for the format the way
// `alpha_chunkfiles` is, so a second version that shares it shares this file
// rather than copying it.
//
// **Anything outside these ranges is somebody else's.** A real a1.1.2 save
// turns out to contain them: the reference world in docs has a stack at slot
// 81, which `InventoryPlayer.readFromNBT` silently drops on load because it is
// neither `< 36` nor in `100..103`. Dropping it is what the original does and
// it is not what this project does -- see core/item/inventory.hpp, which keeps
// such entries aside and writes them back out untouched.

#include "core/util/types.hpp"

namespace mc::items {

struct B1_2Layout {
    // `InventoryPlayer.mainInventory.length`. The first nine are the hotbar,
    // which is a fact about how it is *drawn* rather than a second array.
    static constexpr int kMainSlots = 36;
    static constexpr int kHotbarSlots = 9;

    // `InventoryPlayer.armorInventory`, written at slot + 100.
    static constexpr int kArmourSlots = 4;
    static constexpr int kArmourBase = 100;

    // Which array a slot number belongs to, and the index within it. Returns
    // false for a number in neither, which is the case the caller has to keep
    // rather than discard.
    static bool isMain(int slot) { return slot >= 0 && slot < kMainSlots; }
    static bool isArmour(int slot)
    {
        return slot >= kArmourBase && slot < kArmourBase + kArmourSlots;
    }
    static int armourIndex(int slot) { return slot - kArmourBase; }
    static int armourSlot(int index) { return index + kArmourBase; }
};

}  // namespace mc::items
