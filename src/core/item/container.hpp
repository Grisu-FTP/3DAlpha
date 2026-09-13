#pragma once

// **What one click on a container slot does** -- the slot half of `ee.a(III)V`,
// GuiContainer.mouseClicked, which is the whole of a1.1.2's inventory handling.
//
// There is no Container/windowClick layer in this version: the screen itself
// holds the stack on the cursor (`InventoryPlayer.e`, the player's own field)
// and edits the slot's inventory directly. So the rules are one method, and
// they are transcribed here as a pure function over two stacks -- the slot's and
// the cursor's -- so the chest, the furnace, both crafting grids and the
// player's own inventory all share them, and the host suite can drive every
// branch.
//
// Button 0 is the left click and 1 the right. In the jar's order:
//
//   * **An empty cursor on a stack picks it up** -- all of it on the left, half
//     rounded up on the right. A result slot hands over the whole stack either
//     way, because `mw.a(II)` ignores the count it is asked for.
//   * **A full cursor on an empty slot puts down** all of it on the left and one
//     on the right, never more than the slot's limit of 64 -- and only into a
//     slot that accepts it.
//   * **A full cursor on a different item swaps**, if the cursor would fit.
//   * **A full cursor on the same item adds** as much as fits under both the
//     slot's limit and the item's own stack size: all of it, or one.
//   * **A slot that refuses the cursor** -- a crafting result -- still gives up
//     its stack to a cursor holding the same item, when the item stacks and the
//     whole of it fits. That is how a second torch recipe is taken onto the
//     first.
//
// **Ids alone decide "the same item"**: the jar compares `itemID` and never the
// damage, because nothing that stacks in this version takes damage.
//
// The click outside the window is `clickOutside`: the whole cursor thrown on the
// left, one on the right.

#include "core/item/item_stack.hpp"
#include "core/util/types.hpp"

namespace mc::item {

// `gh.e()` -- getInventoryStackLimit, 64 for every inventory in this version.
inline constexpr int kInventoryStackLimit = 64;

// What a slot will take.
enum class SlotRule : u8 {
    // `mm` -- anything.
    Any,
    // `lj` -- a piece of armour of `armourType` only (`mr.aX`).
    Armour,
    // `an` -- the crafting result: nothing can be put in, the stack can be
    // taken, and taking it spends the grid.
    TakeOnly,
};

struct SlotClick {
    // Something moved.
    bool changed = false;
    // The slot's `onPickupFromSlot` ran -- which for a `TakeOnly` slot is the
    // cue to spend one of each ingredient. See core/item/crafting.hpp.
    bool pickedUp = false;
};

// One click of `button` on a slot holding `slot`, with `cursor` on the mouse.
SlotClick clickSlot(ItemStack& slot, ItemStack& cursor, int button, SlotRule rule,
                    int armourType = -1, int limit = kInventoryStackLimit);

// A click outside the window with something on the cursor. `thrown` receives
// what leaves it -- the whole stack on the left, one on the right -- for the
// caller to drop the way `EntityPlayer.dropPlayerItem` does. False, and nothing
// written, for an empty cursor or another button.
bool clickOutside(ItemStack& cursor, int button, ItemStack* thrown);

// `ev.a(I)Lev;` -- splitStack: a new stack of `count` of the same item and
// damage, taken off `from`. Unclamped, as the original is.
ItemStack splitStack(ItemStack& from, int count);

// Empties a stack in place, tags and all.
void clearStack(ItemStack& stack);

}  // namespace mc::item
