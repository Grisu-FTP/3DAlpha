#pragma once

// **An arrow in flight** -- `kg`, which is `EntityArrow`, and the fourth entity
// in this project.
//
// "The bow doesn't work" was the same bug paintings had and the whole
// `spawns` column exists for: `jg.a(ev, cn, dm)` -- ItemBow.onItemRightClick --
// runs to completion, plays its sound, builds a `kg` and hands it to the world.
// With nowhere to put an entity the click did every step and produced nothing.
//
// **There is no charge in this version.** `jg` has no `onUsingTick`, no
// `getMaxItemUseDuration` and no `onPlayerStoppedUsing` -- drawing a bow arrives
// in Beta 1.8 with the same release Creative does. One click is one arrow at a
// fixed 1.5 velocity, and a build that waited for a draw would wait forever.
//
// **It does not use `moveEntity`**, which is what makes it the cheapest entity
// here after the painting. There is no collision sweep, no step-up and no
// friction: an arrow ray-traces from where it is to where it would be, stops at
// whatever the ray hit, and otherwise adds its motion to its position. The
// sweep in core/entity/sweep.hpp is untouched by this file.
//
// Constants, all read off the class file rather than remembered -- and one of
// them is the sort of thing that is remembered wrong:
//
//   * **Gravity is 0.03, not 0.05.** 0.05 is later Minecraft. An arrow ported
//     with 0.05 drops noticeably short and reads as bad aim rather than as a
//     wrong constant.
//   * Drag is 0.99 in air and 0.8 in water.
//   * The launch velocity is 1.5 and the inaccuracy 1.0, both `jg`'s literals,
//     and the scatter is `nextGaussian() * 0.0075 * inaccuracy` per axis.
//   * An arrow stuck in a block dies after **1200 ticks**, which is a minute.
//
// One thing is openly not a1.1.2's.
//
//   * **It costs no arrow.** The original consumes one through
//     `InventoryPlayer.consumeInventoryItem` and refuses to fire without one.
//     This build has no stack depletion at all -- placing a block does not
//     spend it either -- so requiring ammunition here would be the one place
//     Creative's hand was a stock rather than a catalogue. That is the same
//     invented exemption `docs/status.md` §0s already argues for, and it is
//     stated rather than hidden.
// **What it can hit** is every entity whose `canBeCollidedWith` answers true,
// read off the class files: the painting (`jc`, always), the boat (`dc`) and
// the minecart (`oc`, both while alive). A dropped item and the base `Entity`
// answer false. The sweep is the original's nearest-target sweep, clipped to
// the first block hit, and a struck target takes `attackEntityFrom(shooter, 4)`
// -- which kills a painting outright and puts 40 on a vehicle's counter, so a
// second arrow inside two seconds breaks it. The player is a target too, once
// the arrow is five ticks old -- see `Arrow::shooterGrace` for the five.
//
// Drawing is elsewhere, as ever: core/render/arrow_mesh.hpp is `gk`.

#include "core/entity/damage_source.hpp"
#include "core/entity/water_entry.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {
class BoatSystem;
class MinecartSystem;
class MobSystem;
class PaintingSystem;

// `setSize(0.5F, 0.5F)`, and `yOffset = 0.0F` -- so unlike a dropped item the
// position is the box's **bottom** centre, which is `Entity.setPosition`'s
// ordinary rule.
inline constexpr double kArrowSize = 0.5;

// `jg`'s two literals, passed to `setThrowableHeading`.
inline constexpr float kArrowVelocity = 1.5f;
inline constexpr float kArrowInaccuracy = 1.0f;

// The per-axis scatter, `nextGaussian() * 0.007499999832361937 * inaccuracy`.
inline constexpr double kArrowScatter = 0.007499999832361937;

// The eye offset the constructor applies before it launches: back along the
// heading by 0.16 horizontally and down by a tenth.
inline constexpr double kArrowMuzzleBack = 0.16;
inline constexpr double kArrowMuzzleDrop = 0.10000000149011612;

// `0.99F` in air, `0.8F` in water, and **`0.03F` of gravity** -- see the header
// note. All three are floats in the class file and are widened per tick.
inline constexpr float kArrowAirDrag = 0.99f;
inline constexpr float kArrowWaterDrag = 0.8f;
inline constexpr float kArrowGravity = 0.03f;

// How far a stuck arrow backs out of the face it hit, so it is not drawn inside
// the block.
inline constexpr double kArrowEmbed = 0.05000000074505806;

// `arrowShake`, set to 7 on impact and counted down. The renderer wobbles the
// arrow by it, which is the only thing it is for.
inline constexpr int kArrowShake = 7;

// 1200 ticks stuck in a block, which is a minute.
inline constexpr int kArrowMaxStuck = 1200;

// `if (posY < -64.0D) setDead()`, the last line of `Entity.onEntityUpdate`.
inline constexpr double kArrowVoidFloor = -64.0;

// `entityHit.attackEntityFrom(shootingEntity, 4)`.
inline constexpr int kArrowDamage = 4;

// `boundingBox.expand(0.3F, 0.3F, 0.3F)` -- how much larger than its own box a
// target is to an arrow's ray.
inline constexpr double kArrowTargetGrow = 0.30000001192092896;

// The angle smoothing at the end of `onUpdate`: a fifth of the way to the new
// heading each tick, which is what stops an arrow snapping as it arcs over.
inline constexpr float kArrowTurnRate = 0.2f;

// `ticksInAir >= 5` -- how long an arrow ignores the entity that fired it.
inline constexpr int kArrowSelfGrace = 5;

// **Who fired it**, which a1.1.2 keeps as an `Entity shootingEntity` reference
// and reads for exactly two things: `if (entityHit == shootingEntity) skip`
// during the first five ticks, and `dd.b(Lkh;)V`'s `instanceof cw`. The second
// is the only way to get a music disc in this version -- see
// `MobSystem::dropOnDeath` -- so the arrow has to remember which it was.
enum class ArrowShooter : u8 { Player, Skeleton };

struct Arrow {
    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;
    AABB box{};

    // Degrees, as the original stores them, and the `prev` pair is what the
    // renderer interpolates against.
    float yaw = 0.0f, pitch = 0.0f;
    float prevYaw = 0.0f, prevPitch = 0.0f;

    // The block it is stuck in, and what that block was when it stuck. The
    // second is how it notices the block has been mined out from under it.
    i32 tileX = 0, tileZ = 0;
    int tileY = 0;
    int inTile = 0;

    bool inGround = false;
    int shake = 0;

    // `kh`'s `aV` and `c`. Not saved, as the jar does not save it: an arrow
    // reloaded under water has no entry to make.
    WaterEntry water{};

    int ticksInGround = 0;
    int ticksInAir = 0;

    u8 light = 0;
    bool alive = false;

    // See `ArrowShooter`. One byte, and it is saved, because a creeper shot by
    // a skeleton across a world reload should still drop a record.
    ArrowShooter shooter = ArrowShooter::Player;

    // **Not hitting whoever fired it**, which `kg.e_()` does by reference:
    // `entity != shootingEntity || ticksInAir >= 5`. A skeleton's arrow starts
    // 1.3 blocks up inside the skeleton's own 0.6 x 1.8 box, so without this it
    // shoots itself on the tick it is loosed. **The player's shot is no
    // different**: the muzzle offset backs the arrow up by 0.16 against a
    // half-width of 0.3, so it too is born inside its shooter.
    //
    // **Both candidates are excluded by identity now.** A player's arrow skips
    // the player -- there is one of them and `ArrowTargets::playerPresent` is
    // the handle -- and a mob's arrow skips the mob whose `Mob::handle` matches
    // `shooterMob`. Neither is a proxy, so both are the jar's reference
    // comparison exactly.
    //
    // **The mob half used to be found by place**, over `(shooterX, shooterZ)`,
    // because the pool swap-removes and there was no stable handle to compare.
    // It was close enough for a skeleton, which walks well under its own
    // half-width in a tick -- but only for a skeleton standing still enough,
    // and it broke outright for anything knocked back on the tick it fired:
    // the arrow it had just loosed found it a hair outside its recorded
    // footprint and struck it at a distance of zero. `Mob::handle` is what
    // removed the guess.
    //
    // **The player could not be found by place**, and that is why this is
    // split. The footprint test holds only while the shooter stays inside its
    // own box, and Creative flight is `kFlightSpeed` -- 0.6 a tick against a
    // half-width of 0.3. One tick of flying carried the player clear of the
    // recorded place, the exclusion missed, and the arrow, still inside the box
    // it was born in, was spent on its own archer at a distance of zero. Every
    // shot fired while flying died on the tick it was loosed, which is what
    // "an arrow does not knock a painting off the wall" turned out to be.
    //
    // Not saved -- the jar does not save `shootingEntity` either -- so an arrow
    // reloaded mid-flight has no grace left, which is what a five-tick window
    // means after a world close anyway.
    //
    // **Which mob fired it, by identity.** Zero when nobody did, which is every
    // arrow the player loosed and every arrow read back off the card. See
    // `Mob::handle`: the pool swap-removes, so this is a session handle rather
    // than a slot, and a mob that dies never lends its number to the animal
    // that takes its place.
    u32 shooterMob = 0;
    i8 shooterGrace = 0;

    void setPosition(double px, double py, double pz)
    {
        x = px;
        y = py;
        z = pz;
        const double half = kArrowSize / 2.0;
        // `yOffset` is 0 for an arrow, so the box hangs *below* the position by
        // its full height -- not centred on it as a particle's is.
        box = AABB{px - half, py, pz - half, px + half, py + kArrowSize, pz + half};
    }
};

// The pools an arrow may strike, any of which may be absent.
struct ArrowTargets {
    PaintingSystem* paintings = nullptr;
    BoatSystem* boats = nullptr;
    MinecartSystem* minecarts = nullptr;

    // **The mobs**, which is what the bow was for. An arrow does
    // `attackEntityFrom(shootingEntity, 4)` with knockback, exactly as a punch
    // does -- so an arrow shears a sheep, because the shooter is an
    // `EntityLiving`.
    MobSystem* mobs = nullptr;

    // **The player**, as a box and a place to send the damage. The same shape
    // `MobSurroundings::hurtPlayer` takes and for the same reason -- see
    // core/entity/mob.hpp -- and the source is always `DamageSource::Arrow`,
    // which difficulty scales whoever fired it.
    //
    // **A skeleton's arrow can hit the skeleton beside it**, and does: `kg.e_()`
    // skips only the entity that fired it, and only while the arrow is under
    // five ticks old. That is where a1.1.2's skeletons-shooting-each-other
    // comes from, and it is reproduced rather than special-cased. A skeleton's
    // arrow owes the player no grace at all, for the same reason: the player
    // did not fire it.
    bool playerPresent = false;
    AABB playerBox{};
    void (*hurtPlayer)(void* ctx, int amount, DamageSource source, double fromX,
                       double fromZ) = nullptr;
    void* hurtPlayerCtx = nullptr;
};

// **No cap**, as the original has none. The first 128 are held from
// construction, so ordinary play never allocates; past that the pool grows a
// segment at a time until the heap says stop -- see core/util/segmented_pool.hpp.
class ArrowSystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 128;


    explicit ArrowSystem(i64 seed) : rand_(seed) {}

    // `jg`'s spawn: `new kg(world, player)`, whose constructor takes the
    // shooter's position and angles and does the rest. `eyeX/Y/Z` is the
    // shooter's position as `Entity` stores it -- y is the eye -- and the
    // angles are degrees.
    //
    // Returns false when the heap would not hold another arrow, in which case
    // nothing was fired.
    bool shoot(const tick::TickWorld& world, double eyeX, double eyeY, double eyeZ,
               float yawDegrees, float pitchDegrees);

    // `cw.a(Lkh;F)V`'s spawn: the arrow is placed by the caller -- which has
    // already applied the constructor's muzzle offset and the skeleton's own
    // 1.4 lift -- and then given a heading outright, which **replaces**
    // whatever the constructor's angles produced.
    //
    // `velocity` is 0.6 and `inaccuracy` 12.0 for a skeleton, against the
    // player's 1.5 and 1.0: a skeleton's arrow is slower and far less accurate,
    // which is why one at range misses and one at three blocks does not.
    //
    // `shooterMob` is the firing mob's `Mob::handle`, which is what keeps the
    // arrow off its own archer for five ticks. Zero means nobody, and a shot
    // with no shooter is a target for everything from the tick it is loosed.
    bool shootFrom(const tick::TickWorld& world, double x, double y, double z, double dx,
                   double dy, double dz, float velocity, float inaccuracy,
                   ArrowShooter shooter, u32 shooterMob = 0);

    // One 20 Hz tick of `kg.e_()` for every live arrow.
    // **Mutable**, since an arrow can now kill a mob and a mob's death drops
    // items and -- for a creeper -- is the one path that writes a music disc.
    void tick(tick::TickWorld& world, const ArrowTargets& targets = ArrowTargets{});

    void clear() { arrows_.clear(); }

    int count() const { return arrows_.size(); }
    const Arrow& operator[](int i) const { return arrows_[i]; }

    u32 refused() const { return refused_; }

private:
    friend struct PersistentEntities;

    // `random.drr`, played at the arrow by both of `kg.e_()`'s strike sites.
    void playStruck(const tick::TickWorld& world, const Arrow& a);

    SegmentedPool<Arrow, kInitialCapacity> arrows_;
    u32 refused_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
