#pragma once

// **A dropped item lying in the world** -- `dx`, which is `EntityItem`, and the
// first entity in this project that is not a particle.
//
// Everything that dropped something before this deleted it. `destroyBlock`
// removed a block and left nothing behind; `Inventory::dropOne` took one off
// the hand and the item was simply gone. The gap was named in both places and
// this closes it: a drop is an `EntityItem` now, it falls, it bounces, it
// slides, it waits, and walking over it puts it back.
//
// **What is a1.1.2's and what is ours.** The entity is a transcription --
// `dx.<init>`, `dx.e_()` (onUpdate), `dx.b(dm)` (onCollideWithPlayer) and
// `dm.a(Lev;Z)V` (dropPlayerItemWithRandomChoice) are all here, constants and
// order included. What is *not* a1.1.2's is the button: this version has no
// drop key at all -- `dropOneItem` does not exist in it, and the only callers
// of `dropPlayerItem` in the jar are the inventory screens spilling the stack
// on the cursor. So the mechanism is the game's and the trigger is Creative's,
// the same split Creative itself is.
//
// **It moves with `moveEntity`**, through the shared sweep in
// core/entity/sweep.hpp -- the same one the player and the digging particles
// use. That is why an item lands on a slab, slides down a slope and stops in a
// corner without a line of code here saying so.
//
// **One thing the original does that this does not**, named rather than hidden:
// a block broken by hand still drops nothing. That needs `idDropped` and
// `quantityDropped` off every block -- a table, and a derivation of its own --
// rather than an entity, which is what this is.
//
// `handleWaterMovement` used to be on that list. It is not any more: the flow
// field is core/block/fluid_flow.hpp now and an item in a river drifts
// downstream, the same push the player gets.
//
// **Two things here are openly not a1.1.2's**, and they are the second half of
// the split the drop button already is:
//
//   * **Items on the ground merge.** `dx.e_()` in this version has no such
//     step -- ground merging arrives with Beta 1.8's `combineItems` -- so two
//     heaps of dirt thrown side by side stay two heaps for the five minutes
//     they live, and a floor covered in drops is 64 entities that all have to
//     be walked over one at a time. That is a1.1.2's, and it is the sort of
//     thing this project keeps; it is *also* the thing the pool ceiling below
//     makes expensive, and it was asked for. So `combineItems` is transcribed
//     from the later version and marked, rather than invented: the bigger
//     stack absorbs the smaller, the survivor keeps the *younger* age and the
//     *longer* pickup delay, and the scan runs every 25 ticks as it does
//     there. See kItemMergeInterval.
//   * **An item outside a loaded chunk does not tick at all**, so its five
//     minutes do not run while nobody is there. a1.1.2 has no such rule for
//     the trivial reason that its entities live *in* chunks and an unloaded
//     chunk has none of them resident to tick; ours are a flat pool that
//     outlives the columns under it, so the rule has to be written down. The
//     effect is the one the original has and this did not: walking away from a
//     dropped item and coming back finds it still there.
//
// Drawing is elsewhere, as it is for particles: core/render/item_entity_mesh.hpp
// turns one of these into quads and this file has never heard of a camera.

#include "core/item/item_def.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}
namespace mc::item {
struct Inventory;
}

namespace mc::entity {

// `setSize(0.25F, 0.25F)`, and `yOffset = height / 2` -- so the position is the
// **centre** of the box, exactly as it is for a particle, and the box is a pure
// function of the position rather than a thing stored beside it.
inline constexpr double kItemSize = 0.25;
inline constexpr double kItemHalf = kItemSize / 2.0;

// `dx.e_()`'s own constants: the pull per tick, the drag on y, the drag on the
// two horizontal axes in the air, and the extra bounce on landing.
inline constexpr double kItemPull = 0.03999999910593033;
inline constexpr double kItemVerticalDrag = 0.9800000190734863;
inline constexpr float kItemAirDrag = 0.98f;

// `0.58800006F`, which is `0.6f * 0.98f` as the class file stores it -- the
// ground drag used when the block underneath is *air*, which happens for one
// tick after a slab or a fence has been walked off. A real block underfoot uses
// its own slipperiness times 0.98 instead.
inline constexpr float kItemGroundDrag = 0.58800006f;

// **6000 ticks, which is five minutes**, and it is a hard `>=` in the original
// rather than a fade. The clock only advances on a tick the item actually
// takes, and it only takes one while the column under it is resident -- see
// the header.
inline constexpr int kItemMaxAge = 6000;

// `EntityItem.onUpdate`'s `age % 25 == 0` in the version the merge comes from.
// Every tick would be 64 x 64 box tests at 20 Hz for nothing: two items that
// land together are still two items for at most a second and a quarter, which
// is what the original looks like.
inline constexpr int kItemMergeInterval = 25;

// `boundingBox.expand(0.5, 0.0, 0.5)` -- the box a merge looks in. Horizontal
// only, so a heap on a slab does not swallow the one on the floor beneath it.
inline constexpr double kItemMergeReach = 0.5;

// `kh.G()` -- isInLava -- shrinks the box by this much top and bottom before
// asking, exactly as `kh.g_()` does for water. The same number for both, which
// is what reading the two methods side by side settles.
inline constexpr double kLavaProbeInset = -0.4000000059604645;

// `if (posY < -64.0D) setDead()`, the last line of `Entity.onEntityUpdate`.
inline constexpr double kVoidFloor = -64.0;

// `delayBeforeCanPickup`. The constructor sets 5 and a player's own drop
// overrides it with 40, which is the two seconds that stop a dropped item
// jumping straight back into the hand.
inline constexpr int kItemPickupDelay = 5;
inline constexpr int kItemDropPickupDelay = 40;

// `Block.dropBlockAsItemWithChance` overrides it with 10, which is half a
// second -- long enough that walking through a falling torch does not pick it
// up before it has landed, and short enough that you do not wait for it.
inline constexpr int kBlockDropPickupDelay = 10;

struct ItemEntity {
    // **The box is the authority and the position is derived from it**, for
    // exactly the reason `Particle` gives at length: rebuilding the box from a
    // rounded centre each tick puts its bottom a fraction of an ulp below the
    // block it landed on, and `calculateYOffset` then declines to stop it.
    AABB box{};

    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;

    // What is in it. An id and a count, as the save format has it; the damage
    // rides along so a worn tool that is dropped and picked up again is the
    // same tool.
    item::ItemId item = 0;
    int count = 0;
    i16 damage = 0;

    // `age`, `delayBeforeCanPickup`, and `hoverStart` -- the last being a
    // random phase in radians so a heap of items does not bob in lockstep.
    int age = 0;
    int pickupDelay = 0;
    float hoverPhase = 0.0f;

    // `rotationYaw`, random at spawn. The renderer spins the item from this.
    float yaw = 0.0f;

    // `(sky << 4) | block` where it is, resampled every tick as the original
    // resamples it every frame.
    u8 light = 0;

    bool onGround = false;

    bool alive() const { return count > 0; }

    // `Entity.setPosition` for a box whose `yOffset` is half its height.
    void setPosition(double px, double py, double pz)
    {
        x = prevX = px;
        y = prevY = py;
        z = prevZ = pz;
        box = AABB{px - kItemHalf, py - kItemHalf, pz - kItemHalf,
                   px + kItemHalf, py + kItemHalf, pz + kItemHalf};
    }
};

// **No cap**, as the original has none. The first sixty-four are held from
// construction; past that the pool grows until the heap says stop -- see
// core/util/segmented_pool.hpp. Sixty-four was a hard ceiling once, and a
// Creative player mining at a few blocks a second filled it well inside the
// five minutes an item lives.
//
// **When the heap says stop, a drop replaces the oldest item**, the one closest
// to despawning anyway, rather than silently never appearing -- a refused drop
// was the only path in this code to "minecarts drop nothing when killed by
// arrows". A refusal is right only where it loses nothing, and that is a
// player's throw -- the item stays in the hand -- so `dropFromPlayer` still
// refuses.
class ItemEntitySystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 64;

    explicit ItemEntitySystem(i64 seed) : rand_(seed) {}

    // `dm.a(Lev;Z)V` with `flag` false -- **dropPlayerItemWithRandomChoice**,
    // the throw a player's own drop makes. `eyeX/Y/Z` is the player's position
    // as `Entity` stores it (y is the eye), and the angles are degrees.
    //
    // Returns false when the heap would not hold another item, in which case
    // nothing was spawned and the caller still holds the item -- which is why
    // `dropOne` runs after this and not before. This is the one spawn that
    // refuses rather than evicts; see the class note.
    bool dropFromPlayer(const tick::TickWorld& world, double eyeX, double eyeY, double eyeZ,
                        float yawDegrees, float pitchDegrees, item::ItemId id, int count,
                        i16 damage);

    // `dx.<init>` on its own, for a drop that is not thrown by anybody: the
    // upward hop and the small horizontal scatter, and nothing else.
    // `pickupDelay` is the constructor's 5 unless the caller says otherwise;
    // a block's own drop passes `kBlockDropPickupDelay`. When the heap will
    // not hold another it replaces the oldest item; false only for an empty
    // stack.
    bool spawn(const tick::TickWorld& world, double px, double py, double pz, item::ItemId id,
               int count, i16 damage, int pickupDelay = kItemPickupDelay);

    // One 20 Hz tick of `dx.e_()` for every live item. Ages them out at 6000,
    // merges the ones lying on top of each other, and skips entirely any whose
    // column is not resident.
    void tick(const tick::TickWorld& world);

    // `dx.b(dm)` -- **onCollideWithPlayer**, driven from the player's side
    // because that is where the box is. `playerBox` is expanded by one block
    // horizontally before it gets here, as `EntityPlayer.onUpdate` expands it.
    //
    // Returns how many items were taken, so the caller can play `random.pop`
    // once per pickup without this having a sound engine.
    int collect(const AABB& playerBox, item::Inventory& inventory);

    void clear() { items_.clear(); }

    int count() const { return items_.size(); }
    const ItemEntity& operator[](int i) const { return items_[i]; }

    // Spawns the pool has refused. On the debug page for the same reason the
    // particle pool's is.
    u32 refused() const { return refused_; }

    // Items a drop replaced because the heap would not hold another.
    u32 evicted() const { return evicted_; }

private:
    friend struct PersistentEntities;
    ItemEntity* allocate(bool mayEvict);

    // `spawn`, answering where the item went. `mayEvict` false is
    // `dropFromPlayer`'s refusal.
    ItemEntity* place(const tick::TickWorld& world, double px, double py, double pz,
                      item::ItemId id, int count, i16 damage, int pickupDelay, bool mayEvict);
    void removeAt(int index);

    // `combineItems`, for the pair (a, b). Answers true when one of them was
    // emptied -- which one is the method's own rule, not the caller's, so this
    // says nothing about the order it was handed them in.
    //
    // **A merge empties a stack rather than removing it**, because `tick` is
    // walking the pool while this runs and `removeAt` swaps the last entry
    // into the hole. The sweep at the end of the tick is what actually
    // reclaims them, which is `setDead()` and `World.releaseEntitySkin` doing
    // the same two jobs in the same order.
    bool combine(ItemEntity& a, ItemEntity& b);

    SegmentedPool<ItemEntity, kInitialCapacity> items_;
    u32 refused_ = 0;
    u32 evicted_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
