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
    }
    if (pools.items) {
        pools.items->clear();
        copyIn(items, &pools.items->items_, &pools.items->refused_);
    }
    if (pools.fallingBlocks) {
        pools.fallingBlocks->clear();
        copyIn(fallingBlocks, &pools.fallingBlocks->items_, &pools.fallingBlocks->refused_);
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
    w.writeByte("onGround", static_cast<i8>(e.onGround));
    w.writeByte("flipped", static_cast<i8>(e.flipped));
    w.writeByte("type", static_cast<i8>(e.type));
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
        } else if (!r.skipValue(type)) return false;
    }
    return r.ok() && version == 1;
}
}  // namespace mc::entity
