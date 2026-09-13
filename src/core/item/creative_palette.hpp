#pragma once

// The Creative palette -- **and a1.1.2 has no Creative mode at all**.
//
// Alpha v1.1.2 has one way to play: `Minecraft` carries no gamemode field,
// `EntityPlayer` no capabilities object, and the word does not appear in the
// client. Creative is Beta 1.8's, two years later and a different codebase. So
// nothing about *having* a palette was recovered from a jar, and that is why
// gamemode lives in `<world>/3dalpha.ini` and never in `level.dat` -- see
// core/settings/world_settings.hpp for the other half of the same argument.
//
// **What is in it, though, is no longer this file's opinion.** The palette used
// to be "every block the registry knows, minus air", computed here. That is
// what put the *burning* furnace in the hand next to the furnace, flowing water
// next to water, and the block form of a door -- whose texture is the door's
// lower panel -- where the door item belongs. Those are not blocks a player
// holds; they are states the engine writes.
//
// So the column moved to the generated item table, where it can be *derived*:
// `tools/genref.java --items` measures which block each item places by using it
// in a real world, and hides an ItemBlock whose block already has a carried
// form. That one rule covers doors, signs, reeds, seeds and redstone without
// naming any of them. Four ids -- flowing water, flowing lava, fire and the
// burning furnace -- are excluded by hand and are the only judgement in it;
// they are spelled out in that tool rather than dressed up as a derivation.
//
// **And it is the whole table now, not the two thirds of it that place a
// block.** The rule used to be "nothing that places no block is offered", on
// the argument that a sword does nothing this build can perform. That was the
// wrong test and it was reported as one: a Creative hand is also how a sword,
// an ingot, a smelted ore or a piece of armour gets into a chest, into a save,
// or on to the ground -- and a catalogue that silently omits 84 of its 149 rows
// reads as a table with holes in it. Using one still does nothing, which is
// honest; not being able to hold one was not.
//
// **What it still does not do**, all deliberate:
//
//   * No ordering by category. Id order is the one ordering that is stable
//     across builds and derivable from the data.
//   * No filtering by "would a player ever get this". Bedrock, the mob spawner
//     and the double slab are all in it. A Creative mode with opinions about
//     what you should want is worse than one without.
//   * No naming of what an item is *for*. A hoe and a fishing rod are offered
//     beside a sword and do the same amount, which is nothing.

#include "core/item/item_def.hpp"

namespace mc::item {

// **The two music discs are in it**, and that is the one thing in the palette
// that is not a row of the contiguous item table: they are ids 2256 and 2257,
// which is 1,910 past the end of a1.1.2's item run, and they reach `item::def`
// through a two-entry side array (core/item/registry.hpp). They belong here
// because a skeleton killing a creeper drops one -- the version's only source
// of a record -- so they are obtainable, and a catalogue that omitted the two
// items a player is least likely to find by accident would have the wrong
// hole in it.
//
// How many items the palette offers. For a1.1.2 it is 149.
int paletteSize();

// The item at `index`, or 0 for an index outside the palette. Zero is the
// out-of-range answer rather than an assert because this is read by a UI whose
// page arithmetic can legitimately run off the end of the last page.
ItemId paletteItem(int index);

// Where an item sits in the palette, or -1 for one that is not in it. The
// inverse of the above, and what lets a hotbar slot show which cell it came
// from.
int paletteIndexOf(ItemId id);

}  // namespace mc::item
