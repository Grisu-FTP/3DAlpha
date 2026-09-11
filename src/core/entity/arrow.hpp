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
// second arrow inside two seconds breaks it. The player is a target in the
// original too, once the arrow is five ticks old; with no health to take that
// is left out rather than made to do nothing. Mobs do not exist yet.
//
// Drawing is elsewhere, as ever: core/render/arrow_mesh.hpp is `gk`.

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
    int ticksInGround = 0;
    int ticksInAir = 0;

    u8 light = 0;
    bool alive = false;

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

    // One 20 Hz tick of `kg.e_()` for every live arrow.
    void tick(const tick::TickWorld& world, const ArrowTargets& targets = ArrowTargets{});

    void clear() { arrows_.clear(); }

    int count() const { return arrows_.size(); }
    const Arrow& operator[](int i) const { return arrows_[i]; }

    u32 refused() const { return refused_; }

    // **How many arrows struck something this tick**, so the caller can play
    // `random.drr` without this file owning a sound engine -- the same seam
    // `ItemEntitySystem::collect` uses for `random.pop`.
    int struckLastTick() const { return struck_; }

private:
    friend struct PersistentEntities;
    SegmentedPool<Arrow, kInitialCapacity> arrows_;
    u32 refused_ = 0;
    int struck_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
