#pragma once

// **The `TileEntities` list, modelled rather than preserved** -- `ic`, and the
// four subclasses a1.1.2 registers.
//
// Until now a column's `TileEntities` was an opaque blob (core/nbt/preserved.hpp)
// that came off the card and went back unchanged. That was enough to read a
// spawner's mob out of a real world and nowhere near enough to write one back:
// re-encoding the list means being able to re-encode *every* tenant of it, and
// one of them is a chest holding up to 27 stacks. A build that wrote the list
// without modelling the chest would empty every dungeon chest in the world.
//
// `ic`'s static initialiser is the whole registry, and it has exactly four
// entries:
//
//     a(ke.class, "Furnace");     a(fe.class, "Chest");
//     a(ob.class, "Sign");        a(bd.class, "MobSpawner");
//
// so `TileEntityKind` is those four in that order, plus `Unknown` for an id
// this build has never heard of -- a later version's, a server's, a mod's. An
// unknown tenant keeps every tag it arrived with and is written back verbatim;
// it is never reinterpreted and never dropped.
//
// **What every tile entity carries** is `ic.a(hm)`/`ic.b(hm)`: an `id` string
// and `x`, `y`, `z` as Ints, absolute rather than chunk-relative. The subclass
// tags follow:
//
//   | id           | class | tags                                           |
//   | Furnace      | `ke`  | `Items` (3 slots), `BurnTime` S, `CookTime` S  |
//   | Chest        | `fe`  | `Items` (27 slots)                             |
//   | Sign         | `ob`  | `Text1`..`Text4`, truncated to 15 on read      |
//   | MobSpawner   | `bd`  | `EntityId` String, `Delay` Short               |
//
// **A tile entity with no `id`, or with no position, is dropped** -- which is
// what the original does too. `ic.c(hm)` looks the id up, finds nothing, prints
// "Skipping TileEntity with id null" and returns null, and the chunk loader
// never puts a null in the map, so it is never written back either. That is the
// one lossy case here and it is lossy in the same place the original is.
//
// **The chest's 36-against-27.** `fe`'s constructor allocates `new ev[36]`
// while `getSizeInventory` answers 27, and `fe.a(hm)` reallocates the array to
// `c()` -- so a chest is 36 slots long until it has been round-tripped once and
// 27 afterwards. Nothing can reach slots 27..35, because every writer goes
// through the `gh` interface and that asks `c()`. 27 is therefore the real
// size and the extra nine are a bug with no observable effect; `kChestSlots`
// is 27 and `kChestSlotsAllocated` records the other number so the next reader
// of `fe` does not have to re-derive that it does not matter.
//
// **Slots are sparse on disk and sparse here.** `Items` holds one compound per
// occupied slot, tagged with its index, and an empty slot is simply absent --
// so `items` is the file's own shape and a chest of one item costs one entry.

#include "core/block/block_def.hpp"
#include "core/item/item_stack.hpp"
#include "core/nbt/preserved.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

class ChunkColumn;

// The registration order of `ic`'s static block, so the numbering is the
// original's own and not ours.
enum class TileEntityKind : u8 {
    Furnace = 0,
    Chest,
    Sign,
    MobSpawner,
    // Not in `ic`'s map: an id this build does not model. Round-tripped whole.
    Unknown,
};

// `fe.c()` and `ke.c()`. The chest's backing array is longer; see the header
// note.
inline constexpr int kChestSlots = 27;
inline constexpr int kChestSlotsAllocated = 36;
inline constexpr int kFurnaceSlots = 3;

// Four lines of fifteen characters. `ob.a(hm)` truncates anything longer on
// read, so sixteen bytes a line is lossless as well as NUL-terminated -- the
// same shape core/world/sign_store.hpp uses, and deliberately the same numbers.
inline constexpr int kTileSignLines = 4;
inline constexpr int kTileSignLineLength = 15;
inline constexpr int kTileSignLineBytes = kTileSignLineLength + 1;

// `bd`'s constructor: `"Pig"` on a twenty-tick delay. What a spawner block put
// down by hand is, and what a spawner written without an `EntityId` reads as.
inline constexpr const char* kDefaultSpawnerMob = "Pig";
inline constexpr i16 kDefaultSpawnerDelay = 20;

struct TileEntity {
    // Absolute block coordinates, which is how `ic` stores them. (The chunk's
    // own map is keyed on chunk-relative ones; the NBT is not.)
    i32 x = 0;
    int y = 0;
    i32 z = 0;

    TileEntityKind kind = TileEntityKind::Unknown;

    // Exactly the string that was in the file, and exactly the string written
    // back. For a known kind it equals tileEntityId(kind); for Unknown it is
    // whatever the file said, which is the only thing that can identify it.
    std::string id;

    // `bd.b` -- the mob's save id, and the `bd.a` countdown. `delay` is a
    // Short on disk and an int in the class, so -1 (the "seed me" value) round
    // trips.
    std::string entityId;
    i16 delay = kDefaultSpawnerDelay;

    // `ke.b`/`ke.d`. `ke.c` (currentItemBurnTime) is not saved: `ke.a(hm)`
    // recomputes it from the fuel slot, so there is nothing to keep.
    i16 burnTime = 0;
    i16 cookTime = 0;

    // `ke.c` -- currentItemBurnTime. **Runtime only**: never encoded, and zero
    // after a load until the furnace next takes fuel, where `ke.a(hm)`
    // recomputes it from the fuel slot. Only the screen's flame reads it.
    i16 currentBurnTime = 0;

    // `ob.a`. Fixed-width because the original's own limit is fifteen.
    char lines[kTileSignLines][kTileSignLineBytes] = {};

    // `fe.a`/`ke.a`: the occupied slots only, each carrying its own index in
    // `ItemStack::slot`. Never longer than kChestSlots.
    std::vector<item::ItemStack> items;

    // Tags inside this compound that this build does not model. For Unknown
    // that is everything except `id` and the position; for the four known kinds
    // it is empty in every file a real client wrote.
    nbt::PreservedTags preserved;

    bool at(i32 bx, int by, i32 bz) const { return x == bx && y == by && z == bz; }
};

// The `id` string for a kind, or nullptr for Unknown -- which has no canonical
// name and must use the one it arrived with.
const char* tileEntityId(TileEntityKind kind);

// The reverse, for the four names `ic` registers. False for anything else,
// which is the caller's signal to keep the compound whole.
bool tileEntityKindFromId(std::string_view id, TileEntityKind* out);

// **Which tile entity a block makes**, which is `ly.q[]` (isBlockContainer, set
// by `jt`'s constructor) crossed with each container's `a_()`:
//
//     b  -> fe (Chest)      bj -> bd (MobSpawner)
//     ku -> ke (Furnace)    lr -> ob (Sign), for both the post and the wall
//
// Keyed on the behaviour column rather than on a block id, because no
// subsystem outside the generated table is allowed to know one.
bool tileEntityKindForBlock(block::TickBehaviour behaviour, TileEntityKind* out);

// A default-constructed tenant of that kind, as `a_()` builds it: an empty
// chest, a cold furnace, a blank sign, a `"Pig"` spawner on a 20-tick delay.
TileEntity makeTileEntity(i32 x, int y, i32 z, TileEntityKind kind);

// The entry at a block, or null. Linear: a column holds a handful of these and
// a real one usually holds none.
TileEntity* findTileEntity(std::vector<TileEntity>& list, i32 x, int y, i32 z);
const TileEntity* findTileEntity(const std::vector<TileEntity>& list, i32 x, int y, i32 z);

// Creates or replaces the entry at a block, and returns it. A position holds at
// most one tile entity, which is `Chunk.setChunkBlockTileEntity` putting into a
// map keyed on the position.
TileEntity& putTileEntity(std::vector<TileEntity>& list, i32 x, int y, i32 z,
                          TileEntityKind kind);

// `cn.l(III)V`. True when there was one to forget.
bool eraseTileEntity(std::vector<TileEntity>& list, i32 x, int y, i32 z);

// **Makes the list agree with the blocks**, which is `ga.d(III)Lic;`'s missing
// half done in bulk instead of on demand.
//
// The original heals lazily: ask a chunk for the tile entity at a position, and
// if the map has none but the block is a container, it calls `jt.e` on the spot
// to build one. That works there because every access goes through that one
// method. Here the list is only ever read at a save, so the same rule is
// applied once, over the whole column, at the moment it matters:
//
//   - a container block with no entry (or an entry of the wrong kind) gets a
//     fresh default one, exactly as `jt.e` would have;
//   - a *known* entry whose block is no longer that container is dropped,
//     which is `jt.b` -- and doing it here means an explosion, a fluid or a
//     fire can take a chest away without every one of those paths needing to
//     know what a tile entity is.
//
// **Unknown entries are never touched.** This build cannot tell which block a
// modded tile entity belongs to, and guessing would delete it.
//
// Returns the number of entries added plus removed, which is zero for the
// overwhelming majority of columns.
int reconcileTileEntities(ChunkColumn& column);

// What the list costs in the heap, for the memory budget.
usize tileEntityMemoryUsage(const std::vector<TileEntity>& list);

// Copies a line in, truncated to fifteen characters and always terminated --
// `ob.a(hm)`'s own rule, in the one place that has to apply it.
void setTileSignLine(TileEntity& tile, int line, std::string_view text);

}  // namespace mc::world
