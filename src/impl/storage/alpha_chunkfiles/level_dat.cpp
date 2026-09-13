#include "impl/storage/alpha_chunkfiles/level_dat.hpp"

#include "impl/storage/alpha_chunkfiles/item_nbt.hpp"
#include "core/entity/persistence.hpp"
#include "core/entity/player_body.hpp"

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"

#include <cmath>

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

// **A player lost to a NaN is put back at the spawn point.** Riding a cart
// whose position had overflowed to NaN put the player there too, and a world
// saved like that used to open with the camera nowhere: nothing drawn, no
// collision, no marker on the map. Pos is an eye position (`posY` is
// `yOffset` above the feet), so the spawn block's feet go back up by that.
// Everything else in the compound -- the inventory above all -- is kept.
void repairPlayer(LevelData* level)
{
    PlayerData& p = level->player;
    if (!std::isfinite(p.pos[0]) || !std::isfinite(p.pos[1]) || !std::isfinite(p.pos[2])) {
        p.pos[0] = double(level->spawnX);
        p.pos[1] = double(level->spawnY) + double(entity::kEyeHeight);
        p.pos[2] = double(level->spawnZ);
        p.fallDistance = 0.0f;
    }
    if (!std::isfinite(p.fallDistance)) p.fallDistance = 0.0f;
    for (double& m : p.motion) {
        if (!std::isfinite(m)) m = 0.0;
    }
    for (float& a : p.rotation) {
        if (!std::isfinite(a)) a = 0.0f;
    }
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
        } else if (name == "3DAlphaEntities") {
            if (!nbt::expectType(r, type, nbt::TagType::Compound) || level->entities) return false;
            auto entities = std::make_shared<entity::PersistentEntities>();
            if (!entity::readPersistentEntities(r, entities.get())) return false;
            level->entities = std::move(entities);
        } else if (name == "Player") {
            if (!nbt::expectType(r, type, nbt::TagType::Compound)) return false;
            if (level->player.present || !decodePlayer(r, &level->player)) {
                return false;
            }
        } else if (!level->preserved.capture(r, name, type)) {
            return false;
        }
    }
    if (level->player.present) {
        repairPlayer(level);
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
        encodeItemStack(w, stack);
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
    if (level.entities) entity::writePersistentEntities(w, *level.entities);
    level.preserved.writeTo(w);
    w.endCompound();
    level.preservedRoot.writeTo(w);
    w.endRoot();

    return w.ok();
}

}  // namespace mc::alpha
