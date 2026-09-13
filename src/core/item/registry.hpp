#pragma once

// Item lookup, and the same arrangement as core/block/registry.hpp: a constexpr
// array in .rodata generated from data/<version>/items.json, so a lookup is a
// bounds check and an index.
//
// **The contiguous table stops at 512 and the two music discs are at 2256 and
// 2257**, which is 1,910 ids past the end of a1.1.2's item run. Covering them
// in the array would be 2,258 rows to carry two, so they are a pair of parallel
// side arrays instead and `def` falls back to a scan over them.
//
// **The scan is two entries and costs nothing for an id the table reaches**:
// the bounds check that used to answer "unknown" answers "look in the side
// table" now, and every ordinary lookup takes the same branch it always did.
//
// This used to say the discs were rows "no a1.1.2 player can obtain", which was
// true when nothing dropped one. **A skeleton killing a creeper does**
// (`dd.b(Lkh;)V`, and it is the only source of a record in this version), so
// they are obtainable, they have an icon, they are in the Creative palette, and
// they no longer draw as an empty slot.
//
// An id neither the table nor the side array knows is still *carried* rather
// than dropped -- the same promise core/nbt/preserved.hpp makes about tags --
// and draws as nothing.

#include "core/item/item_def.hpp"
#include "items.hpp"  // generated; see tools/configure.py

namespace mc::item {

// Never fails. An id past the array is looked for in the side table, and one
// that is in neither comes back as the unknown item rather than reading past
// the end.
inline const ItemDef& def(ItemId id)
{
    if (id >= 0 && id < mcver::kItemTableSize) {
        return mcver::kItems[id];
    }
    for (int i = 0; i < mcver::kItemsOutsideTable; ++i) {
        if (mcver::kOutsideItemIds[i] == id) {
            return mcver::kOutsideItems[i];
        }
    }
    return mcver::kUnknownItem;
}

// The block this item puts down, or 0. The place path's whole question.
inline u16 placesBlock(ItemId id) { return def(id).places; }

}  // namespace mc::item
