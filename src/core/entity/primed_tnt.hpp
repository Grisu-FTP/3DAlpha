#pragma once

// **`jd` -- EntityTNTPrimed**, the thing TNT becomes between being lit and
// going off, and the last piece of TNT this port was missing. Without it a
// `q` (BlockTNT) had nowhere to be primed *into*, so every one of its three
// ignition paths ended in a comment naming the gap:
// `core/tick/drop.cpp`'s `blockDestroyedByPlayer`, `core/tick/fire.cpp`'s
// `tryToCatch`, and `core/entity/explosion.cpp`'s phase 3.
//
// **Breaking TNT in a1.1.2 lights it, and that is not a bug here.** `q`'s
// `a(Ljava/util/Random;)I` -- quantityDropped -- returns a hard 0, and
// `q.b(Lcn;IIII)V` -- onBlockDestroyedByPlayer -- has no metadata guard at all
// in this version: it builds a `jd` at the cell's centre, spawns it and plays
// `random.fuse`. The `if (metadata == 1)` that makes a mined block drop
// itself arrives later. So a1.1.2 has exactly one way to *obtain* TNT --
// crafting it -- and a pickaxe to the face is a fuse.
//
// The tick, transcribed from `jd.e_()`:
//
// ```
// prevPos = pos;
// motionY -= 0.04;
// moveEntity(motionX, motionY, motionZ);
// motionX *= 0.98; motionY *= 0.98; motionZ *= 0.98;
// if (onGround) { motionX *= 0.7; motionZ *= 0.7; motionY *= -0.5; }
// if (fuse-- <= 0) { setDead(); explode(); }
// else world.spawnParticle("smoke", posX, posY + 0.5, posZ, 0, 0, 0);
// ```
//
// Four things in it are worth naming:
//
//   * **The fuse test reads the value before the decrement** -- `if (fuse-- <=
//     0)`, which is `dup_x1` in the class file and not a `- 1` anywhere. A
//     fuse of 80 therefore burns for **81 ticks**, not 80: eighty of them
//     smoke and the eighty-first is the blast.
//   * **It never calls `super.onUpdate()`.** `jd.e_()`'s first instruction is
//     the `prevPosX` store, so `kh.y()` never runs -- no splash, no fire, no
//     drowning. TNT thrown into a river is silent, which is the same answer
//     `ff`, `jc` and `oc` give and for the same reason. See
//     `docs/audio-a1.1.2.md` *What an entity plays*.
//   * **The landing drag is applied every tick it is on the ground**, not once
//     on the tick it arrives. TNT that lands on a slope therefore creeps
//     rather than sliding, and a fresh one dropped on flat ground is nearly
//     still by the time it goes off.
//   * **The smoke comes out half a block above the entity's position**, and
//     the position is the box's centre plus nothing -- `yOffset` is
//     `height / 2`, so `posY` is already the middle of the cube and the puff
//     sits at its top face.
//
// **The scatter is `Math.random()` and not the world's generator.** The
// constructor draws one angle from the shared JDK generator, so lighting TNT
// does not shift the block-tick stream the way a drop does. This pool owns a
// time-seeded generator for it, exactly as `ItemEntitySystem` owns one for a
// thrown stack's scatter -- and for the same reason: nothing about the hop is
// meant to repeat.
//
// **Its own blast is 4.0**, which is `je`'s largest caller in this version and
// reaches five blocks, where a creeper's 3.0 reaches 3.7. The cell record in
// `explosion.hpp` is sized for eight and says so.
//
// **What it does not do**, because a1.1.2 does not: it has no
// `onBlockDestroyedByExplosion` chain of its own beyond re-priming (a blast
// lights nearby TNT with a 10..29-tick fuse rather than setting it off, which
// is what makes a chain ripple instead of being one big bang), it is not
// flammable, it takes no damage, and nothing rides it. The fourth ignition
// path -- redstone, `q.a(Lcn;IIIII)V` -- is the only one still missing here,
// and it is missing because power propagation is, not because this is.

#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

class MobSystem;
struct MobSurroundings;

// `setSize(0.98F, 0.98F)`, and `aB = aD / 2` -- `yOffset` is half the height,
// so the box is centred on the position on all three axes. The same shape a
// falling block has, and the same two doubles.
inline constexpr double kPrimedTntSize = 0.98000001907348633;   // (double)0.98f
inline constexpr double kPrimedTntHalf = kPrimedTntSize / 2.0;

// `motionY -= 0.04` and `*= 0.98`, both the class file's own widened literals.
inline constexpr double kPrimedTntPull = 0.03999999910593033;
inline constexpr double kPrimedTntDrag = 0.9800000190734863;

// The three factors on a tick that ends on the ground. Unlike `ff`'s, these
// are **not** cosmetic: this entity outlives its landing, so the bounce and
// the friction are what it actually settles by.
inline constexpr double kPrimedTntLandDrag = 0.699999988079071;
inline constexpr double kPrimedTntBounce = -0.5;

// The hop: `motionY = 0.20000000298023224` flat, and `0.02` of a unit circle
// spread horizontally. Note the angle is drawn in radians and then converted
// *again* -- `MathHelper.sin(f * PI / 180)` where `f` is already
// `Math.random() * PI * 2` -- so the real spread is a 0.11-radian arc and not
// a full circle. That is the class file's, quirk and all; see the note on
// `PrimedTntSystem::spawn`.
inline constexpr double kPrimedTntLaunch = 0.20000000298023224;
inline constexpr float kPrimedTntScatter = 0.02f;

// `this.fuse = 80`, four seconds, and `q.c`'s re-prime is a quarter and an
// eighth of it: `nextInt(80 / 4) + 80 / 8`.
inline constexpr int kPrimedTntFuse = 80;
inline constexpr int kPrimedTntRelitSpread = kPrimedTntFuse / 4;
inline constexpr int kPrimedTntRelitFloor = kPrimedTntFuse / 8;

// `float f = 4.0F;` in `jd.i()`.
inline constexpr float kPrimedTntStrength = 4.0f;

struct PrimedTnt {
    // The box is the authority and the position is derived from it, for the
    // reason `ItemEntity` gives at length.
    AABB box{};

    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;

    // `jd.a`, the only field the class declares and the only one it saves.
    int fuse = 0;

    // `(sky << 4) | block` where it is, resampled every tick as the falling
    // blocks resample theirs.
    u8 light = 0;
    // `kh.aT` -- the fire counter. See core/entity/fire_entry.hpp.
    i16 fire = 0;

    bool onGround = false;
    bool active = false;

    bool alive() const { return active; }

    void setPosition(double px, double py, double pz)
    {
        x = prevX = px;
        y = prevY = py;
        z = prevZ = pz;
        box = AABB{px - kPrimedTntHalf, py - kPrimedTntHalf, pz - kPrimedTntHalf,
                   px + kPrimedTntHalf, py + kPrimedTntHalf, pz + kPrimedTntHalf};
    }
};

// **No cap**, as a1.1.2 has none and as a chain reaction needs none: a wall of
// TNT lights one entity per cell and they go off over a second or two. The
// first sixteen are held from construction and past that the pool grows until
// the heap says stop; only then is a spawn refused and counted, and a refused
// prime is a block that simply vanished -- the cell is already air by the time
// anything asks for the entity.
class PrimedTntSystem {
public:
    static constexpr int kInitialCapacity = 16;

    // The seed is the hop's, and only the hop's. See the header.
    explicit PrimedTntSystem(i64 seed) : rand_(seed) {}

    // `new jd(world, i + 0.5F, j + 0.5F, k + 0.5F)` with the fuse the caller
    // wants: 80 for a block broken or burnt, `nextInt(20) + 10` for one lit by
    // a blast -- and **that draw belongs to the caller**, because it comes off
    // `world.rand` in `q.c(Lcn;III)V` and the ordering of the world's
    // generator is observable.
    bool spawn(const tick::TickWorld& world, i32 x, int y, i32 z,
               int fuse = kPrimedTntFuse);

    // One 20 Hz tick of `jd.e_()` for every live entity.
    //
    // **Takes a mutable world** for the same reason `FallingBlockSystem::tick`
    // does: the last tick of a fuse writes blocks, over a five-block radius.
    //
    // `mobs` and `around` are `je`'s middle phase -- who the blast hurts. Both
    // unset is a blast that moves blocks and nothing else, which is what every
    // headless caller wants and what this port did before there was anything
    // to light. See `applyBlast` in mob.hpp.
    void tick(tick::TickWorld& world, MobSystem* mobs = nullptr,
              const MobSurroundings* around = nullptr);

    void clear() { items_.clear(); }

    int count() const { return items_.size(); }
    const PrimedTnt& operator[](int i) const { return items_[i]; }

    // Spawns the pool has refused, for the debug page.
    u32 refused() const { return refused_; }

private:
    friend struct PersistentEntities;
    PrimedTnt* allocate();
    void removeAt(int index);

    SegmentedPool<PrimedTnt, kInitialCapacity> items_;
    JavaRandom rand_;
    u32 refused_ = 0;
};

}  // namespace mc::entity
