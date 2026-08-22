#pragma once

// One stack of items.
//
// The on-disk and on-wire shapes both predate item NBT: an id, a count and a
// damage value, nothing else. The `items` slot owns the encoding (b1_2 for this
// version), so this type carries the values and not their layout.

#include "core/nbt/preserved.hpp"
#include "core/util/types.hpp"

namespace mc::item {

// Alpha writes an absent slot by omitting it, so an empty stack never reaches
// disk; it exists here because an inventory is a fixed array in memory.
inline constexpr i16 kEmptyItemId = -1;

struct ItemStack {
    i16 id = kEmptyItemId;
    i16 damage = 0;
    i8 count = 0;
    // The inventory slot this stack occupied. Alpha stores the inventory as a
    // sparse list of slot-tagged entries rather than a dense array, and the slot
    // numbering is the save format's, not ours.
    i8 slot = 0;

    // Per-stack tags we do not model. Empty for every real a1.1.2 file, kept so
    // that a world touched by a tool still round-trips.
    nbt::PreservedTags preserved;

    bool empty() const { return id < 0 || count == 0; }
};

}  // namespace mc::item
