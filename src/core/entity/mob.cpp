// See mob.hpp. `ge.e_()`, `ge.y()`, `ge.j()`, `ge.b_()`, `ek.b_()`, `ag`'s two
// methods and the four animals' own, transcribed.
//
// The order inside `tick` is the jar's call order and it is not free to move:
// `onUpdate` runs the counters (which is where a mob dies), then
// `onLivingUpdate` asks the AI and moves, then the body's heading chases the
// direction it turns out to have travelled. Swapping the last two draws an
// animal facing where it was going *last* tick.

#include "core/entity/mob.hpp"

#include "core/audio/block_sound.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/explosion.hpp"
#include "core/entity/block_contact.hpp"
#include "core/entity/fire_entry.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"
#include "core/world/daylight.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

#include <cmath>
#include <string_view>

namespace mc::entity {
namespace {

constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;
constexpr block::BlockId kGrass = block::BlockId(mcver::Block::Grass);

constexpr double kPi = 3.141592653589793;
// The models' and the AI's own float literal, `3.1415927f` -- not `float(kPi)`,
// because `ek.b_()`'s sidestep multiplies by it and a hair either way is a
// different heading.
constexpr float kPi_f = 3.1415927f;

// `ge.y()`'s drowning and `kh.y()`'s fire, in the units they are counted in.
constexpr int kMaxAir = 300;
constexpr int kDrownAt = -20;
constexpr int kLavaFireTicks = 600;
constexpr int kLavaDamage = 10;
constexpr int kVoidDamage = 4;
constexpr double kVoidY = -64.0;

// `ge.s()F` -- getEyeHeight, `height * 0.85` for everything living in this
// version. Not the player's 1.62, which is a `yOffset` and a different thing.
constexpr double kEyeScale = 0.85;

// `cn.c(III)F` -- **getLightBrightness**, the 0..15 level put through the
// lightmap's own curve. `pathWeight` used to compare raw levels and get the
// same *order*; a monster compares the brightness against an absolute 0.5, so
// the curve has to be real. `world::lightBrightness` is that table.
float cellBrightness(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    return world::lightBrightness(world.lightValue(x, y, z));
}

// `kh.a(F)F` -- getEntityBrightness, which is **not** the cell the entity's
// feet are in: it reads two thirds of the way up the box. `yOffset` is zero for
// a mob, so the subtraction in the jar drops out.
float entityBrightness(const tick::TickWorld& world, const Mob& mob)
{
    const double probeY = mob.body.y + (mob.body.box.maxY - mob.body.box.minY) * 0.66;
    return cellBrightness(world, MathHelper::floorDouble(mob.body.x),
                          int(MathHelper::floorDouble(probeY)),
                          MathHelper::floorDouble(mob.body.z));
}

u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

// The nine rows. Every string in here is a literal in the class file, and every
// number is either a `setSize` argument or a field written in a constructor.
//
// **A null sound key is `ge`'s own default**, not a gap: `ge.c()` --
// getLivingSound -- returns null and only five of the nine override it, so a
// creeper and a slime genuinely have no idle noise. `ge.d()` and `ge.e()`
// default to `random.hurt`, and every one of the nine overrides both, which is
// why that key does not appear in this table and does not have to be preloaded
// for a mob.
constexpr MobDef kMobDefs[kMobTypeCount] = {
    // `bo` -- the sheep. **Drops nothing when it dies**: `ge.g()` answers zero
    // and `bo` does not override it. The wool comes off the first hit instead.
    {"Sheep", 0.9f, 1.3f, 10, 0, "mob.sheep", "mob.sheep", "mob.sheep", 1.0f, "Sheared",
     u16(mcver::Block::Wool), MobAi::Wander, false, 0, kMobMoveSpeed, false,
     kAnimalTalkInterval},
    // `mv` -- the pig. 0.9 square, unlike the other two, and the only one whose
    // death sound differs from its hurt sound.
    {"Pig", 0.9f, 0.9f, 10, u16(mcver::Item::RawPorkchop), "mob.pig", "mob.pig",
     "mob.pigdeath", 1.0f, "Saddle", 0, MobAi::Wander, false, 0, kMobMoveSpeed, false,
     kAnimalTalkInterval},
    // `am` -- the cow, and the only animal with a `getSoundVolume` of its own.
    {"Cow", 0.9f, 1.3f, 10, u16(mcver::Item::Leather), "mob.cow", "mob.cowhurt",
     "mob.cowhurt", 0.4f, nullptr, 0, MobAi::Wander, false, 0, kMobMoveSpeed, false,
     kAnimalTalkInterval},
    // `mz` -- the chicken: four health rather than ten, and small enough that
    // its own renderer scales the shadow to 0.3 rather than 0.7.
    {"Chicken", 0.3f, 0.4f, 4, u16(mcver::Item::Feather), "mob.chicken", "mob.chickenhurt",
     "mob.chickenhurt", 1.0f, nullptr, 0, MobAi::Wander, false, 0, kMobMoveSpeed, false,
     kAnimalTalkInterval},

    // ---- the monsters, in `k`'s order: mb, cw, dd, ax, ma ----------------
    //
    // The first three keep `kh`'s default 0.6 x 1.8 -- **none of `dq`, `mb`,
    // `cw` or `dd` calls `setSize` at all** -- and all four `dq` subclasses
    // take its twenty health.

    // `mb` -- the zombie, and the only monster that changes two things about
    // the fight: five damage rather than two, and 0.5 move speed rather than
    // 0.7, so it hits harder and closes slower.
    {"Zombie", 0.6f, 1.8f, kMonsterHealth, u16(mcver::Item::Feather), "mob.zombie",
     "mob.zombiehurt", "mob.zombiedeath", 1.0f, nullptr, 0, MobAi::Melee, true, kZombieAttack,
     0.5f, true, kMonsterTalkInterval},
    // `cw` -- the skeleton. Its hurt and death sounds are the same key, which
    // is why a skeleton you kill sounds like one you only hit.
    {"Skeleton", 0.6f, 1.8f, kMonsterHealth, u16(mcver::Item::Arrow), "mob.skeleton",
     "mob.skeletonhurt", "mob.skeletonhurt", 1.0f, nullptr, 0, MobAi::Bow, true, kMonsterAttack,
     kMobMoveSpeed, true, kMonsterTalkInterval},
    // `dd` -- the creeper. **No idle sound**: `dd` overrides `d()` and `e()`
    // and not `c()`, which is the whole of why one gets behind you.
    {"Creeper", 0.6f, 1.8f, kMonsterHealth, u16(mcver::Item::Gunpowder), nullptr, "mob.creeper",
     "mob.creeperdeath", 1.0f, nullptr, 0, MobAi::Fuse, true, kMonsterAttack, kMobMoveSpeed,
     false, kMonsterTalkInterval},
    // `ax` -- the spider: wider than it is tall, and the only mob in this
    // version whose hurt sound is its idle sound.
    {"Spider", 1.4f, 0.9f, kMonsterHealth, u16(mcver::Item::String), "mob.spider", "mob.spider",
     "mob.spiderdeath", 1.0f, nullptr, 0, MobAi::Leap, true, kMonsterAttack, 0.8f, false,
     kMonsterTalkInterval},
    // `ma` -- the slime. **Every number in this row is a placeholder for size
    // 1** and is overwritten by `setSlimeSize`: the box is `0.6 * size`, the
    // health is `size * size`, and the drop is a slimeball only at size 1.
    // `attackStrength` is the size too. It is in the table so that the row
    // exists and `mobDef` never has a hole, not because it is read.
    {"Slime", kSlimeSizeUnit, kSlimeSizeUnit, 1, u16(mcver::Item::Slimeball), nullptr,
     "mob.slime", "mob.slime", kSlimeVolume, nullptr, 0, MobAi::Hop, true, 1, 0.0f, false,
     kMonsterTalkInterval},
};

// `eo.g(F)F` -- the wrap to -180..180 that four different methods here do by
// hand with a pair of while loops.
float wrapDegrees(float value)
{
    while (value < -180.0f) {
        value += 360.0f;
    }
    while (value >= 180.0f) {
        value -= 360.0f;
    }
    return value;
}

// `ge.b(FFF)F` -- updateRotation: turn `from` towards `to` by at most `limit`.
float turnTowards(float from, float to, float limit)
{
    float delta = wrapDegrees(to - from);
    if (delta > limit) {
        delta = limit;
    }
    if (delta < -limit) {
        delta = -limit;
    }
    return from + delta;
}

}  // namespace

usize preloadMobSounds(audio::SoundEngine& engine)
{
    // Three keys a row and twenty-seven in all, of which `mob.sheep` is named
    // three times over and six more are named twice -- so the de-duplication is
    // not defensive, it is the difference between seventeen decodes and
    // twenty-seven. A linear scan over twenty-seven rows; a set would be
    // machinery for this.
    //
    // **Two of the rows have a null living sound** -- the creeper and the slime
    // -- because `ge.c()` returns null and neither overrides it. Skipping them
    // here is not a guard against a missing string; it is the reason a creeper
    // is quiet.
    std::string_view seen[3 * kMobTypeCount];
    int count = 0;
    usize loaded = 0;

    for (int row = 0; row < kMobTypeCount; ++row) {
        const MobDef& def = kMobDefs[row];
        for (const char* name : {def.livingSound, def.hurtSound, def.deathSound}) {
            if (name == nullptr) {
                continue;
            }
            const std::string_view key(name);
            bool known = false;
            for (int i = 0; i < count; ++i) {
                known = known || seen[i] == key;
            }
            if (known) {
                continue;
            }
            seen[count++] = key;
            loaded += engine.preloadSound(key);
        }
    }

    // **The egg, which is not in `MobDef` and cannot be.** `mob.chickenplop` is
    // played by `mz.j()` -- onLivingUpdate -- rather than by any of the three
    // `getSound` methods, so it is the chicken's and still has no column. One
    // literal is the honest way to say that.
    loaded += engine.preloadSound("mob.chickenplop");

    // **The other two a monster plays that no `getSound` method names**, for
    // the same reason: `cw.a(Lkh;F)V` plays `random.bow` when it looses an
    // arrow and `dd.a(Lkh;F)V` plays `random.fuse` when it lights. Both are in
    // `audio::preloadEffects` already -- the bow because the player has one and
    // the fuse because the explosion needs it -- and naming them here would be
    // a second version of the truth. `random.explode` is the blast itself and
    // belongs to `core/entity/explosion.cpp`, not to a mob.
    return loaded;
}

const MobDef& mobDef(MobType type)
{
    const int index = int(type);
    return kMobDefs[index >= 0 && index < kMobTypeCount ? index : 0];
}

Mob* MobSystem::spawnFromServer(const tick::TickWorld& world, i32 entityId, MobType type,
                                double x, double y, double z, float yaw, float pitch,
                                int slimeSize)
{
    removeById(entityId);
    if (!spawn(world, type, x, y, z, yaw)) {
        return nullptr;
    }
    Mob& mob = mobs_[mobs_.size() - 1];
    mob.entityId = entityId;
    mob.remote = true;
    mob.pitch = pitch;
    mob.prevPitch = pitch;
    // **The size the server named, if it named one.** `spawn` has already drawn
    // one out of this pool's generator, as `ma`'s constructor does, and the
    // draw is kept whether or not it is used -- the stream downstream of it is
    // the same either way. `setSlimeSize` is a resize and not a field write:
    // the box, the health and the placement all follow.
    if (type == MobType::Slime && slimeSize > 0) {
        setSlimeSize(mob, slimeSize);
        mob.body.setFeet(x, y, z);
    }
    mob.body.snapRenderPosition();
    // `gy.a(ez)` seeds `bd/be/bf` from the spawn packet and *then* places the
    // body there, so the first relative move that arrives is measured against
    // the spawn point and not against zero. Nothing is owed yet, so the clock
    // is at rest.
    mob.serverX = x;
    mob.serverY = y;
    mob.serverZ = z;
    mob.serverYaw = double(yaw);
    mob.serverPitch = double(pitch);
    mob.smoothTicks = 0;
    return &mob;
}

Mob* MobSystem::findById(i32 entityId)
{
    if (entityId == 0) {
        return nullptr;
    }
    for (int i = 0; i < mobs_.size(); ++i) {
        if (mobs_[i].entityId == entityId) {
            return &mobs_[i];
        }
    }
    return nullptr;
}

bool MobSystem::removeById(i32 entityId)
{
    if (entityId == 0) {
        return false;
    }
    for (int i = 0; i < mobs_.size(); ++i) {
        if (mobs_[i].entityId == entityId) {
            removeAt(i);
            return true;
        }
    }
    return false;
}

bool MobSystem::placeById(i32 entityId, double x, double y, double z, bool hasLook, float yaw,
                          float pitch)
{
    Mob* mob = findById(entityId);
    if (mob == nullptr) {
        return false;
    }

    // **Nothing moves here.** `setPositionAndRotation2` writes the target and
    // the clock and returns; the walk is `ge.j()`'s, in `tick` below. Writing
    // the body straight to the packet's position -- which is what this used to
    // do -- is what made a remote animal step twenty times a second: `prev`
    // and the live position were equal on every frame of the gap between two
    // packets, so there was nothing for the frame interpolation to interpolate.
    mob->serverX = x;
    mob->serverY = y;
    mob->serverZ = z;
    if (hasLook) {
        mob->serverYaw = double(yaw);
        mob->serverPitch = double(pitch);
    }
    mob->smoothTicks = i16(kServerSmoothTicks);
    return true;
}

bool MobSystem::turnById(i32 entityId, float yaw, float pitch)
{
    Mob* mob = findById(entityId);
    if (mob == nullptr) {
        return false;
    }
    // `gy.a(ju)` -- an Entity Look is `setPositionAndRotation2` with the
    // entity's own position, so the head turns over three ticks and the body
    // stands where it is.
    mob->serverYaw = double(yaw);
    mob->serverPitch = double(pitch);
    mob->smoothTicks = i16(kServerSmoothTicks);
    return true;
}

bool MobSystem::spawn(const tick::TickWorld& world, MobType type, double x, double y, double z,
                      float yaw)
{
    const MobDef& def = mobDef(type);

    Mob mob{};
    mob.alive = true;
    mob.type = type;
    mob.health = i16(def.health);
    mob.air = i16(kMaxAir);
    mob.yaw = yaw;
    mob.prevYaw = yaw;
    mob.renderYaw = yaw;
    mob.prevRenderYaw = yaw;

    // `setSize`, then `setPosition`. A mob's `yOffset` is zero -- only
    // `EntityPlayer` sets that field -- so the position it is given is its feet.
    mob.body.setSize(def.width, def.height, 0.0f);
    mob.body.setFeet(x, y, z);

    // `mz`'s constructor: the first egg is between five and ten minutes away.
    if (type == MobType::Chicken) {
        mob.eggTime = rand_.nextInt(kEggTimeSpread) + kEggTimeBase;
        mob.wingSpeed = 1.0f;
    }

    // `ma`'s constructor, and the draw order is its own: **the size is drawn
    // before the first hop delay**, and both come out of the entity's random
    // in the jar. `1 << rand(3)` is 1, 2 or 4 -- a slime is never size 3.
    if (type == MobType::Slime) {
        mob.hopDelay = 0;
        setSlimeSize(mob, 1 << rand_.nextInt(3));
        mob.hopDelay = i16(rand_.nextInt(kSlimeHopSpread) + kSlimeHopBase);
        mob.body.setFeet(x, y, z);
    }

    mob.light = packedLightAt(world, x, y + double(mob.body.height) * 0.5, z);

    Mob* slot = mobs_.push();
    if (slot == nullptr) {
        ++refused_;
        return false;
    }
    // **Issued last, so a refused spawn does not burn one.** The number only
    // has to be unique, not dense, but a counter that moved on a failure would
    // make the handle depend on how full the pool was -- and this is compared
    // against a saved-off value in the arrow sweep.
    mob.handle = nextHandle_++;
    *slot = mob;
    return true;
}

int MobSystem::animalCount() const
{
    int total = 0;
    for (int i = 0; i < mobs_.size(); ++i) {
        if (mobs_[i].alive && !mobDef(mobs_[i].type).hostile) {
            ++total;
        }
    }
    return total;
}

int MobSystem::monsterCount() const
{
    // `cn.b(Ljava/lang/Class;)I` with `co.class` -- an **interface**, so the
    // slime is counted even though it is an `ge` rather than an `ek`. That is
    // the whole reason `hostile` is a column and not `type >= Zombie`.
    int total = 0;
    for (int i = 0; i < mobs_.size(); ++i) {
        if (mobs_[i].alive && mobDef(mobs_[i].type).hostile) {
            ++total;
        }
    }
    return total;
}

void MobSystem::setSlimeSize(Mob& mob, int size) const
{
    // `ma.c(I)V`. Three writes and a re-place, and the re-place matters: the
    // box is rebuilt around the position, so a slime that splits or is loaded
    // from a save has a box that matches its size rather than its parent's.
    mob.slimeSize = u8(size < 1 ? 1 : size);
    const float edge = kSlimeSizeUnit * float(mob.slimeSize);
    mob.body.setSize(edge, edge, 0.0f);
    mob.health = i16(int(mob.slimeSize) * int(mob.slimeSize));
    mob.prevHealth = mob.health;
    mob.body.setFeet(mob.body.x, mob.body.y, mob.body.z);
}

void MobSystem::removeAt(int index)
{
    const int last = mobs_.size() - 1;
    if (ridden_ == index) {
        ridden_ = -1;
    } else if (ridden_ == last) {
        // The last entry is about to be swapped into the hole.
        ridden_ = index;
    }
    // **The jockey's index follows the swap.** A skeleton whose spider was
    // killed is dismounted -- `kh.e_()` clears `ridingEntity` when the mount is
    // dead -- and one whose spider was the entry being moved is repointed.
    for (int i = 0; i < mobs_.size(); ++i) {
        if (mobs_[i].mountIndex == index) {
            mobs_[i].mountIndex = -1;
        } else if (mobs_[i].mountIndex == last) {
            mobs_[i].mountIndex = i16(index);
        }
    }
    mobs_.swapRemove(index);
}

bool MobSystem::mountOn(int riderIndex, int mountIdx)
{
    if (riderIndex < 0 || riderIndex >= mobs_.size() || mountIdx < 0
        || mountIdx >= mobs_.size() || riderIndex == mountIdx) {
        return false;
    }
    mobs_[riderIndex].mountIndex = i16(mountIdx);
    return true;
}

RiderSeat MobSystem::seatOf(int index) const
{
    RiderSeat s;
    if (index < 0 || index >= mobs_.size() || !mobs_[index].alive) {
        return s;
    }
    const Mob& mob = mobs_[index];
    s.valid = true;
    s.x = mob.body.x;
    s.y = mob.body.y + double(mob.body.height) * 0.75
          - (mob.type == MobType::Spider ? kSpiderSeatDrop : 0.0);
    s.z = mob.body.z;
    s.yaw = mob.yaw;
    return s;
}

RiderSeat MobSystem::seat() const
{
    // `kh.v()` -- getMountedYOffset, `height * 0.75`, not overridden by any
    // animal. The rider's own `yOffset` goes on top of this and is the rider's
    // business; see core/entity/rider.hpp. **The spider overrides it**:
    // `ax.h()` is `height * 0.75 - 0.5`, which is where the spawner's
    // one-in-a-hundred skeleton sits.
    return seatOf(ridden_);
}

bool MobSystem::mount(int index)
{
    if (index < 0 || index >= mobs_.size() || mobs_[index].ridden || !mobs_[index].flag
        || mobs_[index].type != MobType::Pig) {
        return false;
    }
    if (ridden_ >= 0 && ridden_ < mobs_.size()) {
        mobs_[ridden_].ridden = false;
    }
    ridden_ = index;
    mobs_[index].ridden = true;
    return true;
}

RiderSeat MobSystem::dismount()
{
    RiderSeat off;
    if (ridden_ >= 0 && ridden_ < mobs_.size()) {
        Mob& mob = mobs_[ridden_];
        mob.ridden = false;
        // `mountEntity`'s tail, which is the same method for a pig as for a
        // boat -- the rider is put on top of what it was riding. Nothing holds
        // it there, because a mob is not solid: it lands on the pig's back and
        // falls off it, which is what getting off a pig looks like. See
        // core/entity/rider.hpp.
        off.valid = true;
        off.x = mob.body.x;
        off.y = mob.body.box.minY + double(mob.body.height);
        off.z = mob.body.z;
        off.yaw = mob.yaw;
    }
    ridden_ = -1;
    return off;
}

void MobSystem::dropOnDeath(const tick::TickWorld& world, const Mob& mob, bool fromSkeleton)
{
    // `ge.b(Lkh;)V`: `int i = getDropItemId(); if (i > 0) { int j = rand(3);
    // for (k = 0; k < j; k++) dropItem(i, 1); }` -- so **nought to two**, and
    // an empty-handed kill really does leave nothing a third of the time.
    const MobDef& def = mobDef(mob.type);
    const double midY = mob.body.y + double(mob.body.height) * 0.5;

    // `ma.g()I` -- **a slime drops a slimeball only at size 1**, so a big one
    // is worth nothing until it has been cut down twice.
    u16 item = def.dropItem;
    if (mob.type == MobType::Slime && mob.slimeSize != 1) {
        item = 0;
    }

    if (item != 0) {
        const int count = rand_.nextInt(3);
        for (int n = 0; n < count; ++n) {
            world.spawnItem(mob.body.x, midY, mob.body.z, item, 1);
        }
    }

    // **`dd.b(Lkh;)V` -- the one Easter egg in this version, and the only way
    // to get a music disc in it.** A creeper killed by a *skeleton* drops
    // `Item.record13.shiftedIndex + rand(2)`, which is one of the two records
    // a1.1.2 has. Nothing crafts them, no chest generates them and no other
    // entity drops them, so a player who wants one has to get a skeleton to
    // shoot a creeper. `instanceof cw` in the jar; the arrow carries who fired
    // it here -- see core/entity/arrow.hpp.
    if (mob.type == MobType::Creeper && fromSkeleton) {
        world.spawnItem(mob.body.x, midY, mob.body.z,
                        u16(kRecordItemBase + rand_.nextInt(2)), 1);
    }
}

bool MobSystem::attack(tick::TickWorld& world, int index, int amount, bool fromPlayer,
                       double fromX, double fromZ, bool knockback, bool fromSkeleton,
                       bool provokes)
{
    if (index < 0 || index >= mobs_.size() || !mobs_[index].alive) {
        return false;
    }
    Mob& mob = mobs_[index];
    if (mob.health <= 0) {
        return false;
    }

    const MobDef& def = mobDef(mob.type);

    // `bo.a(Lkh;I)Z` -- **the sheep's fleece, and it comes off before the
    // damage is applied.** One to three wool, each thrown with the original's
    // own scatter, and only when an `EntityLiving` did the hitting: a sheep
    // that suffocates keeps its coat.
    if (def.shearDrop != 0 && !mob.flag && fromPlayer) {
        mob.flag = true;
        const int wool = 1 + rand_.nextInt(3);
        for (int n = 0; n < wool; ++n) {
            world.spawnItem(mob.body.x, mob.body.y + double(mob.body.height), mob.body.z,
                            def.shearDrop, 1);
        }
    }

    // `ge.a(Lkh;I)Z`. Being hit resets the despawn clock, which is why an
    // animal a player is fighting does not vanish mid-fight.
    mob.entityAge = 0;
    mob.limbYaw = 1.5f;

    // **Inside the invulnerability window only a bigger hit lands, and only
    // its difference.** `prevHealth` is what the last hit left, so two hits of
    // one inside ten ticks do one damage between them and a sword hit after a
    // punch does the difference. Everything else -- the hurt animation, the
    // knockback and the resistance timer -- is skipped on that path.
    if (double(mob.hurtResistant) > double(kHurtResistantTime) / 2.0) {
        if (mob.prevHealth - amount >= mob.health) {
            return false;
        }
        mob.health = i16(mob.prevHealth - amount);
    } else {
        mob.prevHealth = mob.health;
        mob.hurtResistant = i16(kHurtResistantTime);
        mob.health = i16(mob.health - amount);
        mob.hurtTime = i16(kHurtTime);
        mob.maxHurtTime = i16(kHurtTime);
    }

    if (knockback && mob.health > 0) {
        // `ge.a(Lkh;IDD)V` -- knockBack: halve what is there, push a fixed 0.4
        // along the normalised horizontal separation, and lift by 0.4 with a
        // ceiling at the same number.
        //
        // **The vector points from the mob to its attacker and is then
        // subtracted**, which is the way round `attackEntityFrom` passes it:
        // `knockBack(entity, i, entity.posX - posX, entity.posZ - posZ)`. Built
        // the other way round a punched cow walks into the fist.
        double dx = fromX - mob.body.x;
        double dz = fromZ - mob.body.z;
        double length = std::sqrt(dx * dx + dz * dz);
        if (length < 0.0001) {
            dx = 0.0;
            dz = 1.0;
            length = 1.0;
        }
        const double push = 0.4;
        mob.body.motionX /= 2.0;
        mob.body.motionY /= 2.0;
        mob.body.motionZ /= 2.0;
        mob.body.motionX -= dx / length * push;
        mob.body.motionY += 0.4000000059604645;
        mob.body.motionZ -= dz / length * push;
        if (mob.body.motionY > 0.4000000059604645) {
            mob.body.motionY = 0.4000000059604645;
        }
        mob.body.onGround = false;
    }

    // The sound is played on both paths above, death and hurt alike -- the
    // only silent hit is the one that returned false.
    {
        const char* key = mob.health <= 0 ? def.deathSound : def.hurtSound;
        const float pitch = (rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f;
        world.playSoundAt(key, mob.body.x, mob.body.y + double(mob.body.height) * 0.5,
                          mob.body.z, def.soundVolume, pitch);
    }

    // `dq.a(Lkh;I)Z` -- **a monster that is hit turns on whoever hit it**,
    // whatever it was doing and however far away. It is what makes a skeleton
    // shot from a distance start walking, and it is the only thing besides
    // `findPlayerToAttack` that ever writes `entityToAttack`. The jar excludes
    // the mount, the rider and the mob itself; none of those can be the player
    // here, so the test is the source alone.
    //
    // **`provokes` is ours and is the Creative exception** -- see
    // `MobPlayer::targetable`, which refuses the same player on the other side
    // of the search. Without it a monster hit by a Creative fist would hold a
    // target `findPlayerToAttack` will never hand it again, and would follow
    // somebody it cannot touch for the rest of its life.
    if (def.hostile && fromPlayer && provokes && mob.health > 0) {
        mob.targetingPlayer = true;
    }

    if (mob.health <= 0) {
        // `onDeath`. The corpse lies there for twenty ticks -- `deathTime` in
        // `updateCounters` -- and the drops happen now, not then.
        dropOnDeath(world, mob, fromSkeleton);
        if (ridden_ == index) {
            dismount();
        }
    }
    return true;
}

MobSystem::Interaction MobSystem::interact(tick::TickWorld& world, int index, u16 held)
{
    // **The world is not read by any of the three answers below** -- milk, the
    // saddle and mounting are all pure state changes. It stays in the signature
    // because this is `kh.a(Ldm;)Z`'s seam, every caller has a world in hand,
    // and the first thing added here that is not in this version (shears, a
    // feed) would want it back.
    (void)world;
    Interaction result;
    result.becomes = held;
    if (index < 0 || index >= mobs_.size() || !mobs_[index].alive) {
        return result;
    }
    Mob& mob = mobs_[index];
    if (mob.health <= 0) {
        return result;
    }

    // `am.a(Ldm;)Z` -- **the cow's bucket**, and it checks the *current* item
    // rather than searching the inventory. The bucket is replaced in place,
    // which is why this hands one back rather than spawning anything.
    if (mob.type == MobType::Cow && held == u16(mcver::Item::Bucket)) {
        result.taken = true;
        result.becomes = u16(mcver::Item::MilkBucket);
        return result;
    }

    // `mv.a(Ldm;)Z` -- a saddled pig is mounted, and interact is asked before
    // the held item is: a player holding a saddle who clicks a saddled pig
    // climbs on rather than wasting the saddle.
    if (mob.type == MobType::Pig && mob.flag) {
        result.taken = mount(index);
        result.mounted = result.taken;
        return result;
    }

    // `jw.b(Lev;Lge;)V` -- ItemSaddle, which is not `interact` at all: it is
    // `ItemStack.useItemOnEntity`, run by `EntityPlayer.useCurrentItemOnEntity`
    // only after `interact` has refused. It spends the saddle.
    if (mob.type == MobType::Pig && !mob.flag && held == u16(mcver::Item::Saddle)) {
        mob.flag = true;
        result.taken = true;
        result.becomes = 0;
        return result;
    }

    // **Nothing else answers a right click.** There is no wheat use beyond
    // bread in this version and no shears, so a sheep, a chicken and an
    // unsaddled pig all refuse and the click falls through to the block behind.
    return result;
}

float MobSystem::pathWeight(const tick::TickWorld& world, MobType type, i32 x, int y,
                            i32 z) const
{
    // `dq.a(III)F`: **`0.5 - getLightBrightness`**, which is `ag`'s second line
    // with the sign the other way round. It is the whole of "monsters keep to
    // the dark", and it is also why `ek.a()`'s `getBlockPathWeight >= 0` gate
    // refuses a monster spawn anywhere brighter than half.
    if (mobDef(type).hostile) {
        return kDarkPathWeight - cellBrightness(world, x, y, z);
    }

    // `ag.a(III)F`: grass **under** the cell is worth ten outright, and
    // everything else is worth `getBrightness - 0.5`, which is what makes an
    // animal drift towards open ground and away from caves.
    if (world.blockAt(x, y - 1, z) == kGrass) {
        return kGrassPathWeight;
    }
    return cellBrightness(world, x, y, z) - 0.5f;
}

bool MobSystem::canSee(const tick::TickWorld& world, const Mob& mob, double x, double y,
                       double z) const
{
    // `ge.c(Lkh;)Z` -- `rayTraceBlocks(eye, theirEye) == null`. The port's ray
    // takes a direction and a length rather than two points, which is the same
    // segment written differently.
    const double eyeY = mob.body.y + double(mob.body.height) * kEyeScale;
    const double dx = x - mob.body.x;
    const double dy = y - eyeY;
    const double dz = z - mob.body.z;
    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 1e-9) {
        return true;
    }
    return !rayTrace(world, mob.body.x, eyeY, mob.body.z, dx / length, dy / length,
                     dz / length, length)
                .hit;
}

bool MobSystem::findTarget(const tick::TickWorld& world, Mob& mob,
                           const MobSurroundings& around)
{
    if (!around.player.present || !around.player.targetable) {
        return false;
    }
    // **`ax.i()` refuses in daylight.** A spider only looks for somebody when
    // its own cell is darker than half, which is why one caught in the open at
    // dawn stops hunting -- it does not burn, it simply loses interest.
    if (mobDef(mob.type).ai == MobAi::Leap
        && entityBrightness(world, mob) >= kSunBurnBrightness) {
        return false;
    }

    // `cn.a(Lkh;D)Ldm;` -- the closest player inside sixteen blocks, measured
    // from the mob's *position*, and then `canEntityBeSeen`. **The spider skips
    // the sight test**: `ax.i()` returns the player without asking, so one on
    // the far side of a wall still starts walking.
    const double dx = around.player.x - mob.body.x;
    const double dy = around.player.y - mob.body.y;
    const double dz = around.player.z - mob.body.z;
    if (dx * dx + dy * dy + dz * dz >= kTargetRange * kTargetRange) {
        return false;
    }
    if (mobDef(mob.type).ai == MobAi::Leap) {
        return true;
    }
    return canSee(world, mob, around.player.x, around.player.y, around.player.z);
}

void MobSystem::faceTowards(Mob& mob, double x, double y, double z, float limit) const
{
    const double dx = x - mob.body.x;
    const double dz = z - mob.body.z;
    // `ge.b(Lkh;F)V` measures from eye to eye -- `getEyeHeight` is `height *
    // 0.85` for everything living in this version.
    const double eye = double(mobDef(mob.type).height) * 0.85;
    const double dy = y - (mob.body.y + eye);
    const double flat = std::sqrt(dx * dx + dz * dz);
    const float wantYaw = float(std::atan2(dz, dx) * 180.0 / kPi) - 90.0f;
    const float wantPitch = float(-(std::atan2(dy, flat) * 180.0 / kPi));
    mob.pitch = turnTowards(mob.pitch, wantPitch, limit);
    mob.yaw = turnTowards(mob.yaw, wantYaw, limit);
}

bool MobSystem::wanderAimlessly(const tick::TickWorld& world, Mob& mob,
                                const MobSurroundings& around)
{
    // `ge.b_()`. The age is counted here and nowhere else, so a mob that is
    // following a path ages only on the ticks its path is refused -- which is
    // the original's, and is why a busy animal outlives an idle one.
    ++mob.entityAge;

    if (around.player.present) {
        const double dx = around.player.x - mob.body.x;
        const double dy = around.player.y - mob.body.y;
        const double dz = around.player.z - mob.body.z;
        const double distSq = dx * dx + dy * dy + dz * dz;

        // **Despawning, and every animal is subject to it.** Beta 1.8 is where
        // that stopped; in this version a herd left alone thins itself out and
        // there is nothing a player can do to a particular animal to keep it.
        if (distSq > kDespawnFar) {
            return false;
        }
        if (mob.entityAge > kDespawnAge && rand_.nextInt(kDespawnOdds) == 0) {
            if (distSq < kDespawnNear) {
                mob.entityAge = 0;
            } else {
                return false;
            }
        }
    }

    mob.moveStrafing = 0.0f;
    mob.moveForward = 0.0f;

    if (rand_.nextFloat() < 0.02f) {
        if (around.player.present) {
            const double dx = around.player.x - mob.body.x;
            const double dy = around.player.y - mob.body.y;
            const double dz = around.player.z - mob.body.z;
            const double distSq = dx * dx + dy * dy + dz * dz;
            if (distSq < double(kLookRange) * double(kLookRange)) {
                mob.watchingPlayer = true;
                mob.lookTicks = i16(kLookTicksBase + rand_.nextInt(kLookTicksSpread));
            } else {
                mob.randomYaw = (rand_.nextFloat() - 0.5f) * 20.0f;
                mob.watchingPlayer = false;
            }
        } else {
            mob.randomYaw = (rand_.nextFloat() - 0.5f) * 20.0f;
        }
    }

    if (mob.watchingPlayer && around.player.present) {
        faceTowards(mob, around.player.x, around.player.y, around.player.z, 10.0f);
        --mob.lookTicks;
        const double dx = around.player.x - mob.body.x;
        const double dy = around.player.y - mob.body.y;
        const double dz = around.player.z - mob.body.z;
        const double distSq = dx * dx + dy * dy + dz * dz;
        if (mob.lookTicks <= 0 || distSq > double(kLookRange) * double(kLookRange)) {
            mob.watchingPlayer = false;
        }
    } else {
        mob.watchingPlayer = false;
        if (rand_.nextFloat() < 0.05f) {
            mob.randomYaw = (rand_.nextFloat() - 0.5f) * 20.0f;
        }
        mob.yaw += mob.randomYaw;
        // `defaultPitch`, which is zero for everything in this version.
        mob.pitch = 0.0f;
    }

    const bool inWater = mob.body.box.minY < mob.body.box.maxY
                         && block::isMaterialInBox(world, mob.body.box, kWaterMaterial);
    const bool inLava = block::isMaterialInBox(world, mob.body.box, kLavaMaterial);
    if (inWater || inLava) {
        mob.jumping = rand_.nextFloat() < 0.8f;
    }
    return true;
}

bool MobSystem::updateActionState(tick::TickWorld& world, int index,
                                  const MobSurroundings& around)
{
    const MobDef& def = mobDef(mobs_[index].type);

    // **`ma.b_()` is not `ek.b_()`.** The slime overrides `EntityLiving`'s
    // method directly and never sees the creature AI: no path, no
    // `getBlockPathWeight`, no despawn -- see `hopAbout`.
    if (def.ai == MobAi::Hop) {
        hopAbout(world, mobs_[index], around);
        return true;
    }

    // `dd.b_()` -- **the creeper's wrapper around `super.b_()`**, and the three
    // states it juggles are the whole of why a creeper that loses sight of you
    // un-hisses instead of going off:
    //
    //   * the fuse **runs backwards** on every tick the creeper is not lit, so
    //     backing away from one at 29 ticks of fuse is survivable;
    //   * `creeperState` is set to 2 before the AI runs and reset to -1 after
    //     it unless the AI set it to exactly 1 -- which is a one-tick flag
    //     written as a three-state, and `attackTarget` reads the *previous*
    //     tick's value through it.
    if (def.ai == MobAi::Fuse) {
        Mob& creeper = mobs_[index];
        creeper.prevFuse = creeper.fuse;
        if (creeper.fuse > 0 && creeper.creeperState < 0) {
            --creeper.fuse;
        }
        if (creeper.creeperState >= 0) {
            creeper.creeperState = 2;
        }
    }

    const bool stays = updateCreatureActionState(world, index, around);

    if (def.ai == MobAi::Fuse && index < mobs_.size() && mobs_[index].alive
        && mobs_[index].creeperState != 1) {
        mobs_[index].creeperState = -1;
    }
    return stays;
}

bool MobSystem::updateCreatureActionState(tick::TickWorld& world, int index,
                                          const MobSurroundings& around)
{
    Mob& mob = mobs_[index];
    const MobDef& def = mobDef(mob.type);

    // `ek.b_()`. **The first half is the monsters' and was dead for the
    // animals**: `ag` does not override `findPlayerToAttack`, so
    // `entityToAttack` was never set and everything up to the wander was
    // unreachable. It is reachable now, and `hostile` is what gates it.
    mob.hasAttacked = false;

    if (def.hostile) {
        if (!mob.targetingPlayer) {
            mob.targetingPlayer = findTarget(world, mob, around);
            if (mob.targetingPlayer) {
                ++searchesThisTick_;
                PathRoute route;
                if (paths_.find(world, mob.body.box, mob.body.width, mob.body.height,
                                around.player.x, around.player.box.minY, around.player.z,
                                kTargetPathRange, &route)) {
                    mob.path = route;
                } else {
                    mob.path.clear();
                }
            }
        } else if (around.player.present && around.player.targetable) {
            // `getDistanceToEntity` -- between positions, not between boxes,
            // and **`eo.c` is `sqrt` through a float**, which is why a mob at
            // exactly 2.5 blocks is a coin toss rather than a rule.
            const double dx = mob.body.x - around.player.x;
            const double dy = mob.body.y - around.player.y;
            const double dz = mob.body.z - around.player.z;
            const float distance =
                MathHelper::sqrtDouble(dx * dx + dy * dy + dz * dz);
            if (canSee(world, mob, around.player.x, around.player.y, around.player.z)) {
                attackTarget(world, index, around, distance);
                if (!mobs_[index].alive) {
                    // A creeper that went off. `F()` in the jar, and it is not
                    // a death: no drops, no death sound, no twenty ticks.
                    return false;
                }
            }
        } else {
            // `entityToAttack = null` when the target is gone, which in the jar
            // is a dead or unloaded player. **Switching to Creative arrives
            // here too**, so a zombie already chasing somebody drops the chase
            // on the tick the mode changes rather than finishing its walk.
            mob.targetingPlayer = false;
        }
    }

    if (mob.path.empty() || mob.path.finished()) {
        mob.path.clear();
    }

    // **The two branches are exclusive**, which is what stops a mob that is
    // chasing somebody from also wandering: a monster with a target re-asks for
    // the path one tick in twenty and never draws the ten wander cells at all.
    const bool chasing = !mob.hasAttacked && mob.targetingPlayer && around.player.present;
    if (chasing && (mob.path.empty() || rand_.nextInt(kRetargetOdds) == 0)) {
        {
            ++searchesThisTick_;
            PathRoute route;
            if (paths_.find(world, mob.body.box, mob.body.width, mob.body.height,
                            around.player.x, around.player.box.minY, around.player.z,
                            kTargetPathRange, &route)) {
                mob.path = route;
            } else {
                mob.path.clear();
            }
        }
    } else if (!chasing) {
        const bool wantsPath = (mob.path.empty() && rand_.nextInt(kWanderOdds) == 0)
                               || rand_.nextInt(kWanderOdds) == 0;
        if (wantsPath) {
            // Ten candidate cells within six blocks, scored, best wins. The
            // draws happen whether or not a path is asked for afterwards, which
            // keeps the random stream the same shape as the original's.
            bool found = false;
            i32 bestX = 0;
            int bestY = 0;
            i32 bestZ = 0;
            float bestWeight = -99999.0f;
            for (int n = 0; n < kWanderSamples; ++n) {
                const i32 cx = MathHelper::floorDouble(mob.body.x
                                                       + double(rand_.nextInt(kWanderSpanXZ)) - 6.0);
                const int cy = int(MathHelper::floorDouble(mob.body.y
                                                          + double(rand_.nextInt(kWanderSpanY)) - 3.0));
                const i32 cz = MathHelper::floorDouble(mob.body.z
                                                       + double(rand_.nextInt(kWanderSpanXZ)) - 6.0);
                const float weight = pathWeight(world, mob.type, cx, cy, cz);
                if (weight > bestWeight) {
                    bestWeight = weight;
                    bestX = cx;
                    bestY = cy;
                    bestZ = cz;
                    found = true;
                }
            }
            if (found) {
                ++searchesThisTick_;
                PathRoute route;
                if (paths_.find(world, mob.body.box, mob.body.width, mob.body.height,
                                double(bestX) + 0.5, double(bestY) + 0.5, double(bestZ) + 0.5,
                                kPathSearchRange, &route)) {
                    mob.path = route;
                } else {
                    mob.path.clear();
                }
            }
        }
    }

    const int groundY = int(MathHelper::floorDouble(mob.body.box.minY + 0.5));
    mob.pitch = 0.0f;

    if (mob.path.empty() || rand_.nextInt(kRepathOdds) == 0) {
        // **A despawn is not a death**: `setEntityDead` removes the entity
        // where it stands, with no drops, no sound and no twenty ticks of lying
        // there. That is what `false` here means.
        const bool stays = wanderAimlessly(world, mob, around);
        mob.path.clear();
        return stays;
    }

    // Follow the route. Nodes nearer than twice the mob's width are stepped
    // over, which is what stops a cow shuffling on the spot at every corner.
    const double reach = double(mob.body.width) * 2.0;
    double nodeX = 0.0;
    double nodeY = 0.0;
    double nodeZ = 0.0;
    bool haveNode = !mob.path.finished();
    if (haveNode) {
        mob.path.position(mob.body.width, &nodeX, &nodeY, &nodeZ);
    }
    while (haveNode) {
        const double dx = nodeX - mob.body.x;
        const double dz = nodeZ - mob.body.z;
        if (dx * dx + dz * dz >= reach * reach) {
            break;
        }
        mob.path.advance();
        if (mob.path.finished()) {
            haveNode = false;
            mob.path.clear();
            break;
        }
        mob.path.position(mob.body.width, &nodeX, &nodeY, &nodeZ);
    }

    mob.jumping = false;
    if (haveNode) {
        const double dx = nodeX - mob.body.x;
        const double dz = nodeZ - mob.body.z;
        const double dy = nodeY - double(groundY);
        const float wantYaw = float(std::atan2(dz, dx) * 180.0 / kPi) - 90.0f;
        mob.yaw = turnTowards(mob.yaw, wantYaw, kTurnLimit);

        // `this.moveForward = this.moveSpeed` -- 0.7 for most, 0.5 for a zombie
        // and 0.8 for a spider, flat. There is no walk/run distinction here.
        mob.moveForward = def.moveSpeed;

        // **`hasAttacked` turns the walk into a sidestep.** A skeleton that has
        // just fired points its body at you and converts the path's forward
        // into strafe plus forward about that heading, which is why one circles
        // rather than marching in. `ek.b_()`'s `if (this.g && this.f != null)`.
        if (mob.hasAttacked && mob.targetingPlayer && around.player.present) {
            const double tx = around.player.x - mob.body.x;
            const double tz = around.player.z - mob.body.z;
            const float wasYaw = mob.yaw;
            mob.yaw = float(std::atan2(tz, tx) * 180.0 / kPi) - 90.0f;
            const float turned = (wasYaw - mob.yaw + 90.0f) * kPi_f / 180.0f;
            mob.moveStrafing = -MathHelper::sin(turned) * mob.moveForward;
            mob.moveForward = MathHelper::cos(turned) * mob.moveForward;
        }

        if (dy > 0.0) {
            mob.jumping = true;
        }
    }

    // `if (this.f != null) b(this.f, 30.0F);` -- **faceEntity, and it is after
    // the path turn**, so a monster following a corridor still looks at you.
    if (mob.targetingPlayer && around.player.present) {
        faceTowards(mob, around.player.x, around.player.y, around.player.z, kTurnLimit);
    }

    // A wall is jumped at, which is how an animal gets over the fence it has
    // walked into, and a swimming one bobs.
    if (mob.body.collidedHorizontally) {
        mob.jumping = true;
    }
    const bool inWater = block::isMaterialInBox(world, mob.body.box, kWaterMaterial);
    const bool inLava = block::isMaterialInBox(world, mob.body.box, kLavaMaterial);
    if (rand_.nextFloat() < 0.8f && (inWater || inLava)) {
        mob.jumping = true;
    }
    return true;
}

void MobSystem::attackTarget(tick::TickWorld& world, int index, const MobSurroundings& around,
                             float distance)
{
    Mob& mob = mobs_[index];
    const MobDef& def = mobDef(mob.type);
    const MobPlayer& them = around.player;

    switch (def.ai) {
    case MobAi::Bow: {
        // `cw.a(Lkh;F)V`. **The skeleton does not close**: it fires from
        // wherever it is inside ten blocks and its body stops following the
        // path, which is what `hasAttacked` does below.
        if (distance >= kBowRange) {
            break;
        }
        const double dx = them.x - mob.body.x;
        const double dz = them.z - mob.body.z;
        if (mob.attackTime == 0) {
            // `new kg(world, this)` -- the arrow starts at the shooter, is
            // stepped a sixth of a block out along its facing and dropped a
            // tenth, and *then* the skeleton lifts it 1.4. `eo.b` is cos and
            // `eo.a` is sin; getting them the wrong way round fires sideways.
            const float yawRadians = mob.yaw / 180.0f * kPi_f;
            const double startX = mob.body.x - double(MathHelper::cos(yawRadians)) * 0.16;
            const double startY = mob.body.y - 0.10000000149011612 + kBowArrowLift;
            const double startZ = mob.body.z - double(MathHelper::sin(yawRadians)) * 0.16;

            // **The player's `posY` is their eye**, which is what `them.y`
            // carries, so this aims a fifth of a block below the eye and adds a
            // lift proportional to the flat distance -- the arc that makes a
            // skeleton at range shoot over your head and hit.
            const double dy = (them.y - kBowTargetDrop) - startY;
            const float arc = MathHelper::sqrtDouble(dx * dx + dz * dz) * kBowArc;

            world.playSoundAt("random.bow", mob.body.x, mob.body.y, mob.body.z, 1.0f,
                              1.0f / (rand_.nextFloat() * 0.4f + 0.8f));
            if (around.shootArrow != nullptr) {
                around.shootArrow(around.shootArrowCtx, startX, startY, startZ, dx,
                                  dy + double(arc), dz, kBowVelocity, kBowInaccuracy,
                                  mob.handle);
            }
            mob.attackTime = i16(kBowCooldown);
        }
        // The yaw is **set, not turned towards**: a skeleton snaps to face you
        // the instant it is in range.
        mob.yaw = float(std::atan2(dz, dx) * 180.0 / kPi) - 90.0f;
        mob.hasAttacked = true;
        break;
    }

    case MobAi::Fuse: {
        // `dd.a(Lkh;F)V`. **Three blocks to light and seven to stay lit**, so a
        // creeper you back away from keeps hissing for a while before it gives
        // up -- and gives up without exploding, because the fuse is only
        // advanced on the ticks this method runs.
        const bool lit = mob.creeperState > 0;
        if (!((!lit && distance < kFuseStartRange) || (lit && distance < kFuseHoldRange))) {
            break;
        }
        if (mob.fuse == 0) {
            world.playSoundAt("random.fuse", mob.body.x, mob.body.y, mob.body.z, 1.0f, 0.5f);
        }
        mob.creeperState = 1;
        ++mob.fuse;
        if (mob.fuse == kFuseTicks) {
            detonate(world, index, around);
            return;
        }
        mob.hasAttacked = true;
        break;
    }

    case MobAi::Leap: {
        // `ax.a(Lkh;F)V`. **Daylight is a forgetting, not a burning**: a spider
        // in a bright cell drops its target one tick in a hundred and walks
        // off. It does not catch fire -- only `mb` and `cw` do.
        if (entityBrightness(world, mob) > kSunBurnBrightness
            && rand_.nextInt(kSpiderForgetOdds) == 0) {
            mob.targetingPlayer = false;
            mob.path.clear();
            break;
        }
        if (distance <= kSpiderLeapNear || distance >= kSpiderLeapFar
            || rand_.nextInt(kSpiderLeapOdds) != 0) {
            // Out of the leap window: an ordinary `dq` fist.
            if (distance < kMeleeRange && them.box.maxY > mob.body.box.minY
                && them.box.minY < mob.body.box.maxY) {
                mob.attackTime = i16(kAttackCooldown);
                around.hurtPlayer(def.attackStrength, DamageSource::Monster, mob.body.x,
                                  mob.body.z);
            }
            break;
        }
        // **The leap needs the ground and does nothing without it** -- there is
        // no melee fallback on this branch, so a spider already in the air
        // simply does not attack this tick.
        if (mob.body.onGround) {
            const double dx = them.x - mob.body.x;
            const double dz = them.z - mob.body.z;
            const float flat = MathHelper::sqrtDouble(dx * dx + dz * dz);
            mob.body.motionX = dx / double(flat) * kSpiderLeapSpeed
                               + mob.body.motionX * kSpiderLeapKeep;
            mob.body.motionZ = dz / double(flat) * kSpiderLeapSpeed
                               + mob.body.motionZ * kSpiderLeapKeep;
            mob.body.motionY = kSpiderLeapRise;
        }
        break;
    }

    case MobAi::Melee:
    default: {
        // `dq.a(Lkh;F)V` -- **the vertical test is what stops a zombie hitting
        // you through a floor.** The boxes have to overlap in y, which at 2.5
        // blocks of horizontal reach they only do on the same level.
        if (distance >= kMeleeRange || them.box.maxY <= mob.body.box.minY
            || them.box.minY >= mob.body.box.maxY) {
            break;
        }
        mob.attackTime = i16(kAttackCooldown);
        around.hurtPlayer(def.attackStrength, DamageSource::Monster, mob.body.x, mob.body.z);
        break;
    }
    }
}

void MobSystem::detonate(tick::TickWorld& world, int index, const MobSurroundings& around)
{
    Mob& mob = mobs_[index];

    // `cn.a(Lkh;DDDF)V` at the creeper's own position -- its **feet**, since
    // `yOffset` is zero for a mob, not its middle.
    Explosion blast(mob.body.x, mob.body.y, mob.body.z, kCreeperBlast);
    blast.cast(world);

    // **Phase two runs while the blocks are still standing** (see
    // explosion.hpp), so a creeper that goes off behind a wall takes a
    // fraction of its own blast out of whoever is on the far side. It is
    // `applyBlast` because primed TNT reaches the same phase -- the only
    // difference is that TNT excludes nobody, its exploder argument being null.
    applyBlast(world, blast, this, &around, index);

    blast.destroy(world);

    // `F()` -- setEntityDead, straight away. **A creeper that explodes drops
    // nothing and makes no death sound**: it never reaches `onDeath` and never
    // spends the twenty ticks. The gunpowder is only ever from one you killed.
    mob.alive = false;
}

void MobSystem::splitSlime(const tick::TickWorld& world, const Mob& parent)
{
    // `ma.F()`. **Four children, in a square a quarter of the parent's size
    // around it**, each half the parent's size and each facing a random way.
    //
    // The condition is `size > 1 && health == 0` -- **exactly zero**, not "at
    // or below". A size-4 slime taking 20 damage in one hit ends on -4 and does
    // not split, which is a1.1.2's and is reproduced rather than rounded.
    if (parent.slimeSize <= 1 || parent.health != 0) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const float offX = (float(i % 2) - 0.5f) * float(parent.slimeSize) / 4.0f;
        const float offZ = (float(i / 2) - 0.5f) * float(parent.slimeSize) / 4.0f;
        // `new ma(world)` runs the constructor first -- which draws a size and
        // a hop delay out of the random -- and `setSlimeSize` overwrites the
        // size afterwards. Both draws are kept: skipping them is a different
        // stream for everything downstream.
        if (!spawn(world, MobType::Slime, parent.body.x + double(offX),
                   parent.body.y + 0.5, parent.body.z + double(offZ),
                   rand_.nextFloat() * 360.0f)) {
            return;
        }
        Mob& child = mobs_[mobs_.size() - 1];
        setSlimeSize(child, parent.slimeSize / 2);
        child.body.setFeet(parent.body.x + double(offX), parent.body.y + 0.5,
                           parent.body.z + double(offZ));
    }
}

void MobSystem::hopAbout(const tick::TickWorld& world, Mob& mob, const MobSurroundings& around)
{
    // `ma.b_()`, and what is *not* in it matters as much as what is:
    //
    //   * **no despawn.** `ge.b_()` carries the 128-block removal and the
    //     600-tick clock; `ma` overrides the method and does not call `super`,
    //     so a slime, alone among the nine, is permanent. Left alone, a cavern
    //     fills with them.
    //   * **no path and no `moveSpeed`.** It hops in whatever direction it is
    //     facing, and the only steering is `faceEntity` towards a player.
    //   * **no `entityAge`**, which nothing increments for a slime.

    //
    // **The slime asks for itself and so needs the Creative refusal spelled
    // out**: it is not an `ek`, so it never reaches `findPlayerToAttack` and
    // would otherwise be the one mob that still hurried towards a player it
    // cannot touch. See `MobPlayer::targetable`.
    const bool sees = around.player.present && around.player.targetable
                      && (around.player.x - mob.body.x) * (around.player.x - mob.body.x)
                                 + (around.player.y - mob.body.y) * (around.player.y - mob.body.y)
                                 + (around.player.z - mob.body.z) * (around.player.z - mob.body.z)
                             < kTargetRange * kTargetRange;
    if (sees) {
        faceTowards(mob, around.player.x, around.player.y, around.player.z, 10.0f);
    }

    if (mob.body.onGround) {
        const i16 before = mob.hopDelay--;
        if (before <= 0) {
            mob.hopDelay = i16(rand_.nextInt(kSlimeHopSpread) + kSlimeHopBase);
            if (sees) {
                // Integer division, so a delay of 10..29 becomes 3..9: a slime
                // that can see you hops about three times as often.
                mob.hopDelay = i16(mob.hopDelay / kSlimeHopHurry);
            }
            mob.jumping = true;
            // **Only a slime bigger than 1 makes a noise taking off**, which is
            // why a field of the smallest ones is silent.
            if (mob.slimeSize > 1) {
                world.playSoundAt("mob.slime", mob.body.x, mob.body.y, mob.body.z,
                                  kSlimeVolume,
                                  ((rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f)
                                      * 0.8f);
            }
            mob.squish = kSlimeHopSquish;
            // The strafe is a coin toss between -1 and 1 and the forward is the
            // size, so a big slime covers more ground per hop than a small one.
            mob.moveStrafing = 1.0f - rand_.nextFloat() * 2.0f;
            mob.moveForward = float(mob.slimeSize);
            return;
        }
    }

    mob.jumping = false;
    if (mob.body.onGround) {
        mob.moveForward = 0.0f;
        mob.moveStrafing = 0.0f;
    }
}

// `ge.z()` -- spawnExplosionParticle. See the declaration in mob.hpp for why
// the drift is subtracted from the position as well as carried as motion.
void MobSystem::explosionPuff(const tick::TickWorld& world, int index)
{
    if (index < 0 || index >= mobs_.size()) {
        return;
    }
    const Mob& mob = mobs_[index];
    for (int n = 0; n < kExplosionPuffs; ++n) {
        // **The three Gaussians come first**, before the position draws: the
        // jar stores them in locals and only then builds the argument list.
        // Named rather than nested for the reason `fizz` gives -- draws off one
        // generator in one argument list have no guaranteed order in C++ and a
        // fixed one in the jar.
        const double driftX = rand_.nextGaussian() * kExplosionPuffDrift;
        const double driftY = rand_.nextGaussian() * kExplosionPuffDrift;
        const double driftZ = rand_.nextGaussian() * kExplosionPuffDrift;
        const double px = mob.body.x + double(rand_.nextFloat() * mob.body.width * 2.0f)
                          - double(mob.body.width) - driftX * kExplosionPuffThrowBack;
        const double py = mob.body.y + double(rand_.nextFloat() * mob.body.height)
                          - driftY * kExplosionPuffThrowBack;
        const double pz = mob.body.z + double(rand_.nextFloat() * mob.body.width * 2.0f)
                          - double(mob.body.width) - driftZ * kExplosionPuffThrowBack;
        world.spawnParticle(int(ParticleKind::Explode), px, py, pz, driftX, driftY, driftZ);
    }
}

bool MobSystem::updateCounters(tick::TickWorld& world, int index,
                               const MobSurroundings& around)
{
    (void)around;
    Mob& mob = mobs_[index];
    const MobDef& def = mobDef(mob.type);
    const double midY = mob.body.y + double(mob.body.height) * 0.5;

    // `ge.y()`'s idle noise: the counter is compared against a draw from 1000
    // *before* it is incremented, and reset to minus the talk interval when it
    // fires -- so the odds climb from nothing to certainty over two minutes.
    if (mob.health > 0) {
        const int draw = rand_.nextInt(1000);
        const int before = mob.livingSoundTime++;
        if (draw < before) {
            mob.livingSoundTime = -def.talkInterval;
            const float pitch = (rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f;
            // **The draw happens for a creeper and a slime too and is then
            // thrown away.** `ge.y()` calls `getLivingSound()` and only checks
            // the result for null afterwards, so the clock still runs and the
            // stream still advances -- which is why this is a null test here
            // and not a `hostile` test up at the `nextInt`.
            if (def.livingSound != nullptr) {
                world.playSoundAt(def.livingSound, mob.body.x, midY, mob.body.z,
                                  def.soundVolume, pitch);
            }
        }
    }

    // **`kh.y()`'s water branch, which comes before the fire counter.** That
    // order is the method's and it is the whole of "jumping in a lake puts a
    // burning cow out": the branch zeroes `fire` before the counter below can
    // spend another tick of it. It also clears the fall distance, which is why
    // an animal that walks off a cliff into water is unharmed.
    //
    // `updateWaterEntry` asks `handleWaterMovement`, which is a mutator, so
    // this is also where the current carries the animal -- once here and again
    // inside `moveEntityWithHeading`, exactly as the jar does it twice.
    const WaterEntryResult wet = mob.body.updateWaterEntry(world);
    if (wet.splash) {
        world.playSoundAt(kSplashSound, mob.body.x, mob.body.y, mob.body.z, wet.volume,
                          splashPitch(rand_));
        // The spray, which is a row of each scaled by the animal's own width --
        // a chicken makes half what a cow does. See core/entity/water_entry.hpp.
        waterEntryParticles(world, rand_, mob.body.x, mob.body.box.minY, mob.body.z,
                            mob.body.width, mob.body.motionX, mob.body.motionY,
                            mob.body.motionZ);
    }
    if (wet.inWater) {
        mob.fire = 0;
    }

    // `kh.y()`'s fire, lava and void, in its order. **Every one of them is
    // `attackEntityFrom(null, n)`** in the jar, which is why they go through
    // `attack` here rather than subtracting: a cow that falls in lava should
    // leave its leather and make the noise a cow makes.
    if (mob.fire > 0) {
        if (mob.fire % 20 == 0) {
            attack(world, index, 1, false);
        }
        --mob.fire;
    }
    if (block::isMaterialInBox(world, mob.body.box, kLavaMaterial)) {
        attack(world, index, kLavaDamage, false);
        mob.fire = i16(kLavaFireTicks);
    }
    if (mob.body.y < kVoidY) {
        // `ge.E()` -- the void does four a tick to a living thing rather than
        // killing it outright, which is `Entity`'s behaviour.
        attack(world, index, kVoidDamage, false);
    }

    // Drowning. The box is shrunk the same way `isInsideOfMaterial` shrinks it
    // -- a cow standing in a puddle is not drowning.
    if (mob.health > 0) {
        const AABB head{mob.body.box.minX, mob.body.box.maxY - 0.1,       mob.body.box.minZ,
                        mob.body.box.maxX, mob.body.box.maxY - 0.05, mob.body.box.maxZ};
        if (block::isMaterialInBox(world, head, kWaterMaterial)) {
            --mob.air;
            if (mob.air <= kDrownAt) {
                mob.air = 0;
                // **Eight bubbles, and they come out before the blow.** A
                // block either way on all three axes -- two draws per axis, so
                // the cloud is thickest at the head -- and each carries the
                // animal's own motion, which is what makes a drowning cow
                // trail bubbles as it drifts.
                for (int n = 0; n < 8; ++n) {
                    const double dx = double(rand_.nextFloat() - rand_.nextFloat());
                    const double dy = double(rand_.nextFloat() - rand_.nextFloat());
                    const double dz = double(rand_.nextFloat() - rand_.nextFloat());
                    world.spawnParticle(int(ParticleKind::Bubble), mob.body.x + dx,
                                        mob.body.y + dy, mob.body.z + dz,
                                        mob.body.motionX, mob.body.motionY,
                                        mob.body.motionZ);
                }
                attack(world, index, 2, false);
            }
        } else {
            mob.air = i16(kMaxAir);
        }
    }

    // Suffocation: `isEntityInsideOpaqueBlock`, asked at the eye.
    if (mob.health > 0) {
        const i32 bx = MathHelper::floorDouble(mob.body.x);
        const int by =
            int(MathHelper::floorDouble(mob.body.y + double(mob.body.height) * kEyeScale));
        const i32 bz = MathHelper::floorDouble(mob.body.z);
        if (world.opaqueAt(bx, by, bz)) {
            attack(world, index, 1, false);
        }
    }

    if (mob.attackTime > 0) {
        --mob.attackTime;
    }
    if (mob.hurtTime > 0) {
        --mob.hurtTime;
    }
    if (mob.hurtResistant > 0) {
        --mob.hurtResistant;
    }

    if (mob.health <= 0) {
        ++mob.deathTime;
        if (mob.deathTime > kDeathTicks) {
            // **The death puff** -- `ge.e_()` calls `z()` the tick the corpse
            // goes. Six draws a particle and two of them Gaussian, which is the
            // one place a mob's death touches this pool's generator hard. See
            // `explosionPuff`.
            explosionPuff(world, index);
            //
            // **`ma.F()` -- the split -- happens here** and not where the blow
            // landed: `setEntityDead` is what a slime overrides, and the twenty
            // ticks run first. So a big slime you kill lies there and *then*
            // becomes four.
            if (mob.type == MobType::Slime) {
                splitSlime(world, mob);
            }
            return false;
        }
    }

    // The previous tick's angles, taken at the end of `onEntityUpdate` exactly
    // as the jar does -- so everything after this in the tick moves the current
    // ones and the renderer has both.
    mob.prevRenderYaw = mob.renderYaw;
    mob.prevYaw = mob.yaw;
    mob.prevPitch = mob.pitch;
    return true;
}

void MobSystem::shove(Mob& mob, double otherX, double otherZ, double* otherMotionX,
                      double* otherMotionZ)
{
    // `kh.f(kh)` -- applyEntityCollision, and two things in it are easy to get
    // wrong. The separation is scaled by the **square root of the larger of the
    // two absolute offsets** rather than by the distance, and the reciprocal of
    // that same number is clamped at one -- so two animals standing in exactly
    // the same cell shove each other hard and two barely touching hardly at
    // all. `entityCollisionReduction` is zero for everything living in this
    // version, so the factor the jar applies here is one.
    double dx = otherX - mob.body.x;
    double dz = otherZ - mob.body.z;
    const double longest = std::abs(dx) > std::abs(dz) ? std::abs(dx) : std::abs(dz);
    if (longest < kPushMinDistance) {
        return;
    }
    const double length = std::sqrt(longest);
    dx /= length;
    dz /= length;
    double scale = 1.0 / length;
    if (scale > 1.0) {
        scale = 1.0;
    }
    dx *= scale * kPushScale;
    dz *= scale * kPushScale;
    mob.body.motionX -= dx;
    mob.body.motionZ -= dz;
    if (otherMotionX != nullptr) {
        *otherMotionX += dx;
    }
    if (otherMotionZ != nullptr) {
        *otherMotionZ += dz;
    }
}

// **`ge.j()`'s first block** -- the walk towards where the server last said.
//
// ```java
// if (newPosRotationIncrements > 0) {
//     double d  = posX + (newPosX - posX) / (double)newPosRotationIncrements;
//     ...                                  // and the same for y, z and pitch
//     double d3 = MathHelper.wrapAngleTo180(newRotationYaw - (double)rotationYaw);
//     rotationYaw = (float)((double)rotationYaw + d3 / (double)newPosRotationIncrements);
//     newPosRotationIncrements--;
//     setPosition(d, d1, d2);
//     setRotation(rotationYaw, rotationPitch);
// }
// ```
//
// The divisor is what is *left* on the clock, not what it started at, so the
// gap is closed by a third, then a half, then wholly -- a body that hears
// nothing more lands exactly on the last thing it was told and stops. Identical
// in shape to `net::RemoteEntities::tick`, because it is the same method in the
// jar one class further up.
//
// **`setPosition`, not `moveEntity`.** Nothing is swept and nothing collides:
// the server has already decided where this animal is, and a client that
// clipped the walk would argue with it.
void MobSystem::interpolateToServer(Mob& mob) const
{
    if (mob.smoothTicks <= 0) {
        return;
    }
    const double steps = double(mob.smoothTicks);
    const double x = mob.body.x + (mob.serverX - mob.body.x) / steps;
    const double y = mob.body.y + (mob.serverY - mob.body.y) / steps;
    const double z = mob.body.z + (mob.serverZ - mob.body.z) / steps;

    const double turn = double(wrapDegrees(float(mob.serverYaw - double(mob.yaw))));
    mob.yaw = float(double(mob.yaw) + turn / steps);
    mob.pitch = float(double(mob.pitch) + (mob.serverPitch - double(mob.pitch)) / steps);
    --mob.smoothTicks;

    mob.body.setFeet(x, y, z);
    // A server's animal is placed, never pushed, so whatever motion it had is
    // not carried into the next tick -- there is no next tick for it here.
    mob.body.motionX = 0.0;
    mob.body.motionY = 0.0;
    mob.body.motionZ = 0.0;
}

// **`ge.e_()`'s tail**, which every animal runs -- the server's as much as this
// console's. The legs swing with the distance actually covered (that half is in
// `moveEntityWithHeading` in the jar and is here because the body does not know
// it is a mob), and the body turns towards the direction of travel at 30 % a
// tick while the head keeps its own yaw.
//
// It is a function of its own because a remote animal needs it too and needs
// nothing else in the tick: `ge.B` -- `isMultiplayerEntity`, which `gy.a(ez)`
// sets on every mob it spawns -- suppresses `b_()`, the AI, and **only** `b_()`.
// Everything below this line still runs over there, which is why a cow walking
// past on somebody else's console has legs and turns to face where it is going
// rather than sliding sideways.
void MobSystem::headingAndLight(const tick::TickWorld& world, Mob& mob, double beforeX,
                                double beforeZ) const
{
    const double movedX = mob.body.x - beforeX;
    const double movedZ = mob.body.z - beforeZ;
    mob.prevLimbYaw = mob.limbYaw;
    float travelled = MathHelper::sqrtDouble(movedX * movedX + movedZ * movedZ) * 4.0f;
    if (travelled > 1.0f) {
        travelled = 1.0f;
    }
    mob.limbYaw += (travelled - mob.limbYaw) * 0.4f;
    mob.limbSwing += mob.limbYaw;

    float heading = mob.renderYaw;
    const double flat = std::sqrt(movedX * movedX + movedZ * movedZ);
    if (flat > 0.05) {
        heading = float(std::atan2(movedZ, movedX) * 180.0 / kPi) - 90.0f;
    }
    float delta = wrapDegrees(heading - mob.renderYaw);
    mob.renderYaw += delta * 0.3f;
    float headTurn = wrapDegrees(mob.yaw - mob.renderYaw);
    if (headTurn < -75.0f) {
        headTurn = -75.0f;
    }
    if (headTurn > 75.0f) {
        headTurn = 75.0f;
    }
    mob.renderYaw = mob.yaw - headTurn;
    if (headTurn * headTurn > 2500.0f) {
        mob.renderYaw += headTurn * 0.2f;
    }

    // **Every tick, and this is what a remote animal was missing.** The light
    // byte a mob is drawn with used to be written once, by the spawn, and never
    // again for one the server owns -- so an animal that arrived before the
    // column it stands in was lit stayed at zero for its whole life and was
    // drawn black, and one that walked out of a cave stayed cave-dark in the
    // sun. `nq`/`dn` read the world's light at the entity on every frame; this
    // is the tick's share of that.
    mob.light = packedLightAt(world, mob.body.x,
                              mob.body.y + double(mob.body.height) * 0.5, mob.body.z);
}

void MobSystem::tick(tick::TickWorld& world, const MobSurroundings& around)
{
    // **What the tick actually cost, kept rather than enforced.** This used to
    // be a budget: one search a tick across every mob in the world. See
    // `MobSystem::peakSearchesPerTick`.
    if (searchesThisTick_ > peakSearchesPerTick_) {
        peakSearchesPerTick_ = searchesThisTick_;
    }
    searchesThisTick_ = 0;

    for (int i = 0; i < mobs_.size();) {
        Mob& mob = mobs_[i];

        // **A mob the server owns has no mind here**, and that is the whole of
        // what `ge.B` suppresses in the jar: `j()` skips `b_()` and runs
        // everything else. No AI, no physics, no despawn -- all three belong to
        // the server -- but the walk towards what the server last said, the
        // legs, the heading and the light are this console's, because they are
        // what a frame is drawn from. See MobSystem::spawnFromServer.
        if (mob.remote) {
            const double beforeX = mob.body.x;
            const double beforeZ = mob.body.z;
            mob.body.snapRenderPosition();
            mob.prevYaw = mob.yaw;
            mob.prevRenderYaw = mob.renderYaw;
            mob.prevPitch = mob.pitch;
            ++mob.ticksExisted;
            interpolateToServer(mob);
            headingAndLight(world, mob, beforeX, beforeZ);
            ++i;
            continue;
        }

        // Ours, and every pool here has it: a mob outlives the column under it,
        // so one at the edge of the render distance would otherwise fall
        // through unloaded ground. See core/entity/boat.hpp.
        if (!world.chunkResident(MathHelper::floorDouble(mob.body.x) >> 4,
                                 MathHelper::floorDouble(mob.body.z) >> 4)) {
            ++i;
            continue;
        }

        if (!updateCounters(world, i, around)) {
            removeAt(i);
            continue;
        }

        ++mob.ticksExisted;

        // ---- `mb.j()` / `cw.j()` -- catching fire at dawn ----------------
        //
        // Identical eight lines in both classes, and the draw is what makes it
        // gradual: `nextFloat() * 30 < (brightness - 0.4) * 2` is impossible
        // below a brightness of 0.4 and still only about one tick in twenty at
        // full daylight, so a zombie caught in the open smoulders for a second
        // or two before it goes up rather than igniting on the exact tick.
        //
        // **`isDaytime` is `skylightSubtracted < 4`** -- the world's, not the
        // cell's -- and the sky test is the chunk height map, so a zombie under
        // a one-block overhang is safe.
        const MobDef& kind = mobDef(mob.type);
        if (kind.burnsInSunlight && mob.health > 0 && world.skyDarken() < 4) {
            const float bright = entityBrightness(world, mob);
            if (bright > kSunBurnBrightness
                && world.canSeeSky(MathHelper::floorDouble(mob.body.x),
                                   int(MathHelper::floorDouble(mob.body.y)),
                                   MathHelper::floorDouble(mob.body.z))
                && rand_.nextFloat() * kSunBurnDraw < (bright - kSunBurnBias) * 2.0f) {
                mob.fire = i16(kSunBurnTicks);
            }
        }

        // `dq.j()`'s first two lines. **A monster standing in light ages twice
        // as fast**, and `entityAge` is what despawns it -- so one that survives
        // the morning under a tree is gone in half the time one in a cave is.
        if (kind.hostile && kind.ai != MobAi::Hop
            && entityBrightness(world, mob) > kSunBurnBrightness) {
            mob.entityAge += kMonsterDaylightAgeing;
        }

        // `ma.e_()` takes both of these **before** `super.e_()`, so the squish
        // the renderer interpolates is last tick's and the landing test below
        // compares against where the body was when the tick started.
        const bool wasOnGround = mob.body.onGround;
        if (mob.type == MobType::Slime) {
            mob.prevSquish = mob.squish;
        }

        // ---- `ge.j()` -- onLivingUpdate ---------------------------------
        if (mob.health <= 0) {
            mob.jumping = false;
            mob.moveStrafing = 0.0f;
            mob.moveForward = 0.0f;
            mob.randomYaw = 0.0f;
        } else if (!updateActionState(world, i, around)) {
            // Despawned. Nothing is dropped and nothing is heard.
            removeAt(i);
            continue;
        }

        mob.moveStrafing *= 0.98f;
        mob.moveForward *= 0.98f;
        mob.randomYaw *= 0.9f;

        // **`moveEntityWithHeading`, which is the player's.** The jump branch
        // inside it is `ge.j()`'s: in water or lava the button is a flat rise
        // and on the ground it is the 0.42 impulse. See
        // core/entity/player_body.cpp -- the one thing that file does which the
        // jar does not is treat jump on a ladder as a climb, and a mob that is
        // on a ladder is pressed against it anyway (`collidedHorizontally` sets
        // `jumping`), so both routes give the same 0.2.
        PlayerInput input;
        input.strafe = mob.moveStrafing;
        input.forward = mob.moveForward;
        input.yawDegrees = mob.yaw;
        input.jump = mob.jumping;

        const double beforeX = mob.body.x;
        const double beforeZ = mob.body.z;

        // **A ridden pig is ticked exactly like an unridden one**, and that is
        // the jar rather than a shortcut: `mv` reads nothing from
        // `riddenByEntity` and there is no steering anywhere in this version,
        // so a saddled pig goes where its own AI says and the player goes with
        // it. Steering a pig with a carrot on a stick is 1.4's.
        mob.body.tick(world, input);

        // **A cow ruins a field exactly as a player does**: `onEntityWalking`
        // is `moveEntity`'s and nothing in it asks what is standing there. The
        // animal's own footstep earns it, one per block covered, and the call
        // comes before the collision scan below because that is the order the
        // jar's tail has. See tick::entityWalkedOnBlock.
        if (mob.body.steppedOn) {
            tick::entityWalkedOnBlock(world, mob.body.stepBlockX, mob.body.stepBlockY,
                                      mob.body.stepBlockZ);
        }

        // **The blocks the animal is standing in**, which is the first half of
        // `moveEntity`'s tail and in a1.1.2 means the cactus. Each cell is its
        // own `attackEntityFrom(null, 1)`, and the ten-tick invulnerability
        // window inside `attack` is what turns a point a tick into about a
        // heart a second. Nothing is removed from the pool here -- a mob that
        // dies keeps its slot and spends its death countdown, exactly as one
        // killed by the fire branch below does. See
        // core/entity/block_contact.hpp.
        {
            const int hits = blockContactHits(world, mob.body.box);
            for (int hit = 0; hit < hits; ++hit) {
                attack(world, i, kContactDamage, false);
            }
        }

        // **`kh.c()`'s tail, all of it** -- the branch that sets an animal
        // alight from a fire block, damages it every tick it stands in one,
        // and hisses when a burning one goes under. It is `moveEntity`'s last
        // act rather than `y()`'s, which is why it is here and not up with the
        // water branch above: the tick an animal walks out of lava and into a
        // river, `y()` still saw it dry and only the move put it under.
        //
        // **The ignition half used to be missing**, on a reading of the jar
        // that was wrong in one place -- `og` has no entity callback, but `kh`
        // writes its own counter here, from a box test against the world. See
        // core/entity/fire_entry.hpp, which carries the correction and the
        // bytecode.
        //
        // `fireResistance` is 1 for every animal, so a cow standing in a flame
        // is alight on the tick after it steps in: the counter rests at -1 and
        // catching fire is the tick it reaches zero.
        const FireEntryResult burn =
            updateFireEntry(&mob.fire, boundingBoxBurning(world, mob.body.box),
                            fireWetProbe(world, mob.body.box));
        if (burn.damage) {
            // `dealFireDamage(1)` is `attackEntityFrom(null, 1)`, and for an
            // `EntityLiving` that lands only when the ten-tick invulnerability
            // window has lapsed -- so standing in fire costs an animal about a
            // heart a second rather than one a tick.
            attack(world, i, 1, false);
        }
        if (burn.fizz) {
            world.playSoundAt(kFizzSound, mob.body.x, mob.body.y, mob.body.z, 0.7f,
                              fizzPitch(rand_));
        }

        // The footstep, which `Entity.moveEntity` earns for a mob exactly as it
        // does for a player.
        // Every cue's key is a string literal out of the generated step-sound
        // table, so `data()` is null-terminated and nothing here allocates --
        // which this loop may not do. See core/audio/block_sound.hpp.
        if (mob.body.stepSoundDue != block::kAir) {
            const audio::SoundCue cue = audio::stepCue(mob.body.stepSoundDue);
            if (cue.playable()) {
                world.playSoundAt(cue.key.data(), mob.body.x, mob.body.y, mob.body.z,
                                  cue.volume, cue.pitch);
            }
        }

        // **The shove**, `ge.j()`'s tail: everything inside a box a fifth of a
        // block wider than this one is pushed apart from it. In this build that
        // is the player and the other animals, and the animals are the half
        // that shows -- without it a herd stands in one cell.
        {
            const AABB reach = mob.body.box.expand(0.2, 0.0, 0.2);
            if (around.player.present && around.player.motionX != nullptr
                && reach.intersects(around.player.box)) {
                shove(mob, around.player.x, around.player.z, around.player.motionX,
                      around.player.motionZ);
            }
            for (int other = 0; other < mobs_.size(); ++other) {
                if (other == i) {
                    continue;
                }
                Mob& neighbour = mobs_[other];
                if (!neighbour.alive || !reach.intersects(neighbour.body.box)) {
                    continue;
                }
                shove(mob, neighbour.body.x, neighbour.body.z, &neighbour.body.motionX,
                      &neighbour.body.motionZ);
            }
        }

        // ---- the chicken's wings, `mz.j()` ------------------------------
        if (mob.type == MobType::Chicken) {
            mob.prevWingRotation = mob.wingRotation;
            mob.prevDestPos = mob.destPos;
            mob.destPos = float(double(mob.destPos)
                                + double(mob.body.onGround ? -1 : 4) * kChickenDestStep);
            if (mob.destPos < 0.0f) {
                mob.destPos = 0.0f;
            }
            if (mob.destPos > 1.0f) {
                mob.destPos = 1.0f;
            }
            if (!mob.body.onGround && mob.wingSpeed < 1.0f) {
                mob.wingSpeed = 1.0f;
            }
            mob.wingSpeed = float(double(mob.wingSpeed) * kChickenFlapDecay);
            // **Falling is slowed, not stopped**: a chicken keeps 60 % of a
            // downward motion every tick, which is why one walking off a cliff
            // drifts down instead of dropping.
            if (!mob.body.onGround && mob.body.motionY < 0.0) {
                mob.body.motionY *= kChickenFallDamping;
            }
            mob.wingRotation += mob.wingSpeed * kChickenFlapGain;

            // The egg. Laid on the world's clock and not on the player's, and
            // the timer is re-drawn rather than reset to a constant.
            if (mob.health > 0 && --mob.eggTime <= 0) {
                world.playSoundAt("mob.chickenplop", mob.body.x, mob.body.y, mob.body.z, 1.0f,
                                  (rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f);
                world.spawnItem(mob.body.x, mob.body.y, mob.body.z, u16(mcver::Item::Egg), 1);
                mob.eggTime = rand_.nextInt(kEggTimeSpread) + kEggTimeBase;
            }
        }

        // ---- `ma.e_()`'s tail: the landing ------------------------------
        //
        // **The squish is not driven, it decays.** A hop sets it to 1, a
        // landing to -0.5, and every tick multiplies by 0.6 -- so a slime
        // stretches on the way up and flattens on the way down with no
        // animation clock anywhere.
        if (mob.type == MobType::Slime) {
            if (mob.body.onGround && !wasOnGround) {
                // **`size * 8` slime blobs, thrown in a ring on the floor.**
                // The angle is uniform round the circle and the radius is
                // `size * 0.5` scaled by a draw in 0.5..1, so a big slime
                // splatters wider as well as more -- and the height is
                // `boundingBox.minY`, the floor it just hit, not the middle of
                // the body.
                for (int n = 0; n < int(mob.slimeSize) * 8; ++n) {
                    const float angle = rand_.nextFloat() * 3.1415927f * 2.0f;
                    const float reach = rand_.nextFloat() * 0.5f + 0.5f;
                    const float radius = float(mob.slimeSize) * 0.5f * reach;
                    world.spawnParticle(
                        int(ParticleKind::Slime),
                        mob.body.x + double(MathHelper::sin(angle) * radius),
                        mob.body.box.minY,
                        mob.body.z + double(MathHelper::cos(angle) * radius));
                }
                if (mob.slimeSize > 2) {
                    world.playSoundAt("mob.slime", mob.body.x, mob.body.y, mob.body.z,
                                      kSlimeVolume,
                                      ((rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f)
                                          / 0.8f);
                }
                mob.squish = kSlimeLandSquish;
            }
            mob.squish = float(double(mob.squish) * kSlimeSquishDecay);

            // `ma.b(Ldm;)V` -- **onCollideWithPlayer, which is damage by
            // standing on you rather than by swinging.** The player's own
            // `onUpdate` runs it over everything inside its box grown a block
            // sideways; a slime of size 1 does nothing at all.
            if (mob.health > 0 && mob.slimeSize > 1 && around.player.present
                && around.player.targetable
                && mob.body.box.expand(1.0, 0.0, 1.0).intersects(around.player.box)) {
                const double dx = mob.body.x - around.player.x;
                const double dy = mob.body.y - around.player.y;
                const double dz = mob.body.z - around.player.z;
                const double reach = kSlimeTouchScale * double(mob.slimeSize);
                if (double(MathHelper::sqrtDouble(dx * dx + dy * dy + dz * dz)) < reach
                    && canSee(world, mob, around.player.x, around.player.y,
                              around.player.z)) {
                    // `ma` is not a `dq`, so difficulty leaves a slime's
                    // touch alone.
                    around.hurtPlayer(int(mob.slimeSize), DamageSource::Other, mob.body.x,
                                      mob.body.z);
                    world.playSoundAt("mob.slimeattack", mob.body.x, mob.body.y, mob.body.z,
                                      1.0f,
                                      (rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f);
                }
            }
        }

        // ---- the body's heading, `ge.e_()`'s tail -----------------------
        headingAndLight(world, mob, beforeX, beforeZ);

        // `dq.e_()`'s last line: **`if (world.difficulty == 0) setEntityDead()`**,
        // after the whole tick and not before it. So on Peaceful a monster is
        // spawned, ticks once, and is gone -- it is a removal and not a
        // suppression, which is why the 200-cap spawner still runs and still
        // costs what it costs. The slime is not a `dq` and is not removed;
        // `ma.a()Z` refuses to *spawn* a big one instead.
        if (kind.hostile && kind.ai != MobAi::Hop && around.difficulty == 0) {
            removeAt(i);
            continue;
        }
        ++i;
    }

    // **The jockey, put back on its spider.** A second pass, because a rider
    // may sit earlier in the pool than its mount and a one-pass version would
    // seat half of them a tick behind. It is the same overwrite
    // `PlayerBody::followSeat` does for a boat and for the same reason: `ge`
    // never checks whether it is riding, so the skeleton has already run its
    // AI, aimed and fired, and only where it *is* comes from the spider.
    for (int i = 0; i < mobs_.size(); ++i) {
        Mob& rider = mobs_[i];
        // `rider.mountIndex == i` cannot happen from `mountOn`, which refuses
        // it, but can from a save whose pool lost an entry between write and
        // read -- and a mob seated on itself climbs 1.35 blocks a tick.
        if (rider.mountIndex < 0 || rider.mountIndex == i) {
            rider.mountIndex = -1;
            continue;
        }
        const RiderSeat where = seatOf(rider.mountIndex);
        if (!where.valid) {
            rider.mountIndex = -1;
            continue;
        }
        rider.body.setFeet(where.x, where.y, where.z);
        rider.body.motionX = 0.0;
        rider.body.motionY = 0.0;
        rider.body.motionZ = 0.0;
        rider.body.onGround = true;
    }

    mobs_.trim();
}


// See mob.hpp. The transcription is `je`'s middle block, whose two halves --
// the damage and the impulse -- come out of one `effectOn`.
void applyBlast(tick::TickWorld& world, const Explosion& blast, MobSystem* mobs,
                const MobSurroundings* around, int exclude)
{
    int damage = 0;
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;

    if (around != nullptr && around->player.present
        && blast.effectOn(world, around->player.x, around->player.y, around->player.z,
                          around->player.box, &damage, &vx, &vy, &vz)) {
        // `hurtPlayer` wants somewhere to be knocked back *from*, and for a
        // blast that is the centre of it.
        //
        // **Who exploded decides whether difficulty applies.** `je` hands its
        // exploder to `attackEntityFrom`: a creeper is a `dq` and is scaled,
        // and primed TNT passes null, which is an unscaled hit with no
        // knockback of its own. A mob that blew up is the one excluded here.
        const DamageSource source = exclude >= 0 ? DamageSource::Monster : DamageSource::World;
        around->hurtPlayer(damage, source, blast.x(), blast.z());
        // The impulse is **added directly** and is not `knockBack`: `je` writes
        // `motionX += ...` rather than calling the halve-and-lift.
        if (around->player.motionX != nullptr) {
            *around->player.motionX += vx;
        }
        if (around->player.motionY != nullptr) {
            *around->player.motionY += vy;
        }
        if (around->player.motionZ != nullptr) {
            *around->player.motionZ += vz;
        }
    }

    if (around != nullptr && around->items != nullptr) {
        around->items->takeBlast(world, blast);
    }

    if (mobs == nullptr) {
        return;
    }
    for (int other = 0; other < mobs->count(); ++other) {
        if (other == exclude || !(*mobs)[other].alive) {
            continue;
        }
        Mob& victim = mobs->at(other);
        if (!blast.effectOn(world, victim.body.x, victim.body.y, victim.body.z,
                            victim.body.box, &damage, &vx, &vy, &vz)) {
            continue;
        }
        // `fromPlayer` is false: an explosion is not an `EntityLiving`, so a
        // sheep caught in one keeps its wool.
        mobs->attack(world, other, damage, false);
        victim.body.motionX += vx;
        victim.body.motionY += vy;
        victim.body.motionZ += vz;
    }
}

}  // namespace mc::entity
