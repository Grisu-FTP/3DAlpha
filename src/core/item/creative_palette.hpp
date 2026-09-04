#pragma once

// The Creative block palette -- **and a1.1.2 has no Creative mode at all**.
//
// This is the first thing in the project with no oracle behind it. Alpha v1.1.2
// has one way to play: `Minecraft` carries no gamemode field, `EntityPlayer` no
// capabilities object, and the word does not appear in the client. Creative is
// Beta 1.8's, two years later and a different codebase. So nothing in this file
// was recovered from a jar, nothing here can be pinned against a reference
// implementation, and no test in `tests/` compares it to one. That is also why
// gamemode lives in `<world>/3dalpha.ini` and never in `level.dat` -- see
// core/settings/world_settings.hpp for the other half of the same argument.
//
// What that buys is licence to make the palette *derived* rather than curated.
// Beta's own creative inventory is a hand-written list in `CreativeTabs`, and a
// hand-written list here would be 70 block ids typed into a source file --
// which is precisely the thing CONTRIBUTING forbids and for a better reason
// than style: the next version manifest would silently inherit a1.1.2's list.
//
// So the palette is **every block this version's registry defines**, in id
// order, minus air. `BlockDef::known` is the column that answers it -- it is
// already false for the sixteen ids a1.1.2 leaves empty -- and it means adding
// a version whose registry has more blocks in it adds them here with no edit.
//
// **What it does not do**, all of it deliberate and none of it a guess:
//
//   * No item ids. The `items` slot is empty for this version, and a palette
//     of *blocks* is what a block-placing game needs first. Saplings, doors,
//     beds and cake are items backed by blocks in the original and are offered
//     here as their blocks, which places the block form directly.
//   * No filtering by "would a player ever get this". Fire, mob spawners,
//     flowing water and the double slab are all in it. Excluding them would be
//     inventing a second table with nothing to check it against, and a Creative
//     mode that can place a mob spawner is more useful to this project than one
//     that has opinions.
//   * No ordering by category. Id order is the one ordering that is stable
//     across builds and derivable from the data.

#include "core/block/block_def.hpp"
#include "core/util/types.hpp"

namespace mc::item {

// How many blocks the palette offers. Computed once at load from the registry;
// for a1.1.2 it is 70 -- `kBlockCount` minus air.
int paletteSize();

// The block at `index`, or air for an index outside the palette. Air is the
// out-of-range answer rather than an assert because this is read by a UI whose
// page arithmetic can legitimately run off the end of the last page.
block::BlockId paletteBlock(int index);

// Where a block sits in the palette, or -1 for one that is not in it (air, and
// any id this version's registry does not define). The inverse of the above,
// and what lets a hotbar slot show which palette cell it came from.
int paletteIndexOf(block::BlockId id);

}  // namespace mc::item
