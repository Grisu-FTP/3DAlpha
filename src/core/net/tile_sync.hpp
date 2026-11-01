#pragma once

// **A chest's or a furnace's contents, as they cross the link.**
//
// a1.1.2 has no window packets, so a guest could never open either: the
// columns it is sent carry no tile entities, and a screen opened on the empty
// copy it grew locally would let the player "take" things that are not there.
//
// The answer is to make the guest's own copy true for as long as the screen is
// up. The host sends the tile entity's contents (`packet::TileSync`) when the
// guest opens it and again whenever they change underneath; the guest writes
// them into the column's tile entity, where `item::ContainerSession` already
// reads and clicks -- so every rule about what a click does stays in one place
// -- and sends the contents back after each click, which is how the host learns
// what moved.
//
// **Whoever clicked is believed.** A guest's snapshot replaces the host's slots
// outright. That is exact for one person at a chest; two people at once, or a
// furnace finishing an item in the instant a guest takes the last one, can
// lose a stack to whichever write came second. A furnace's own *progress*
// (burn, cook) is the host's alone and a guest's copy of it is ignored.
//
// **Payload**, big-endian by explicit shifts, in `packet::TileSync`'s bytes:
//
//   u8  kind            1 furnace, 2 chest
//   i16 burn, cook, currentBurn           furnace only
//   u8  n               occupied slots
//   n x { u8 slot, i16 id, i8 count, i16 damage }

#include "core/item/item_stack.hpp"
#include "core/net/packets.hpp"
#include "core/tick/behaviour.hpp"
#include "core/util/types.hpp"
#include "core/world/tile_entity.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {
class MinecartSystem;
}

namespace mc::net {

enum class TileKind : u8 { None = 0, Furnace = 1, Chest = 2 };

struct TileSnapshot {
    TileKind kind = TileKind::None;
    i32 x = 0;
    int y = 0;
    i32 z = 0;
    // Furnace only: `ke.b`, `ke.d` and the runtime `ke.c`.
    i16 burn = 0;
    i16 cook = 0;
    i16 currentBurn = 0;
    // Dense, `world::kChestSlots` long at most; a furnace uses the first three.
    int slotCount = 0;
    item::ItemStack slots[world::kChestSlots];
};

// Which tile entities the block at x, y, z opens a screen on: itself for a
// furnace, every half and neighbour for a chest (`tick::chestInventoryParts`).
// Zero when it is neither.
int tileParts(const tick::TickWorld& world, i32 x, int y, i32 z,
              tick::ChestPart out[tick::kMaxChestParts]);

// The tile entity at the block, which is made empty if it is missing -- the
// lazy heal a container block placed since the last save needs. False when the
// block is not a furnace or a chest, or its column is not resident.
bool readTile(const tick::TickWorld& world, i32 x, int y, i32 z, TileSnapshot* out);

// Writes the snapshot's slots into the block's tile entity, and its progress
// too when `progress`. False when the block is not the container it was.
bool writeTile(tick::TickWorld& world, const TileSnapshot& snapshot, bool progress);

// The payload above, and back. `decodeTile` fails on a short or malformed one.
void encodeTile(const TileSnapshot& snapshot, std::vector<u8>* out);
bool decodeTile(const u8* data, usize size, TileSnapshot* out);

// **A chest cart's 27 slots**, which have no cell. They travel in the same three
// packets a chest's do, as a "tile" whose y is `kCartTileY` -- below the world,
// so never a block -- and whose x is the cart's entity id (z is 0). The rules
// are a chest's: the host sends them on open and on change, the guest sends
// them after each click, and whoever clicked is believed. An older host finds
// no container at y -1 and answers with a TileClose, which is the right answer.
inline constexpr int kCartTileY = -1;

// The cart's slots as a snapshot, and back. False when the entity id is not a
// live chest cart in `carts`.
bool readCart(entity::MinecartSystem& carts, i32 entityId, TileSnapshot* out);
bool writeCart(entity::MinecartSystem& carts, const TileSnapshot& snapshot);

Packet makeTileSync(const TileSnapshot& snapshot);
bool readTileSync(const Packet& packet, TileSnapshot* out);

// Same contents, ignoring where they are and (unless `progress`) how far a
// furnace has cooked -- what "did a click change anything" asks.
bool sameTile(const TileSnapshot& a, const TileSnapshot& b, bool progress);

}  // namespace mc::net
