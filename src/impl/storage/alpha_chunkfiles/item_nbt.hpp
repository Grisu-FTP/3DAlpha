#pragma once

// One `ev` -- ItemStack -- as NBT, which is the same four tags everywhere it
// appears in an Alpha file.
//
// `ev.a(Lhm;)Lhm;` writes `id` (Short), `Count` (Byte) and `Damage` (Short),
// and every caller adds the `Slot` byte before handing the compound over:
// `dk.a` for the player's inventory, `fe.b` for a chest, `ke.b` for a furnace.
// The reader is `ev(Lhm;)`, which takes the three, plus the caller's own read
// of `Slot`.
//
// It lives here rather than in core/item because it is a *format*: the same
// three values are written differently by later versions, and the version's
// storage slot is where that belongs. The two callers -- level.dat's inventory
// and the chunk's TileEntities -- were the same twenty lines twice.
//
// **`Slot` is part of the compound in both places**, so it is read and written
// here too; a context that has no slot simply leaves `ItemStack::slot` at zero
// and ignores it.

#include "core/item/item_stack.hpp"
#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"

namespace mc::alpha {

// Reads the compound the cursor has just entered, to its TAG_End. Unmodelled
// tags are captured onto the stack so a tool's additions survive the trip.
bool decodeItemStack(nbt::Reader& r, item::ItemStack* stack);

// Writes the four tags into the compound already opened by the caller, so the
// caller decides whether it is a named compound or a list element.
void encodeItemStack(nbt::Writer& w, const item::ItemStack& stack);

}  // namespace mc::alpha
