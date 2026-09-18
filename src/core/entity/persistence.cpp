// Serialises the bounded pool snapshot as explicit NBT fields, never native
// struct bytes. This keeps saves independent of host/3DS padding and endian.
#include "core/entity/persistence.hpp"
#include "core/item/registry.hpp"
#include <cmath>

namespace mc::entity {
namespace {
// One live pool into one snapshot list, sized once up front so a large pool
// is one allocation rather than a doubling series.
template <class T, class Pool> bool copyOut(const Pool& live, SavedPool<T>* saved)
{
    if (!saved->reserve(live.size())) return false;
    for (int i = 0; i < live.size(); ++i) {
        if (!saved->push(live[i])) return false;
    }
    return true;
}
template <class T, class Pool> void copyIn(const SavedPool<T>& saved, Pool* live, u32* refused)
{
    for (int i = 0; i < saved.count(); ++i) {
        T* slot = live->push();
        if (slot == nullptr) {
            ++*refused;
            continue;
        }
        *slot = saved[i];
    }
}
}  // namespace
bool PersistentEntities::capture(const EntityPools& pools)
{
    bool ok = true;
    if (pools.paintings) ok = ok && copyOut(pools.paintings->paintings_, &paintings);
    if (pools.arrows) ok = ok && copyOut(pools.arrows->arrows_, &arrows);
    if (pools.boats) ok = ok && copyOut(pools.boats->boats_, &boats);
    if (pools.minecarts) ok = ok && copyOut(pools.minecarts->carts_, &minecarts);
    if (pools.items) ok = ok && copyOut(pools.items->items_, &items);
    if (pools.fallingBlocks) ok = ok && copyOut(pools.fallingBlocks->items_, &fallingBlocks);
    if (pools.primedTnt) ok = ok && copyOut(pools.primedTnt->items_, &primedTnt);
    if (pools.mobs) ok = ok && copyOut(pools.mobs->mobs_, &mobs);
    // **The chest carts' contents, which are not in the carts.** One entry per
    // store the pool holds, keyed by the cart's id; an empty store is written
    // anyway, because "this cart has a chest" is itself worth keeping.
    if (pools.minecarts) {
        // Sized once, exactly as `copyOut` sizes the pools above; a snapshot is
        // filled once and never refilled.
        ok = ok && minecartChests.reserve(int(pools.minecarts->chests_.size()));
        for (const MinecartSystem::CartChest& chest : pools.minecarts->chests_) {
            SavedCartChest row;
            row.cart = chest.cart;
            for (int slot = 0; slot < kMinecartChestSlots; ++slot) {
                const item::ItemStack& stack = chest.slots[slot];
                row.id[slot] = stack.empty() ? item::kEmptyItemId : stack.id;
                row.damage[slot] = stack.empty() ? i16(0) : stack.damage;
                row.count[slot] = stack.empty() ? i8(0) : stack.count;
            }
            ok = ok && minecartChests.push(row);
        }
    }
    return ok;
}
void PersistentEntities::restore(const EntityPools& pools) const
{
    if (pools.paintings) {
        pools.paintings->clear();
        copyIn(paintings, &pools.paintings->paintings_, &pools.paintings->refused_);
    }
    if (pools.arrows) {
        pools.arrows->clear();
        copyIn(arrows, &pools.arrows->arrows_, &pools.arrows->refused_);
    }
    if (pools.boats) {
        pools.boats->clear();
        copyIn(boats, &pools.boats->boats_, &pools.boats->refused_);
    }
    if (pools.minecarts) {
        pools.minecarts->clear();
        copyIn(minecarts, &pools.minecarts->carts_, &pools.minecarts->refused_);

        // **Every cart gets an id, and the counter goes past the highest.** A
        // world saved before carts had ids reads them all as 0, which is the
        // "no id" value -- so they are handed fresh ones here rather than left
        // sharing one, and the chest store below then finds nothing for them,
        // which is right: that world had no cart inventories to lose.
        MinecartSystem& carts = *pools.minecarts;
        u32 highest = 0;
        for (int i = 0; i < carts.carts_.size(); ++i) {
            highest = carts.carts_[i].id > highest ? carts.carts_[i].id : highest;
        }
        carts.nextId_ = highest + 1;
        for (int i = 0; i < carts.carts_.size(); ++i) {
            if (carts.carts_[i].id == 0) {
                carts.carts_[i].id = carts.nextId_++;
            }
        }

        for (int i = 0; i < minecartChests.count(); ++i) {
            const SavedCartChest& row = minecartChests[i];
            if (carts.indexOfId(row.cart) < 0) {
                continue;  // a chest for a cart that did not survive the read
            }
            MinecartSystem::CartChest* store = carts.openChestStore(row.cart);
            for (int slot = 0; slot < kMinecartChestSlots; ++slot) {
                item::ItemStack stack;
                if (row.count[slot] > 0 && row.id[slot] != item::kEmptyItemId) {
                    stack.id = row.id[slot];
                    stack.damage = row.damage[slot];
                    stack.count = row.count[slot];
                }
                stack.slot = i8(slot);
                store->slots[slot] = stack;
            }
        }
    }
    if (pools.items) {
        pools.items->clear();
        copyIn(items, &pools.items->items_, &pools.items->refused_);
    }
    if (pools.fallingBlocks) {
        pools.fallingBlocks->clear();
        copyIn(fallingBlocks, &pools.fallingBlocks->items_, &pools.fallingBlocks->refused_);
    }
    if (pools.primedTnt) {
        pools.primedTnt->clear();
        copyIn(primedTnt, &pools.primedTnt->items_, &pools.primedTnt->refused_);
    }
    if (pools.mobs) {
        pools.mobs->clear();
        copyIn(mobs, &pools.mobs->mobs_, &pools.mobs->refused_);
        // **`Mob::handle` is not in the save**, because the thing it stands in
        // for -- `shootingEntity` -- is a live reference a1.1.2 never writes.
        // Every mob therefore arrives holding zero, which is the value the
        // arrow sweep reads as "fired by nobody"; without this an arrow in
        // flight across a reload would decline to hit any of them.
        pools.mobs->reissueHandles();
    }
}
namespace {
using nbt::TagType;
void write(nbt::Writer& w, const Painting& e)
{
    w.writeInt("tileX", e.tileX);
    w.writeInt("tileY", e.tileY);
    w.writeInt("tileZ", e.tileZ);
    w.writeInt("direction", e.direction);
    w.writeInt("art", e.art);
    w.writeInt("checkIn", e.checkIn);
}
bool read(nbt::Reader& r, Painting* out, bool* keep)
{
    Painting e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "tileX") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.tileX = static_cast<decltype(e.tileX)>(r.intValue());
        } else if (name == "tileY") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.tileY = static_cast<decltype(e.tileY)>(r.intValue());
        } else if (name == "tileZ") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.tileZ = static_cast<decltype(e.tileZ)>(r.intValue());
        } else if (name == "direction") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.direction = static_cast<decltype(e.direction)>(r.intValue());
        } else if (name == "art") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.art = static_cast<decltype(e.art)>(r.intValue());
        } else if (name == "checkIn") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.checkIn = static_cast<decltype(e.checkIn)>(r.intValue());
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    if (e.direction < 0 || e.direction > 3 || e.art < 0 || e.art >= kPaintingArtCount || e.checkIn < 0 || e.checkIn > 100 || std::abs(double(e.tileX)) > 32000000 || std::abs(double(e.tileY)) > 32000000 || std::abs(double(e.tileZ)) > 32000000) { *keep = false; return true; }
    setPaintingDirection(&e, e.direction);
    for (auto& light : e.cellLight) light = 0xf0;
    e.alive = true;
    e.light = 0xf0;
    *out = e;
    return true;
}
void write(nbt::Writer& w, const Arrow& e)
{
    w.writeDouble("x", e.x);
    w.writeDouble("y", e.y);
    w.writeDouble("z", e.z);
    w.writeDouble("motionX", e.motionX);
    w.writeDouble("motionY", e.motionY);
    w.writeDouble("motionZ", e.motionZ);
    w.writeFloat("yaw", e.yaw);
    w.writeFloat("pitch", e.pitch);
    w.writeInt("tileX", e.tileX);
    w.writeInt("tileY", e.tileY);
    w.writeInt("tileZ", e.tileZ);
    w.writeInt("inTile", e.inTile);
    w.writeInt("shake", e.shake);
    w.writeInt("ticksInGround", e.ticksInGround);
    w.writeInt("ticksInAir", e.ticksInAir);
    w.writeByte("inGround", static_cast<i8>(e.inGround));
    // **Who fired it.** a1.1.2 does not save `shootingEntity` -- a reloaded
    // arrow has forgotten -- and this port does, because forgetting would mean
    // a creeper shot across a world close drops no record, and that drop is the
    // only source of one in this version. `shooterGrace` is *not* saved, for
    // the reason the jar does not save the reference: five ticks is not a thing
    // that survives a close.
    w.writeByte("Shooter", static_cast<i8>(e.shooter));
}
bool read(nbt::Reader& r, Arrow* out, bool* keep)
{
    Arrow e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "x") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.x = static_cast<decltype(e.x)>(r.doubleValue());
            if (!std::isfinite(e.x) || std::abs(e.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.y = static_cast<decltype(e.y)>(r.doubleValue());
            if (!std::isfinite(e.y) || std::abs(e.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.z = static_cast<decltype(e.z)>(r.doubleValue());
            if (!std::isfinite(e.z) || std::abs(e.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionX = static_cast<decltype(e.motionX)>(r.doubleValue());
            if (!std::isfinite(e.motionX) || std::abs(e.motionX) > 32000000) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionY = static_cast<decltype(e.motionY)>(r.doubleValue());
            if (!std::isfinite(e.motionY) || std::abs(e.motionY) > 32000000) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionZ = static_cast<decltype(e.motionZ)>(r.doubleValue());
            if (!std::isfinite(e.motionZ) || std::abs(e.motionZ) > 32000000) *keep = false;
        } else if (name == "yaw") {
            if (!nbt::expectType(r, type, TagType::Float)) return false;
            e.yaw = static_cast<decltype(e.yaw)>(r.floatValue());
            if (!std::isfinite(e.yaw) || std::abs(e.yaw) > 32000000) *keep = false;
        } else if (name == "pitch") {
            if (!nbt::expectType(r, type, TagType::Float)) return false;
            e.pitch = static_cast<decltype(e.pitch)>(r.floatValue());
            if (!std::isfinite(e.pitch) || std::abs(e.pitch) > 32000000) *keep = false;
        } else if (name == "tileX") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.tileX = static_cast<decltype(e.tileX)>(r.intValue());
        } else if (name == "tileY") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.tileY = static_cast<decltype(e.tileY)>(r.intValue());
        } else if (name == "tileZ") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.tileZ = static_cast<decltype(e.tileZ)>(r.intValue());
        } else if (name == "inTile") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.inTile = static_cast<decltype(e.inTile)>(r.intValue());
        } else if (name == "shake") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.shake = static_cast<decltype(e.shake)>(r.intValue());
        } else if (name == "ticksInGround") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.ticksInGround = static_cast<decltype(e.ticksInGround)>(r.intValue());
        } else if (name == "ticksInAir") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.ticksInAir = static_cast<decltype(e.ticksInAir)>(r.intValue());
        } else if (name == "Shooter") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.shooter = r.byteValue() == i8(ArrowShooter::Skeleton) ? ArrowShooter::Skeleton
                                                                    : ArrowShooter::Player;
        } else if (name == "inGround") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.inGround = static_cast<decltype(e.inGround)>(r.byteValue());
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    e.setPosition(e.x, e.y, e.z);
    e.prevX = e.x; e.prevY = e.y; e.prevZ = e.z;
    e.prevYaw = e.yaw;
    e.prevPitch = e.pitch;
    if (e.ticksInGround < 0 || e.ticksInGround > 1200 || e.ticksInAir < 0 || e.ticksInAir > 1000000 || e.shake < 0 || e.shake > 1000) { *keep = false; return true; }
    e.alive = true;
    e.light = 0xf0;
    *out = e;
    return true;
}
void write(nbt::Writer& w, const Boat& e)
{
    w.writeDouble("x", e.x);
    w.writeDouble("y", e.y);
    w.writeDouble("z", e.z);
    w.writeDouble("motionX", e.motionX);
    w.writeDouble("motionY", e.motionY);
    w.writeDouble("motionZ", e.motionZ);
    w.writeFloat("yaw", e.yaw);
    w.writeInt("damage", e.damage);
    w.writeInt("timeSinceHit", e.timeSinceHit);
    w.writeInt("forwardDirection", e.forwardDirection);
    w.writeShort("Fire", e.fire);
    w.writeByte("onGround", static_cast<i8>(e.onGround));
}
bool read(nbt::Reader& r, Boat* out, bool* keep)
{
    Boat e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "x") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.x = static_cast<decltype(e.x)>(r.doubleValue());
            if (!std::isfinite(e.x) || std::abs(e.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.y = static_cast<decltype(e.y)>(r.doubleValue());
            if (!std::isfinite(e.y) || std::abs(e.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.z = static_cast<decltype(e.z)>(r.doubleValue());
            if (!std::isfinite(e.z) || std::abs(e.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionX = static_cast<decltype(e.motionX)>(r.doubleValue());
            if (!std::isfinite(e.motionX) || std::abs(e.motionX) > 32000000) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionY = static_cast<decltype(e.motionY)>(r.doubleValue());
            if (!std::isfinite(e.motionY) || std::abs(e.motionY) > 32000000) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionZ = static_cast<decltype(e.motionZ)>(r.doubleValue());
            if (!std::isfinite(e.motionZ) || std::abs(e.motionZ) > 32000000) *keep = false;
        } else if (name == "yaw") {
            if (!nbt::expectType(r, type, TagType::Float)) return false;
            e.yaw = static_cast<decltype(e.yaw)>(r.floatValue());
            if (!std::isfinite(e.yaw) || std::abs(e.yaw) > 32000000) *keep = false;
        } else if (name == "damage") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.damage = static_cast<decltype(e.damage)>(r.intValue());
        } else if (name == "timeSinceHit") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.timeSinceHit = static_cast<decltype(e.timeSinceHit)>(r.intValue());
        } else if (name == "forwardDirection") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.forwardDirection = static_cast<decltype(e.forwardDirection)>(r.intValue());
        } else if (name == "Fire") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.fire = static_cast<decltype(e.fire)>(r.shortValue());
        } else if (name == "onGround") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.onGround = static_cast<decltype(e.onGround)>(r.byteValue());
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    e.setPosition(e.x, e.y, e.z);
    e.prevX = e.x; e.prevY = e.y; e.prevZ = e.z;
    e.prevYaw = e.yaw;
    if (e.damage < 0 || e.damage > 1000000 || e.timeSinceHit < 0 || e.timeSinceHit > 1000000 || (e.forwardDirection != -1 && e.forwardDirection != 1)) { *keep = false; return true; }
    e.alive = true;
    e.light = 0xf0;
    *out = e;
    return true;
}
void write(nbt::Writer& w, const Minecart& e)
{
    w.writeDouble("x", e.x);
    w.writeDouble("y", e.y);
    w.writeDouble("z", e.z);
    w.writeDouble("motionX", e.motionX);
    w.writeDouble("motionY", e.motionY);
    w.writeDouble("motionZ", e.motionZ);
    w.writeDouble("pushX", e.pushX);
    w.writeDouble("pushZ", e.pushZ);
    w.writeFloat("yaw", e.yaw);
    w.writeInt("fuel", e.fuel);
    w.writeInt("damage", e.damage);
    w.writeInt("timeSinceHit", e.timeSinceHit);
    w.writeInt("forwardDirection", e.forwardDirection);
    w.writeShort("Fire", e.fire);
    w.writeByte("onGround", static_cast<i8>(e.onGround));
    w.writeByte("flipped", static_cast<i8>(e.flipped));
    w.writeByte("type", static_cast<i8>(e.type));
    // **The handle its contents are filed under.** A save written before carts
    // had one reads back as 0, which `restore` hands a fresh id -- see there.
    w.writeInt("id", static_cast<i32>(e.id));
}
bool read(nbt::Reader& r, Minecart* out, bool* keep)
{
    Minecart e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "x") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.x = static_cast<decltype(e.x)>(r.doubleValue());
            if (!std::isfinite(e.x) || std::abs(e.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.y = static_cast<decltype(e.y)>(r.doubleValue());
            if (!std::isfinite(e.y) || std::abs(e.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.z = static_cast<decltype(e.z)>(r.doubleValue());
            if (!std::isfinite(e.z) || std::abs(e.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionX = static_cast<decltype(e.motionX)>(r.doubleValue());
            if (!std::isfinite(e.motionX) || std::abs(e.motionX) > 32000000) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionY = static_cast<decltype(e.motionY)>(r.doubleValue());
            if (!std::isfinite(e.motionY) || std::abs(e.motionY) > 32000000) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionZ = static_cast<decltype(e.motionZ)>(r.doubleValue());
            if (!std::isfinite(e.motionZ) || std::abs(e.motionZ) > 32000000) *keep = false;
        } else if (name == "pushX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.pushX = static_cast<decltype(e.pushX)>(r.doubleValue());
            if (!std::isfinite(e.pushX) || std::abs(e.pushX) > 32000000) *keep = false;
        } else if (name == "pushZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.pushZ = static_cast<decltype(e.pushZ)>(r.doubleValue());
            if (!std::isfinite(e.pushZ) || std::abs(e.pushZ) > 32000000) *keep = false;
        } else if (name == "yaw") {
            if (!nbt::expectType(r, type, TagType::Float)) return false;
            e.yaw = static_cast<decltype(e.yaw)>(r.floatValue());
            if (!std::isfinite(e.yaw) || std::abs(e.yaw) > 32000000) *keep = false;
        } else if (name == "id") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            const i32 id = r.intValue();
            e.id = id > 0 ? u32(id) : 0u;
        } else if (name == "fuel") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.fuel = static_cast<decltype(e.fuel)>(r.intValue());
        } else if (name == "damage") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.damage = static_cast<decltype(e.damage)>(r.intValue());
        } else if (name == "timeSinceHit") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.timeSinceHit = static_cast<decltype(e.timeSinceHit)>(r.intValue());
        } else if (name == "forwardDirection") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.forwardDirection = static_cast<decltype(e.forwardDirection)>(r.intValue());
        } else if (name == "Fire") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.fire = static_cast<decltype(e.fire)>(r.shortValue());
        } else if (name == "onGround") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.onGround = static_cast<decltype(e.onGround)>(r.byteValue());
        } else if (name == "flipped") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.flipped = static_cast<decltype(e.flipped)>(r.byteValue());
        } else if (name == "type") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.type = static_cast<decltype(e.type)>(r.byteValue());
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    e.setPosition(e.x, e.y, e.z);
    e.prevX = e.x; e.prevY = e.y; e.prevZ = e.z;
    e.prevYaw = e.yaw;
    if (int(e.type) > 2 || e.fuel < 0 || e.fuel > 100000000) { *keep = false; return true; }
    if (e.damage < 0 || e.damage > 1000000 || e.timeSinceHit < 0 || e.timeSinceHit > 1000000 || (e.forwardDirection != -1 && e.forwardDirection != 1)) { *keep = false; return true; }
    e.alive = true;
    e.light = 0xf0;
    *out = e;
    return true;
}
void write(nbt::Writer& w, const ItemEntity& e)
{
    w.writeDouble("x", e.x);
    w.writeDouble("y", e.y);
    w.writeDouble("z", e.z);
    w.writeDouble("motionX", e.motionX);
    w.writeDouble("motionY", e.motionY);
    w.writeDouble("motionZ", e.motionZ);
    w.writeFloat("yaw", e.yaw);
    w.writeFloat("hoverPhase", e.hoverPhase);
    w.writeInt("count", e.count);
    w.writeInt("age", e.age);
    w.writeInt("pickupDelay", e.pickupDelay);
    w.writeShort("item", e.item);
    w.writeShort("damage", e.damage);
    // `dx.f` and `kh.aT`: what a burning stack has left, and how long it has
    // left to burn. See core/entity/fire_entry.hpp.
    w.writeShort("Health", e.health);
    w.writeShort("Fire", e.fire);
    w.writeByte("onGround", static_cast<i8>(e.onGround));
}
bool read(nbt::Reader& r, ItemEntity* out, bool* keep)
{
    ItemEntity e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "x") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.x = static_cast<decltype(e.x)>(r.doubleValue());
            if (!std::isfinite(e.x) || std::abs(e.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.y = static_cast<decltype(e.y)>(r.doubleValue());
            if (!std::isfinite(e.y) || std::abs(e.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.z = static_cast<decltype(e.z)>(r.doubleValue());
            if (!std::isfinite(e.z) || std::abs(e.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionX = static_cast<decltype(e.motionX)>(r.doubleValue());
            if (!std::isfinite(e.motionX) || std::abs(e.motionX) > 32000000) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionY = static_cast<decltype(e.motionY)>(r.doubleValue());
            if (!std::isfinite(e.motionY) || std::abs(e.motionY) > 32000000) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionZ = static_cast<decltype(e.motionZ)>(r.doubleValue());
            if (!std::isfinite(e.motionZ) || std::abs(e.motionZ) > 32000000) *keep = false;
        } else if (name == "yaw") {
            if (!nbt::expectType(r, type, TagType::Float)) return false;
            e.yaw = static_cast<decltype(e.yaw)>(r.floatValue());
            if (!std::isfinite(e.yaw) || std::abs(e.yaw) > 32000000) *keep = false;
        } else if (name == "hoverPhase") {
            if (!nbt::expectType(r, type, TagType::Float)) return false;
            e.hoverPhase = static_cast<decltype(e.hoverPhase)>(r.floatValue());
            if (!std::isfinite(e.hoverPhase) || std::abs(e.hoverPhase) > 32000000) *keep = false;
        } else if (name == "count") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.count = static_cast<decltype(e.count)>(r.intValue());
        } else if (name == "age") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.age = static_cast<decltype(e.age)>(r.intValue());
        } else if (name == "pickupDelay") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.pickupDelay = static_cast<decltype(e.pickupDelay)>(r.intValue());
        } else if (name == "item") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.item = static_cast<decltype(e.item)>(r.shortValue());
        } else if (name == "damage") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.damage = static_cast<decltype(e.damage)>(r.shortValue());
        } else if (name == "Health") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.health = static_cast<decltype(e.health)>(r.shortValue());
            if (e.health <= 0 || e.health > kItemHealth) *keep = false;
        } else if (name == "Fire") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.fire = static_cast<decltype(e.fire)>(r.shortValue());
        } else if (name == "onGround") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.onGround = static_cast<decltype(e.onGround)>(r.byteValue());
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    e.setPosition(e.x, e.y, e.z);
    e.prevX = e.x; e.prevY = e.y; e.prevZ = e.z;
    if (e.item == 0 || e.item >= mcver::kItemTableSize || e.count <= 0 || e.count > 127 || e.age < 0 || e.age >= kItemMaxAge || e.pickupDelay < 0 || e.pickupDelay > 1000000) { *keep = false; return true; }
    e.light = 0xf0;
    *out = e;
    return true;
}
void write(nbt::Writer& w, const FallingBlock& e)
{
    w.writeDouble("x", e.x);
    w.writeDouble("y", e.y);
    w.writeDouble("z", e.z);
    w.writeDouble("motionX", e.motionX);
    w.writeDouble("motionY", e.motionY);
    w.writeDouble("motionZ", e.motionZ);
    w.writeInt("fallTime", e.fallTime);
    w.writeByte("block", static_cast<i8>(e.block));
    w.writeShort("Fire", e.fire);
    w.writeByte("onGround", static_cast<i8>(e.onGround));
}
bool read(nbt::Reader& r, FallingBlock* out, bool* keep)
{
    FallingBlock e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "x") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.x = static_cast<decltype(e.x)>(r.doubleValue());
            if (!std::isfinite(e.x) || std::abs(e.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.y = static_cast<decltype(e.y)>(r.doubleValue());
            if (!std::isfinite(e.y) || std::abs(e.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.z = static_cast<decltype(e.z)>(r.doubleValue());
            if (!std::isfinite(e.z) || std::abs(e.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionX = static_cast<decltype(e.motionX)>(r.doubleValue());
            if (!std::isfinite(e.motionX) || std::abs(e.motionX) > 32000000) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionY = static_cast<decltype(e.motionY)>(r.doubleValue());
            if (!std::isfinite(e.motionY) || std::abs(e.motionY) > 32000000) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionZ = static_cast<decltype(e.motionZ)>(r.doubleValue());
            if (!std::isfinite(e.motionZ) || std::abs(e.motionZ) > 32000000) *keep = false;
        } else if (name == "fallTime") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            e.fallTime = static_cast<decltype(e.fallTime)>(r.intValue());
        } else if (name == "block") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.block = static_cast<decltype(e.block)>(r.byteValue());
        } else if (name == "Fire") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.fire = static_cast<decltype(e.fire)>(r.shortValue());
        } else if (name == "onGround") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.onGround = static_cast<decltype(e.onGround)>(r.byteValue());
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    e.setPosition(e.x, e.y, e.z);
    e.prevX = e.x; e.prevY = e.y; e.prevZ = e.z;
    if (!e.alive() || e.fallTime < 0 || e.fallTime > kFallingBlockGiveUp) { *keep = false; return true; }
    e.light = 0xf0;
    *out = e;
    return true;
}
// **Primed TNT**, whose whole state past the usual six doubles is one counter.
// `jd.a(Lhm;)V` writes exactly that -- `nbt.setByte("Fuse", (byte)fuse)` -- and
// a byte is enough for 80. The port keeps an int in memory and narrows here, so
// a file this build writes is one a1.1.2 could read if the pools were ever
// moved into the chunks.
void write(nbt::Writer& w, const PrimedTnt& e)
{
    w.writeDouble("x", e.x);
    w.writeDouble("y", e.y);
    w.writeDouble("z", e.z);
    w.writeDouble("motionX", e.motionX);
    w.writeDouble("motionY", e.motionY);
    w.writeDouble("motionZ", e.motionZ);
    w.writeByte("Fuse", static_cast<i8>(e.fuse));
    w.writeShort("Fire", e.fire);
    w.writeByte("onGround", static_cast<i8>(e.onGround));
}
bool read(nbt::Reader& r, PrimedTnt* out, bool* keep)
{
    PrimedTnt e;
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "x") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.x = r.doubleValue();
            if (!std::isfinite(e.x) || std::abs(e.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.y = r.doubleValue();
            if (!std::isfinite(e.y) || std::abs(e.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.z = r.doubleValue();
            if (!std::isfinite(e.z) || std::abs(e.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionX = r.doubleValue();
            if (!std::isfinite(e.motionX) || std::abs(e.motionX) > 32000000) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionY = r.doubleValue();
            if (!std::isfinite(e.motionY) || std::abs(e.motionY) > 32000000) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, type, TagType::Double)) return false;
            e.motionZ = r.doubleValue();
            if (!std::isfinite(e.motionZ) || std::abs(e.motionZ) > 32000000) *keep = false;
        } else if (name == "Fuse") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.fuse = int(r.byteValue());
        } else if (name == "Fire") {
            if (!nbt::expectType(r, type, TagType::Short)) return false;
            e.fire = static_cast<decltype(e.fire)>(r.shortValue());
        } else if (name == "onGround") {
            if (!nbt::expectType(r, type, TagType::Byte)) return false;
            e.onGround = r.byteValue() != 0;
        } else if (!r.skipValue(type)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    e.setPosition(e.x, e.y, e.z);
    e.prevX = e.x; e.prevY = e.y; e.prevZ = e.z;
    // **A fuse past 80 is not ours**, and a negative one is an entity that
    // should already have gone off. Either is a damaged file rather than a
    // state this build can reach, so the entity is dropped and the rest of the
    // level still loads.
    if (e.fuse < 0 || e.fuse > kPrimedTntFuse) { *keep = false; return true; }
    e.active = true;
    e.light = 0xf0;
    *out = e;
    return true;
}
// **An animal.** Its path is deliberately not saved: a route is a plan about a
// world that has been unloaded since, and a mob with no path asks for one on the
// tick after it loads. Neither is the wing flap, which settles within a second.
//
// `Saddle`/`Sheared` are one field in the jar and one field here -- see
// `Mob::flag` -- so the tag is spelled the way a1.1.2 spells whichever of the
// two this animal is.
void write(nbt::Writer& w, const Mob& e)
{
    w.writeByte("Type", static_cast<i8>(e.type));
    w.writeDouble("x", e.body.x);
    w.writeDouble("y", e.body.y);
    w.writeDouble("z", e.body.z);
    w.writeDouble("motionX", e.body.motionX);
    w.writeDouble("motionY", e.body.motionY);
    w.writeDouble("motionZ", e.body.motionZ);
    w.writeFloat("yaw", e.yaw);
    w.writeFloat("pitch", e.pitch);
    w.writeShort("Health", e.health);
    w.writeShort("Air", e.air);
    w.writeShort("Fire", e.fire);
    w.writeByte("Flag", static_cast<i8>(e.flag ? 1 : 0));
    w.writeInt("EggTime", e.eggTime);
    w.writeInt("EntityAge", e.entityAge);

    // **The monsters' five.** `Size` is spelled the way `ma.a(Lhm;)V` spells it
    // -- and stored **one less**, because the jar writes `size - 1` and reads
    // `+ 1`. `Fuse` and `Ignited` are `dd`'s two; `Target` is whether this was
    // chasing the player, which a1.1.2 does not save at all (a reloaded monster
    // re-acquires within a tick) and which is kept here because the port's
    // reload is a session restore rather than a world load. `Riding` is the
    // spider jockey's index, and it survives because the pool is written and
    // read in order.
    w.writeByte("Size", static_cast<i8>(int(e.slimeSize) - 1));
    w.writeShort("Fuse", e.fuse);
    w.writeByte("Ignited", static_cast<i8>(e.creeperState));
    w.writeByte("Target", static_cast<i8>(e.targetingPlayer ? 1 : 0));
    w.writeShort("HopDelay", e.hopDelay);
    w.writeShort("Riding", e.mountIndex);
}
bool read(nbt::Reader& r, Mob* out, bool* keep)
{
    Mob e;
    int type = 0;
    int slimeSize = 1;
    TagType tag;
    std::string_view name;
    while (r.nextField(&tag, &name)) {
        if (name == "Type") {
            if (!nbt::expectType(r, tag, TagType::Byte)) return false;
            type = r.byteValue();
        } else if (name == "x") {
            if (!nbt::expectType(r, tag, TagType::Double)) return false;
            e.body.x = r.doubleValue();
            if (!std::isfinite(e.body.x) || std::abs(e.body.x) > 32000000) *keep = false;
        } else if (name == "y") {
            if (!nbt::expectType(r, tag, TagType::Double)) return false;
            e.body.y = r.doubleValue();
            if (!std::isfinite(e.body.y) || std::abs(e.body.y) > 32000000) *keep = false;
        } else if (name == "z") {
            if (!nbt::expectType(r, tag, TagType::Double)) return false;
            e.body.z = r.doubleValue();
            if (!std::isfinite(e.body.z) || std::abs(e.body.z) > 32000000) *keep = false;
        } else if (name == "motionX") {
            if (!nbt::expectType(r, tag, TagType::Double)) return false;
            e.body.motionX = r.doubleValue();
            if (!std::isfinite(e.body.motionX)) *keep = false;
        } else if (name == "motionY") {
            if (!nbt::expectType(r, tag, TagType::Double)) return false;
            e.body.motionY = r.doubleValue();
            if (!std::isfinite(e.body.motionY)) *keep = false;
        } else if (name == "motionZ") {
            if (!nbt::expectType(r, tag, TagType::Double)) return false;
            e.body.motionZ = r.doubleValue();
            if (!std::isfinite(e.body.motionZ)) *keep = false;
        } else if (name == "yaw") {
            if (!nbt::expectType(r, tag, TagType::Float)) return false;
            e.yaw = r.floatValue();
            if (!std::isfinite(e.yaw)) *keep = false;
        } else if (name == "pitch") {
            if (!nbt::expectType(r, tag, TagType::Float)) return false;
            e.pitch = r.floatValue();
            if (!std::isfinite(e.pitch)) *keep = false;
        } else if (name == "Health") {
            if (!nbt::expectType(r, tag, TagType::Short)) return false;
            e.health = static_cast<i16>(r.shortValue());
        } else if (name == "Air") {
            if (!nbt::expectType(r, tag, TagType::Short)) return false;
            e.air = static_cast<i16>(r.shortValue());
        } else if (name == "Fire") {
            if (!nbt::expectType(r, tag, TagType::Short)) return false;
            e.fire = static_cast<i16>(r.shortValue());
        } else if (name == "Flag") {
            if (!nbt::expectType(r, tag, TagType::Byte)) return false;
            e.flag = r.byteValue() != 0;
        } else if (name == "EggTime") {
            if (!nbt::expectType(r, tag, TagType::Int)) return false;
            e.eggTime = r.intValue();
        } else if (name == "EntityAge") {
            if (!nbt::expectType(r, tag, TagType::Int)) return false;
            e.entityAge = r.intValue();
        } else if (name == "Size") {
            if (!nbt::expectType(r, tag, TagType::Byte)) return false;
            slimeSize = int(r.byteValue()) + 1;
        } else if (name == "Fuse") {
            if (!nbt::expectType(r, tag, TagType::Short)) return false;
            e.fuse = static_cast<i16>(r.shortValue());
        } else if (name == "Ignited") {
            if (!nbt::expectType(r, tag, TagType::Byte)) return false;
            e.creeperState = static_cast<i8>(r.byteValue());
        } else if (name == "Target") {
            if (!nbt::expectType(r, tag, TagType::Byte)) return false;
            e.targetingPlayer = r.byteValue() != 0;
        } else if (name == "HopDelay") {
            if (!nbt::expectType(r, tag, TagType::Short)) return false;
            e.hopDelay = static_cast<i16>(r.shortValue());
        } else if (name == "Riding") {
            if (!nbt::expectType(r, tag, TagType::Short)) return false;
            e.mountIndex = static_cast<i16>(r.shortValue());
        } else if (!r.skipValue(tag)) return false;
    }
    if (!r.ok()) return false;
    if (!*keep) return true;
    if (type < 0 || type >= kMobTypeCount) { *keep = false; return true; }
    e.type = static_cast<MobType>(type);
    const MobDef& def = mobDef(e.type);
    if (e.health <= 0 || e.health > 1000) { *keep = false; return true; }
    if (e.eggTime < 0 || e.eggTime > kEggTimeBase + kEggTimeSpread) e.eggTime = kEggTimeBase;

    // **A slime's box is its size and not the table's.** Anything outside
    // 1, 2, 4 is a file this build did not write; it is clamped to 1 rather
    // than rejected, because a slime the wrong size is still a slime and the
    // alternative is losing it.
    float width = def.width;
    float height = def.height;
    if (e.type == MobType::Slime) {
        if (slimeSize != 1 && slimeSize != 2 && slimeSize != kSlimeMaxSize) slimeSize = 1;
        e.slimeSize = static_cast<u8>(slimeSize);
        width = height = kSlimeSizeUnit * float(e.slimeSize);
        if (e.health > int(e.slimeSize) * int(e.slimeSize)) {
            e.health = static_cast<i16>(int(e.slimeSize) * int(e.slimeSize));
        }
    }
    if (e.fuse < 0 || e.fuse > kFuseTicks) e.fuse = 0;
    e.prevFuse = e.fuse;
    if (e.creeperState < -1 || e.creeperState > 2) e.creeperState = -1;
    if (e.hopDelay < 0) e.hopDelay = 0;
    if (!mobDef(e.type).hostile) e.targetingPlayer = false;
    e.body.setSize(width, height, 0.0f);
    e.body.setFeet(e.body.x, e.body.y, e.body.z);
    e.prevYaw = e.yaw;
    e.prevPitch = e.pitch;
    e.renderYaw = e.yaw;
    e.prevRenderYaw = e.yaw;
    e.prevHealth = e.health;
    e.alive = true;
    e.light = 0xf0;
    *out = e;
    return true;
}
// **A chest cart's 27 slots**, and the shape is the one every inventory in the
// save format has: a sparse list, one compound per occupied slot, tagged with
// its index. An empty cart writes an entry with an empty list -- "this cart has
// a chest" is worth keeping on its own.
void write(nbt::Writer& w, const SavedCartChest& e)
{
    w.writeInt("cart", static_cast<i32>(e.cart));
    w.beginList("Items", TagType::Compound);
    for (int slot = 0; slot < kMinecartChestSlots; ++slot) {
        if (e.count[slot] <= 0 || e.id[slot] == item::kEmptyItemId) {
            continue;
        }
        w.beginListElementCompound();
        w.writeByte("Slot", static_cast<i8>(slot));
        w.writeShort("id", e.id[slot]);
        w.writeShort("Damage", e.damage[slot]);
        w.writeByte("Count", e.count[slot]);
        w.endCompound();
    }
    w.endList();
}

bool read(nbt::Reader& r, SavedCartChest* out, bool* keep)
{
    SavedCartChest e;
    for (int slot = 0; slot < kMinecartChestSlots; ++slot) {
        e.id[slot] = item::kEmptyItemId;
    }
    TagType type;
    std::string_view name;
    while (r.nextField(&type, &name)) {
        if (name == "cart") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            const i32 id = r.intValue();
            if (id <= 0) *keep = false;
            e.cart = id > 0 ? u32(id) : 0u;
        } else if (name == "Items") {
            if (!nbt::expectType(r, type, TagType::List)) return false;
            TagType element;
            i32 count;
            if (!r.enterList(&element, &count) || count < 0
                || (count != 0 && element != TagType::Compound)) {
                return false;
            }
            for (i32 i = 0; i < count; ++i) {
                int slot = -1;
                i16 id = item::kEmptyItemId;
                i16 damage = 0;
                i8 stackCount = 0;
                TagType field;
                std::string_view key;
                while (r.nextField(&field, &key)) {
                    if (key == "Slot") {
                        if (!nbt::expectType(r, field, TagType::Byte)) return false;
                        slot = int(r.byteValue());
                    } else if (key == "id") {
                        if (!nbt::expectType(r, field, TagType::Short)) return false;
                        id = r.shortValue();
                    } else if (key == "Damage") {
                        if (!nbt::expectType(r, field, TagType::Short)) return false;
                        damage = r.shortValue();
                    } else if (key == "Count") {
                        if (!nbt::expectType(r, field, TagType::Byte)) return false;
                        stackCount = r.byteValue();
                    } else if (!r.skipValue(field)) {
                        return false;
                    }
                }
                // A slot outside the cart is dropped rather than failing the
                // read: the rest of the world is still good.
                if (slot >= 0 && slot < kMinecartChestSlots && stackCount > 0) {
                    e.id[slot] = id;
                    e.damage[slot] = damage;
                    e.count[slot] = stackCount;
                }
            }
        } else if (!r.skipValue(type)) {
            return false;
        }
    }
    *out = e;
    return true;
}

template<class T>
void writePool(nbt::Writer& w, std::string_view name, const SavedPool<T>& pool)
{
    w.beginList(name, TagType::Compound);
    for (int i = 0; i < pool.count(); ++i) {
        w.beginListElementCompound();
        write(w, pool[i]);
        w.endCompound();
    }
    w.endList();
}
// **No count limit, and the count in the file is not trusted for one.** Each
// element is read before it is stored, so a damaged length fails on the read
// that runs off the end rather than on an allocation sized from it. An entity
// the heap will not hold is still read -- the stream has to get past it -- and
// then dropped, so a save made on a console with more memory still opens.
//
// **A bad entity is dropped, not the world.** `read` fails only when the
// stream itself is broken. An entity that decodes cleanly but holds an
// impossible value -- a NaN position from a runaway minecart stack, say --
// clears `keep` and is skipped, and everything else in the level still loads.
template<class T>
bool readPool(nbt::Reader& r, TagType type, SavedPool<T>* pool)
{
    if (!nbt::expectType(r, type, TagType::List)) return false;
    TagType element;
    i32 count;
    if (!r.enterList(&element, &count) || count < 0
        || (count != 0 && element != TagType::Compound)) return false;
    for (i32 i = 0; i < count; ++i) {
        T value;
        bool keep = true;
        if (!read(r, &value, &keep)) return false;
        if (keep) (void)pool->push(value);
    }
    return r.ok();
}
}  // namespace
void writePersistentEntities(nbt::Writer& w, const PersistentEntities& state)
{
    w.beginCompound("3DAlphaEntities");
    w.writeInt("Version", 1);
    writePool(w, "paintings", state.paintings);
    writePool(w, "arrows", state.arrows);
    writePool(w, "boats", state.boats);
    writePool(w, "minecarts", state.minecarts);
    writePool(w, "items", state.items);
    writePool(w, "fallingBlocks", state.fallingBlocks);
    // Added without a version bump, on the same argument the animals are --
    // see the note below.
    writePool(w, "primedTnt", state.primedTnt);
    // **Added without a version bump, deliberately.** A build that predates
    // animals skips an unknown list and keeps everything else, and a build that
    // has them reads a file written without one as "no animals" -- so the two
    // directions both work and `Version` still means what it meant.
    writePool(w, "mobs", state.mobs);
    // Added without a version bump, on the same argument as the two above: a
    // build without it skips the list and a build with it reads a file that has
    // none as "no cart inventories".
    writePool(w, "minecartChests", state.minecartChests);
    w.endCompound();
}
bool readPersistentEntities(nbt::Reader& r, PersistentEntities* out)
{
    TagType type;
    std::string_view name;
    int version = 0;
    while (r.nextField(&type, &name)) {
        if (name == "Version") {
            if (!nbt::expectType(r, type, TagType::Int)) return false;
            version = r.intValue();
        } else if (name == "paintings") {
            if (!readPool(r, type, &out->paintings)) return false;
        } else if (name == "arrows") {
            if (!readPool(r, type, &out->arrows)) return false;
        } else if (name == "boats") {
            if (!readPool(r, type, &out->boats)) return false;
        } else if (name == "minecarts") {
            if (!readPool(r, type, &out->minecarts)) return false;
        } else if (name == "items") {
            if (!readPool(r, type, &out->items)) return false;
        } else if (name == "fallingBlocks") {
            if (!readPool(r, type, &out->fallingBlocks)) return false;
        } else if (name == "primedTnt") {
            if (!readPool(r, type, &out->primedTnt)) return false;
        } else if (name == "mobs") {
            if (!readPool(r, type, &out->mobs)) return false;
        } else if (name == "minecartChests") {
            if (!readPool(r, type, &out->minecartChests)) return false;
        } else if (!r.skipValue(type)) return false;
    }
    return r.ok() && version == 1;
}
}  // namespace mc::entity
