#pragma once

// a1.1.2's `cg` -- WorldGenDungeons, the mossy room with a spawner and one or
// two chests. Eight tries per chunk, before the ores, and the only generator
// in population that produces anything other than blocks.
//
// **It is the reason tile entities had to exist at all.** A dungeon writes a
// mob spawner and up to two chests, and both carry data no block array can
// hold: the spawner's mob name and the chest's contents. Everything else in
// the world can be reconstructed from 32,768 bytes; these cannot.
//
// The side table is deliberately small. `docs/status.md` records the settled
// decision that chests and furnaces are **not** tickable in a1.1.2 -- only
// `ke` (furnace) and `bd` (spawner) override the tick, chests and signs do
// not -- and that they render as ordinary cubes. So nothing here needs a
// per-block object at runtime; it needs a list of "there is a chest at this
// coordinate holding these stacks", which is exactly what the chunk's
// `TileEntities` NBT list already round-trips as an opaque blob.
//
// The generator emits into `DungeonOutput` rather than writing tile entities
// itself, so the transcription can be verified without a storage layer.
//
// Derivation in docs/worldgen-a1.1.2.md.

#include "core/item/item_stack.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc {
class JavaRandom;
}

namespace mc::worldgen {

class PopulationView;

// One chest the dungeon placed, and what went in it.
struct DungeonChest {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    // Up to eight of the eleven loot rolls produce an item, and two stacks can
    // land in the same slot -- the original overwrites, so this holds what
    // survived rather than what was rolled.
    std::vector<item::ItemStack> contents;
};

// The mob spawner at the room's centre. `mob` is the original's own string and
// is empty when the fourth branch of the picker is somehow reached, which it
// cannot be -- `nextInt(4)` has four outcomes and all four are named. Kept
// because the original has the branch.
struct DungeonSpawner {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    const char* mob = "";
};

struct DungeonOutput {
    bool placed = false;
    DungeonSpawner spawner;
    std::vector<DungeonChest> chests;
};

// `cg.a(cn, Random, int, int, int)`. `x`/`y`/`z` is the driver's own point,
// which for dungeons is offset by +8 in x and z -- unlike every other pass.
//
// **Every draw happens whether or not the room is built**, which is why the
// early returns matter: a refused dungeon still consumes the two size rolls,
// and eight refusals a chunk is the common case.
bool generateDungeon(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z,
                     DungeonOutput* out);

// The loot table, `cg.a(Random)`. Exposed because it is a pure function of the
// stream and worth pinning without building a room around it.
//
// Returns an empty stack for the rolls that produce nothing -- 7, 8 and 9 are
// gated behind a second draw, and 10 always fails.
item::ItemStack rollDungeonLoot(JavaRandom& random);

// `cg.b(Random)`. One of Skeleton, Zombie (twice), or Spider.
const char* rollDungeonMob(JavaRandom& random);

}  // namespace mc::worldgen
