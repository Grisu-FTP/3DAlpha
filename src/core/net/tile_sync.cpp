// A chest's or a furnace's contents on the wire. See tile_sync.hpp.

#include "core/net/tile_sync.hpp"

#include "core/block/registry.hpp"
#include "core/entity/minecart.hpp"
#include "core/tick/furnace.hpp"
#include "core/tick/tick_world.hpp"

#include <vector>

namespace mc::net {

namespace {

bool sameStack(const item::ItemStack& a, const item::ItemStack& b)
{
    if (a.empty() || b.empty()) {
        return a.empty() == b.empty();
    }
    return a.id == b.id && a.count == b.count && a.damage == b.damage;
}

void putI16(std::vector<u8>* out, i16 v)
{
    out->push_back(u8((u16(v) >> 8) & 0xFF));
    out->push_back(u8(u16(v) & 0xFF));
}

struct Reader {
    const u8* data;
    usize size;
    usize at = 0;

    bool u8At(u8* out)
    {
        if (at + 1 > size) return false;
        *out = data[at++];
        return true;
    }
    bool i16At(i16* out)
    {
        if (at + 2 > size) return false;
        *out = i16((u16(data[at]) << 8) | u16(data[at + 1]));
        at += 2;
        return true;
    }
};

TileKind kindOf(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    switch (block::def(world.blockAt(x, y, z)).tick) {
    case block::TickBehaviour::Furnace:
        return TileKind::Furnace;
    case block::TickBehaviour::Chest:
        return TileKind::Chest;
    default:
        return TileKind::None;
    }
}

int slotsOf(TileKind kind)
{
    return kind == TileKind::Furnace ? tick::kFurnaceSlotCount : world::kChestSlots;
}

}  // namespace

int tileParts(const tick::TickWorld& world, i32 x, int y, i32 z,
              tick::ChestPart out[tick::kMaxChestParts])
{
    switch (kindOf(world, x, y, z)) {
    case TileKind::Furnace:
        out[0] = tick::ChestPart{x, y, z};
        return 1;
    case TileKind::Chest:
        return tick::chestInventoryParts(world, x, y, z, out);
    case TileKind::None:
        break;
    }
    return 0;
}

bool readTile(const tick::TickWorld& world, i32 x, int y, i32 z, TileSnapshot* out)
{
    const TileKind kind = kindOf(world, x, y, z);
    if (kind == TileKind::None) {
        return false;
    }
    std::vector<world::TileEntity>* list = world.tileEntitiesAt(x, z);
    if (list == nullptr) {
        return false;
    }
    *out = TileSnapshot{};
    out->kind = kind;
    out->x = x;
    out->y = y;
    out->z = z;
    out->slotCount = slotsOf(kind);
    const world::TileEntity* tile = world::findTileEntity(*list, x, y, z);
    if (tile == nullptr) {
        return true;  // a container block with nothing in it yet
    }
    tick::tileItems(*tile, out->slots, out->slotCount);
    if (kind == TileKind::Furnace) {
        out->burn = tile->burnTime;
        out->cook = tile->cookTime;
        out->currentBurn = tile->currentBurnTime;
    }
    return true;
}

bool writeTile(tick::TickWorld& world, const TileSnapshot& snapshot, bool progress)
{
    const TileKind kind = kindOf(world, snapshot.x, snapshot.y, snapshot.z);
    if (kind == TileKind::None || kind != snapshot.kind) {
        return false;
    }
    std::vector<world::TileEntity>* list = world.tileEntitiesAt(snapshot.x, snapshot.z);
    if (list == nullptr) {
        return false;
    }
    const world::TileEntityKind tileKind = kind == TileKind::Furnace
                                               ? world::TileEntityKind::Furnace
                                               : world::TileEntityKind::Chest;
    world::TileEntity* tile = world::findTileEntity(*list, snapshot.x, snapshot.y, snapshot.z);
    if (tile == nullptr || tile->kind != tileKind) {
        tile = &world::putTileEntity(*list, snapshot.x, snapshot.y, snapshot.z, tileKind);
    }
    tick::setTileItems(*tile, snapshot.slots, slotsOf(kind));
    if (kind == TileKind::Furnace && progress) {
        tile->burnTime = snapshot.burn;
        tile->cookTime = snapshot.cook;
        tile->currentBurnTime = snapshot.currentBurn;
    }
    world.markTileEntityChanged(snapshot.x, snapshot.z);
    return true;
}

void encodeTile(const TileSnapshot& snapshot, std::vector<u8>* out)
{
    out->clear();
    out->push_back(u8(snapshot.kind));
    if (snapshot.kind == TileKind::Furnace) {
        putI16(out, snapshot.burn);
        putI16(out, snapshot.cook);
        putI16(out, snapshot.currentBurn);
    }
    const usize countAt = out->size();
    out->push_back(0);
    u8 occupied = 0;
    for (int i = 0; i < snapshot.slotCount; ++i) {
        const item::ItemStack& stack = snapshot.slots[i];
        if (stack.empty()) {
            continue;
        }
        out->push_back(u8(i));
        putI16(out, stack.id);
        out->push_back(u8(stack.count));
        putI16(out, stack.damage);
        ++occupied;
    }
    (*out)[countAt] = occupied;
}

bool decodeTile(const u8* data, usize size, TileSnapshot* out)
{
    Reader in{data, size};
    u8 kind = 0;
    if (!in.u8At(&kind) || (kind != u8(TileKind::Furnace) && kind != u8(TileKind::Chest))) {
        return false;
    }
    TileSnapshot snapshot;
    snapshot.kind = TileKind(kind);
    snapshot.slotCount = slotsOf(snapshot.kind);
    if (snapshot.kind == TileKind::Furnace) {
        if (!in.i16At(&snapshot.burn) || !in.i16At(&snapshot.cook)
            || !in.i16At(&snapshot.currentBurn)) {
            return false;
        }
    }
    u8 occupied = 0;
    if (!in.u8At(&occupied)) {
        return false;
    }
    for (int n = 0; n < occupied; ++n) {
        u8 slot = 0;
        u8 count = 0;
        i16 id = 0;
        i16 damage = 0;
        if (!in.u8At(&slot) || !in.i16At(&id) || !in.u8At(&count) || !in.i16At(&damage)) {
            return false;
        }
        if (int(slot) >= snapshot.slotCount || id < 0 || count == 0) {
            return false;
        }
        item::ItemStack& stack = snapshot.slots[slot];
        stack.id = id;
        stack.count = i8(count);
        stack.damage = damage;
        stack.slot = i8(slot);
    }
    *out = snapshot;
    return true;
}

bool readCart(entity::MinecartSystem& carts, i32 entityId, TileSnapshot* out)
{
    const int index = carts.indexOfEntity(entityId);
    if (index < 0 || carts[index].type != entity::MinecartType::Chest) {
        return false;
    }
    const item::ItemStack* slots = carts.chestSlots(carts[index].id);
    if (slots == nullptr) {
        return false;
    }
    *out = TileSnapshot{};
    out->kind = TileKind::Chest;
    out->x = entityId;
    out->y = kCartTileY;
    out->z = 0;
    out->slotCount = entity::kMinecartChestSlots;
    for (int i = 0; i < entity::kMinecartChestSlots; ++i) {
        out->slots[i] = slots[i];
    }
    return true;
}

bool writeCart(entity::MinecartSystem& carts, const TileSnapshot& snapshot)
{
    if (snapshot.y != kCartTileY || snapshot.kind != TileKind::Chest) {
        return false;
    }
    const int index = carts.indexOfEntity(snapshot.x);
    if (index < 0 || carts[index].type != entity::MinecartType::Chest) {
        return false;
    }
    item::ItemStack* slots = carts.chestSlots(carts[index].id);
    if (slots == nullptr) {
        return false;
    }
    for (int i = 0; i < entity::kMinecartChestSlots; ++i) {
        slots[i] = snapshot.slots[i];
    }
    return true;
}

Packet makeTileSync(const TileSnapshot& snapshot)
{
    Packet p;
    p.reset(packet::TileSync);
    p.pushInt(snapshot.x);
    p.pushInt(snapshot.y);
    p.pushInt(snapshot.z);
    encodeTile(snapshot, &p.bytes);
    return p;
}

bool readTileSync(const Packet& packet, TileSnapshot* out)
{
    if (packet.id != packet::TileSync) {
        return false;
    }
    TileSnapshot snapshot;
    if (!decodeTile(packet.bytes.data(), packet.bytes.size(), &snapshot)) {
        return false;
    }
    snapshot.x = i32(packet.integer(0));
    snapshot.y = int(packet.integer(1));
    snapshot.z = i32(packet.integer(2));
    *out = snapshot;
    return true;
}

bool sameTile(const TileSnapshot& a, const TileSnapshot& b, bool progress)
{
    if (a.kind != b.kind || a.slotCount != b.slotCount) {
        return false;
    }
    for (int i = 0; i < a.slotCount; ++i) {
        if (!sameStack(a.slots[i], b.slots[i])) {
            return false;
        }
    }
    return !progress || (a.burn == b.burn && a.cook == b.cook && a.currentBurn == b.currentBurn);
}

}  // namespace mc::net
