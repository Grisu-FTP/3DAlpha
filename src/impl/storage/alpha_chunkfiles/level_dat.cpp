#include "impl/storage/alpha_chunkfiles/level_dat.hpp"

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"

namespace mc::alpha {
namespace {

using item::ItemStack;
using world::LevelData;
using world::PlayerData;

// Position, motion and rotation are fixed-length lists. A wrong element type or
// a wrong count is a malformed file rather than something to work around: there
// is no way to write back what we could not read.
bool enterFixedList(nbt::Reader& r, nbt::TagType want, i32 expected)
{
    nbt::TagType elemType;
    i32 count = 0;
    if (!r.enterList(&elemType, &count) || elemType != want || count != expected) {
        r.fail();
        return false;
    }
    return true;
}

bool readDoubleList(nbt::Reader& r, double* dst, i32 expected)
{
    if (!enterFixedList(r, nbt::TagType::Double, expected)) {
        return false;
    }
    for (i32 i = 0; i < expected; ++i) {
        dst[i] = r.doubleValue();
    }
    return r.ok();
}

bool readFloatList(nbt::Reader& r, float* dst, i32 expected)
{
    if (!enterFixedList(r, nbt::TagType::Float, expected)) {
        return false;
    }
    for (i32 i = 0; i < expected; ++i) {
        dst[i] = r.floatValue();
    }
    return r.ok();
}

bool decodeItemStack(nbt::Reader& r, ItemStack* stack)
{
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "Slot") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            stack->slot = r.byteValue();
        } else if (name == "id") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            stack->id = r.shortValue();
        } else if (name == "Count") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            stack->count = r.byteValue();
        } else if (name == "Damage") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            stack->damage = r.shortValue();
        } else if (!stack->preserved.capture(r, name, type)) {
            return false;
        }
    }
    return r.ok();
}

bool decodeInventory(nbt::Reader& r, PlayerData* player)
{
    nbt::TagType elemType;
    i32 count = 0;
    if (!r.enterList(&elemType, &count)) {
        return false;
    }
    // An empty inventory is written as a list of TAG_End, which is legal and
    // means exactly zero elements.
    if (count != 0 && elemType != nbt::TagType::Compound) {
        r.fail();
        return false;
    }

    player->inventory.clear();
    for (i32 i = 0; i < count; ++i) {
        ItemStack stack;
        if (!decodeItemStack(r, &stack)) {
            return false;
        }
        player->inventory.push_back(std::move(stack));
    }
    return r.ok();
}

bool decodePlayer(nbt::Reader& r, PlayerData* player)
{
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "Dimension") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            player->dimension = r.intValue();
            player->hasDimension = true;
        } else if (name == "Score") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            player->score = r.intValue();
        } else if (name == "Pos") {
            if (!nbt::expectType(r, type, nbt::TagType::List)) return false;
            if (!readDoubleList(r, player->pos, 3)) return false;
        } else if (name == "Motion") {
            if (!nbt::expectType(r, type, nbt::TagType::List)) return false;
            if (!readDoubleList(r, player->motion, 3)) return false;
        } else if (name == "Rotation") {
            if (!nbt::expectType(r, type, nbt::TagType::List)) return false;
            if (!readFloatList(r, player->rotation, 2)) return false;
        } else if (name == "OnGround") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            player->onGround = r.byteValue() != 0;
        } else if (name == "FallDistance") {
            if (!nbt::expectType(r, type, nbt::TagType::Float)) return false;
            player->fallDistance = r.floatValue();
        } else if (name == "Health") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            player->health = r.shortValue();
        } else if (name == "AttackTime") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            player->attackTime = r.shortValue();
        } else if (name == "HurtTime") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            player->hurtTime = r.shortValue();
        } else if (name == "DeathTime") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            player->deathTime = r.shortValue();
        } else if (name == "Air") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            player->air = r.shortValue();
        } else if (name == "Fire") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            player->fire = r.shortValue();
        } else if (name == "Inventory") {
            if (!nbt::expectType(r, type, nbt::TagType::List)) return false;
            if (!decodeInventory(r, player)) return false;
        } else if (!player->preserved.capture(r, name, type)) {
            return false;
        }
    }

    player->present = r.ok();
    return r.ok();
}

bool decodeData(nbt::Reader& r, LevelData* level)
{
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "LastPlayed") {
            if (!nbt::expectType(r, type, nbt::TagType::Long)) return false;
            level->lastPlayed = r.longValue();
        } else if (name == "SizeOnDisk") {
            if (!nbt::expectType(r, type, nbt::TagType::Long)) return false;
            level->sizeOnDisk = r.longValue();
        } else if (name == "RandomSeed") {
            if (!nbt::expectType(r, type, nbt::TagType::Long)) return false;
            level->randomSeed = r.longValue();
        } else if (name == "Time") {
            if (!nbt::expectType(r, type, nbt::TagType::Long)) return false;
            level->time = r.longValue();
        } else if (name == "SnowCovered") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            level->snowCovered = r.byteValue() != 0;
        } else if (name == "SpawnX") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            level->spawnX = r.intValue();
        } else if (name == "SpawnY") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            level->spawnY = r.intValue();
        } else if (name == "SpawnZ") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            level->spawnZ = r.intValue();
        } else if (name == "Player") {
            if (!nbt::expectType(r, type, nbt::TagType::Compound)) return false;
            if (level->player.present || !decodePlayer(r, &level->player)) {
                return false;
            }
        } else if (!level->preserved.capture(r, name, type)) {
            return false;
        }
    }
    return r.ok();
}

void encodePlayer(nbt::Writer& w, const PlayerData& player)
{
    w.beginCompound("Player");
    if (player.hasDimension) {
        w.writeInt("Dimension", player.dimension);
    }

    w.beginList("Pos", nbt::TagType::Double);
    for (double v : player.pos) {
        w.listDouble(v);
    }
    w.endList();

    w.beginList("Rotation", nbt::TagType::Float);
    for (float v : player.rotation) {
        w.listFloat(v);
    }
    w.endList();

    w.beginList("Motion", nbt::TagType::Double);
    for (double v : player.motion) {
        w.listDouble(v);
    }
    w.endList();

    w.writeByte("OnGround", player.onGround ? 1 : 0);
    w.writeFloat("FallDistance", player.fallDistance);
    w.writeShort("Health", player.health);
    w.writeShort("AttackTime", player.attackTime);
    w.writeShort("HurtTime", player.hurtTime);
    w.writeShort("DeathTime", player.deathTime);
    w.writeShort("Air", player.air);
    w.writeShort("Fire", player.fire);
    w.writeInt("Score", player.score);

    // endList() rewrites the element type to Byte if this comes out empty,
    // which is what the original game emits. See core/nbt/writer.cpp.
    w.beginList("Inventory", nbt::TagType::Compound);
    for (const ItemStack& stack : player.inventory) {
        w.beginListElementCompound();
        w.writeByte("Slot", stack.slot);
        w.writeShort("id", stack.id);
        w.writeByte("Count", stack.count);
        w.writeShort("Damage", stack.damage);
        stack.preserved.writeTo(w);
        w.endCompound();
    }
    w.endList();

    player.preserved.writeTo(w);
    w.endCompound();
}

}  // namespace

bool decodeLevelDat(ConstByteSpan nbt, LevelData* out)
{
    LevelData level;

    nbt::Reader r(nbt);
    if (!r.enterRoot()) {
        return false;
    }

    bool sawData = false;
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "Data") {
            if (!nbt::expectType(r, type, nbt::TagType::Compound)) {
                return false;
            }
            if (sawData || !decodeData(r, &level)) {
                return false;
            }
            sawData = true;
        } else if (!level.preservedRoot.capture(r, name, type)) {
            return false;
        }
    }

    if (!r.ok() || !sawData) {
        return false;
    }

    *out = std::move(level);
    return true;
}

bool encodeLevelDat(const LevelData& level, std::vector<u8>* out)
{
    nbt::Writer w(*out);
    w.beginRoot();
    w.beginCompound("Data");
    w.writeLong("LastPlayed", level.lastPlayed);
    w.writeLong("SizeOnDisk", level.sizeOnDisk);
    w.writeLong("RandomSeed", level.randomSeed);
    w.writeInt("SpawnX", level.spawnX);
    w.writeInt("SpawnY", level.spawnY);
    w.writeInt("SpawnZ", level.spawnZ);
    w.writeLong("Time", level.time);
    w.writeByte("SnowCovered", level.snowCovered ? 1 : 0);
    if (level.player.present) {
        encodePlayer(w, level.player);
    }
    level.preserved.writeTo(w);
    w.endCompound();
    level.preservedRoot.writeTo(w);
    w.endRoot();

    return w.ok();
}

}  // namespace mc::alpha
