#pragma once

// Item lookup, and the same arrangement as core/block/registry.hpp: a constexpr
// array in .rodata generated from data/<version>/items.json, so a lookup is a
// bounds check and an index.
//
// **The table stops short of the two music discs**, which are ids 2256 and
// 2257. Covering them would mean a 2,258-entry array to carry two rows that no
// a1.1.2 player can obtain, place, or do anything with. They are still in
// items.json, and a stack holding one still round-trips through a save
// untouched -- an id the table does not know is carried, not dropped, which is
// the same promise core/nbt/preserved.hpp makes about tags. What is lost is the
// icon and the name, and a disc in a hotbar slot draws as an empty slot.

#include "core/item/item_def.hpp"
#include "items.hpp"  // generated; see tools/configure.py

namespace mc::item {

// Never fails. An id outside the table -- a disc, or one from a world this
// build does not know -- comes back as the unknown item rather than reading
// past the end.
inline const ItemDef& def(ItemId id)
{
    return id >= 0 && id < mcver::kItemTableSize ? mcver::kItems[id] : mcver::kUnknownItem;
}

// The block this item puts down, or 0. The place path's whole question.
inline u16 placesBlock(ItemId id) { return def(id).places; }

}  // namespace mc::item
