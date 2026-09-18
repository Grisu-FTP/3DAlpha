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
// **Dropped items never merge**, and that is the original's behaviour rather
// than an omission. `dx.e_()` has no such step, and neither does any version
// for the next two years: the whole of Alpha, the whole of Beta and release
// 1.2.5 leave two heaps of dirt thrown side by side as two heaps for the five
// minutes they live. `EntityItem.combineItems` is first there in **1.3.1**,
// where `onUpdate` walks the entities in `boundingBox.expand(0.5, 0, 0.5)`
// every tick; the `age % 25` throttle on that scan is later still (it is there
// by 1.8.9). The bisect that gives that is absent-in-1.2.5, present-in-1.3.1
// -- the 2012 snapshots between the two are not in the version manifest it
// used, so it is a release bound and not a snapshot one. Measured, not
// remembered: docs/status.md records the jars and the bytecode.
//
// This file did once carry 1.3.1's `combineItems`, transcribed and marked as a
// deviation. It does not any more: a floor covered in drops is 64 separate
// entities to walk over, which is what a1.1.2 looks like, and that is the
// thing this project keeps. The pool ceiling below is what pays for it.
//
// **One thing here is openly not a1.1.2's**, and it is the other half of the
// split the drop button already is:
//
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

#include "core/entity/water_entry.hpp"
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

// `kh.G()` -- isInLava -- shrinks the box by this much top and bottom before
// asking, exactly as `kh.g_()` does for water. The same number for both, which
// is what reading the two methods side by side settles.
inline constexpr double kLavaProbeInset = -0.4000000059604645;

// **`dx`'s health, which the constructor sets to 5** -- and it is the only
// health in the version that is not a living thing's. `dx.a(Lkh;I)Z` subtracts
// the damage and calls `setDead()` at or below zero, with no invulnerability
// window of any kind, so five one-point hits in five consecutive ticks kill a
// stack. That is exactly what standing in fire does to it.
inline constexpr i16 kItemHealth = 5;

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

// `je`, from core/entity/explosion.hpp. Only `takeBlast` names it.
class Explosion;

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

    // **The server's id for this item, and zero when there is no server.**
    // Every `kh` in a1.1.2 carries an `entityId` in both kinds of game. A
    // client only ever *reads* it -- every packet that moves or removes an item
    // names it by this and nothing else -- and a console hosting a session
    // writes it, stamping one onto each item as it tells the guests about it
    // (core/net/world_server.hpp). Not saved: it belongs to the session, not to
    // the world. See core/net/entities.hpp.
    i32 entityId = 0;

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

    // `kh`'s `aV` and `c`, which is all a splash needs. Not saved: the jar
    // does not save it either, and a stack reloaded in a river should not
    // splash on the tick the world opens.
    WaterEntry water{};

    // **`dx.f` -- the five points of health a dropped stack has**, and `kh.aT`,
    // the fire counter every entity carries. The two together are what makes a
    // stack thrown into a fire burn up: `moveEntity`'s tail deals one point a
    // tick and `dx.a(Lkh;I)Z` calls `setDead()` at zero, so five ticks in a
    // flame is the end of it. See core/entity/fire_entry.hpp.
    i16 health = kItemHealth;
    i16 fire = 0;

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

    // `dm.a(Lev;Z)V` with `flag` true -- **the scatter a death drops the
    // inventory with**: a random heading, up to half a block a tick of it, a
    // fixed 0.2 upward, and the same two-second pickup delay as a throw. The
    // draws are the *player's* generator (`aQ`), which is why it is passed in.
    //
    // **Evicts rather than refuses**, unlike a throw: the slot is emptied
    // whether or not the item found room, so a refusal here would be the loss
    // the throw's refusal exists to avoid.
    bool dropOnDeath(const tick::TickWorld& world, double eyeX, double eyeY, double eyeZ,
                     item::ItemId id, int count, i16 damage, JavaRandom& thrower);

    // **A stack put into the world with a velocity chosen for it** -- a broken
    // chest's spill, which builds the entity and then overwrites its motion.
    // Evicts rather than refuses, for the death drop's reason: the stack has
    // already left the chest.
    bool spawnMoving(const tick::TickWorld& world, double px, double py, double pz,
                     item::ItemId id, int count, i16 damage, double motionX, double motionY,
                     double motionZ);

    // `dx.<init>` on its own, for a drop that is not thrown by anybody: the
    // upward hop and the small horizontal scatter, and nothing else.
    // `pickupDelay` is the constructor's 5 unless the caller says otherwise;
    // a block's own drop passes `kBlockDropPickupDelay`. When the heap will
    // not hold another it replaces the oldest item; false only for an empty
    // stack.
    bool spawn(const tick::TickWorld& world, double px, double py, double pz, item::ItemId id,
               int count, i16 damage, int pickupDelay = kItemPickupDelay);

    // One 20 Hz tick of `dx.e_()` for every live item. Ages them out at 6000
    // and skips entirely any whose column is not resident. Two items lying on
    // top of each other stay two: see the header.
    void tick(const tick::TickWorld& world);

    // `dx.b(dm)` -- **onCollideWithPlayer**, driven from the player's side
    // because that is where the box is. `playerBox` is expanded by one block
    // horizontally before it gets here, as `EntityPlayer.onUpdate` expands it.
    //
    // Returns how many items were taken, so the caller can play `random.pop`
    // once per pickup without this having a sound engine.
    int collect(const AABB& playerBox, item::Inventory& inventory);

    // **`je`'s middle phase for every stack in reach.** The blast calls
    // `kh.a(Lkh;I)Z` on each entity in its box and `dx`'s override is the
    // subtraction `hurt` makes, so a stack takes the same damage a mob would
    // and five points ends it -- anything close to a creeper is gone. The
    // impulse is added whether or not it survived, as `je` adds it. Dead
    // stacks are swept before returning, since a blast never runs inside
    // `tick`'s walk.
    void takeBlast(const tick::TickWorld& world, const Explosion& blast);

    // ---- items a server owns -------------------------------------------
    //
    // **Spawned, moved and removed by id, and never picked up locally.** The
    // server decides who picks up what -- it says so with Collect and Add To
    // Inventory -- so one of these carries a pickup delay long enough that no
    // local path can take it even if one is asked to. It falls and bobs like
    // any other item, because `EntityItem` on a real client does too; the
    // server's position updates correct it.
    ItemEntity* spawnFromServer(const tick::TickWorld& world, i32 entityId, double px,
                                double py, double pz, item::ItemId id, int count, i16 damage,
                                double motionX, double motionY, double motionZ);

    ItemEntity* findById(i32 entityId);

    // **One item by position in the pool, to be written to.** The const
    // `operator[]` below is what the renderer and the tick use; this is for the
    // host of a session, which has to stamp a server id onto an item the
    // single-player game spawned without one. See `ItemEntity::entityId`.
    ItemEntity* at(int i) { return &items_[i]; }

    bool removeById(i32 entityId);
    bool placeById(i32 entityId, double px, double py, double pz);

    // **Every stack in the pool that no server owns**, removed, and how many
    // that was. In a session the pool holds two kinds of item at once: the
    // ones this console threw, which are the server's to make, and the ones
    // the server has already made, which carry its id. A client hands over the
    // first kind and must not touch the second -- handing back what the server
    // just sent is how one dropped stack becomes an endless supply of them.
    // See `NetPlay::forwardDrops`.
    int removeUnowned();

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

    SegmentedPool<ItemEntity, kInitialCapacity> items_;
    u32 refused_ = 0;
    u32 evicted_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
