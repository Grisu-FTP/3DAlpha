#pragma once

// **A boat** -- `dc`, which is `EntityBoat`.
//
// "Boats and minecarts don't work (not even placeable)" was the `spawns` bug
// again, and for the boat there is a second half to it: `me.a(...)` --
// ItemBoat.onItemRightClick -- is not an `onItemUse` at all. It casts **its own
// ray, at reach 5.0, with liquids on**, and spawns at
// `(hit.x + 0.5, hit.y + 1.5, hit.z + 0.5)`. That is exactly the shape a bucket
// has, and it is why a boat cannot be placed by the block path: the crosshair's
// ray is not allowed to see water at all, and water is the only place a boat is
// any use.
//
// **What a boat is, mechanically**, is four things and none of them is a
// vehicle in the usual sense:
//
//   * **Buoyancy by slice.** The box is cut into five horizontal slices and
//     each is asked whether it is in water; the fraction that is, doubled and
//     less one, scales a 0.04 push. So a boat entirely under water rises at
//     0.04 a tick, one entirely out of it falls at 0.04, and one exactly half
//     in floats. That is the whole of it -- there is no water level anywhere.
//   * **Steering by the rider's motion.** `motionX += riddenByEntity.motionX * 0.2`,
//     and nothing else. `EntityLiving` never checks whether it is riding, so a
//     player in a boat keeps turning the movement keys into motion exactly as
//     they would on foot and only their *position* is overwritten. See
//     core/entity/rider.hpp.
//   * **A hard 0.4 cap** on each horizontal axis, applied before the move --
//     which is what makes a boat feel like a boat rather than like a sled.
//   * **It breaks on impact.** Moving faster than 0.15 into a wall kills it and
//     drops three planks and two sticks.
//
// **It uses `moveEntity`**, unlike the arrow: the shared sweep in
// core/entity/sweep.hpp is what makes a boat ride up onto a beach and stop
// against a cliff.
//
// **Riding is the first thing in this project that takes the camera off the
// player body**, and that half is deliberately not here: this file hands out a
// `RiderSeat` and knows nothing about a `PlayerBody`, a camera or a button.
//
// The pool survives world saves through core/entity/persistence.hpp. One thing
// is openly not a1.1.2's:
//
//   * **The splash particles are the caller's.** `onUpdate` spawns up to
//     `1 + speed * 60` of them a tick and this file owns no particle pool --
//     the same split `EntityArrow`'s bubbles take.

#include "core/entity/rider.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `setSize(1.5F, 0.6F)`, and `yOffset = height / 2.0F` -- so a boat's position
// is half a boat above its own floor.
//
// **The halving is in float**, for the reason core/entity/minecart.hpp gives at
// length: a box bottom rebuilt as `y + yOffset - yOffset` lands exactly on the
// block below only when the constant is the float one. It costs nothing to get
// right and a boat run aground would otherwise stick.
inline constexpr double kBoatWidth = double(1.5f);
inline constexpr double kBoatHeight = double(0.6f);
inline constexpr double kBoatYOffset = double(0.6f / 2.0f);

// `getMountedYOffset()` is `height * 0.0D - 0.30000001192092896D`, which is a
// constant with the height multiplied out by zero -- so it is **-0.3**, and the
// zero is the class file's own, not a simplification here.
inline constexpr double kBoatMountedYOffset = -0.30000001192092896;

// `me.a(...)`'s reach, which is the bucket's 5.0 and not the hand's 4.0, and it
// rays **with liquids** -- which is the whole reason a boat cannot go down the
// block-placement path.
inline constexpr double kBoatReach = 5.0;

// How many horizontal slices the buoyancy test cuts the box into, and the
// 0.125 it drops each slice by before asking.
inline constexpr int kBoatBuoyancySlices = 5;
inline constexpr double kBoatBuoyancyDrop = 0.125;

// `0.03999999910593033D * d3`, where d3 runs -1 to 1 with how much of the boat
// is under water.
inline constexpr double kBoatBuoyancy = 0.03999999910593033;

// `motionX += riddenByEntity.motionX * 0.2D`.
inline constexpr double kBoatRiderPush = 0.2;

// The hard cap on each horizontal axis, applied before the move.
inline constexpr double kBoatSpeedCap = 0.4;

// `if (onGround) { motion *= 0.5 }` -- a boat on land is halved every tick,
// which is what stops one sliding across a beach for ever.
inline constexpr double kBoatGroundDrag = 0.5;

// The three drags applied after the move, and y is the odd one out.
inline constexpr double kBoatDragX = 0.9900000095367432;
inline constexpr double kBoatDragY = 0.949999988079071;
inline constexpr double kBoatDragZ = 0.9900000095367432;

// Faster than this into a wall and the boat breaks; faster than this at all and
// it throws spray.
inline constexpr double kBoatBreakSpeed = 0.15;


// The yaw chase at the end of `onUpdate`: at most twenty degrees a tick towards
// the direction it is actually travelling, and only once it has moved more than
// a thousandth of a block.
inline constexpr double kBoatTurnLimit = 20.0;
inline constexpr double kBoatTurnThreshold = 0.001;

// What a broken boat leaves -- `for (0..2) dropItemWithOffset(Block.planks, 1, 0F)`
// then `for (0..1) dropItemWithOffset(Item.stick, 1, 0F)`. The ids come out of
// the generated tables; only the counts are here.
inline constexpr int kBoatPlanksDropped = 3;
inline constexpr int kBoatSticksDropped = 2;

struct Boat {
    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;
    AABB box{};

    // Degrees. Pitch is forced to zero every tick by `onUpdate` -- a boat in
    // this version never tilts.
    float yaw = 0.0f;
    float prevYaw = 0.0f;

    // `damage`, `timeSinceHit` and `forwardDirection`. The renderer rocks the
    // hull by the first two and `forwardDirection` flips the sign; see the
    // header for why they never move yet.
    int damage = 0;
    int timeSinceHit = 0;
    int forwardDirection = 1;

    bool onGround = false;
    bool hitWall = false;
    bool ridden = false;

    u8 light = 0;
    bool alive = false;

    // `Entity.setPosition` for a box whose `yOffset` is half its height.
    void setPosition(double px, double py, double pz);
};

// **No cap**, as the original has none. Boats are put down one at a time by
// hand and they persist, so the first eight -- held from construction -- are
// what ordinary play needs; past that the pool grows until the heap says stop.
// See core/util/segmented_pool.hpp.
class BoatSystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 8;

    explicit BoatSystem(i64 seed) : rand_(seed) {}

    // `me.a(...)`'s spawn, given the block its own ray landed on. The +1.5 on y
    // is the original's and is what floats a boat on the surface of the cell
    // rather than inside it.
    //
    // Returns false when the heap would not hold another boat.
    bool place(const tick::TickWorld& world, i32 blockX, int blockY, i32 blockZ);

    // One 20 Hz tick of `dc.e_()` for every live boat. `rider` is what is
    // sitting in `riddenIndex()`, or absent.
    void tick(const tick::TickWorld& world, const VehicleRider& rider);

    // Which boat is being ridden, or -1.
    int riddenIndex() const { return ridden_; }

    // Where the rider should be put this tick. Invalid when nothing is aboard.
    RiderSeat seat() const;

    // Climbing in and out. `mount` refuses a boat that is already occupied,
    // which is `EntityBoat.interact`'s only rule.
    bool mount(int index);
    void dismount();

    // `dc.a(Lkh;I)Z` -- **attackEntityFrom**, and it is the whole of what
    // damages a boat: the hull flips its `forwardDirection`, `timeSinceHit`
    // goes to 10 so the renderer rocks it, and the damage counter takes
    // `amount * 10`. **Past 40 the boat breaks** and leaves the same three
    // planks and two sticks a wall does -- which is five bare-handed hits,
    // because `InventoryPlayer.getDamageVsEntity` answers 1 for an empty hand
    // and the counter sheds one a tick between them.
    //
    // Returns false only for an index that is not a live boat; a hit that does
    // not break it still returns true, as the original's does.
    bool attack(const tick::TickWorld& world, int index, int amount);

    void clear()
    {
        boats_.clear();
        ridden_ = -1;
    }

    int count() const { return boats_.size(); }
    const Boat& operator[](int i) const { return boats_[i]; }

    u32 refused() const { return refused_; }

private:
    friend struct PersistentEntities;
    void removeAt(int index);

    // The three planks and two sticks, spawned through the world's drop sink
    // exactly as a broken block's are. Both the wall and the hand reach it.
    void dropAndRemove(const tick::TickWorld& world, int index);

    SegmentedPool<Boat, kInitialCapacity> boats_;
    int ridden_ = -1;
    u32 refused_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
