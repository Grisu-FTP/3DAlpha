#pragma once

// **A minecart** -- `oc`, which is `EntityMinecart`, and the largest single
// entity in a1.1.2.
//
// "Boats and minecarts don't work (not even placeable)" was two bugs for the
// minecart and only one of them is the `spawns` bug. The other is that
// **`ItemMinecart.onItemUse` places only on rails** -- it tests the clicked
// block against `Block.rails` and returns false for everything else -- so a
// player trying one on the ground gets nothing, correctly, and the report is
// half a description of the game.
//
// **The rail physics is the whole of it.** Everything else -- gravity, drag,
// the wall bounce -- is a handful of lines; what makes a minecart a minecart is
// a hundred lines of following a track, and the shape of it is worth stating
// before reading the code:
//
//   * **The cart is snapped onto the rail's centreline every tick.** Not
//     steered towards it: `posX` and `posZ` are *assigned* from a parametric
//     point along the segment joining the rail's two connections. A cart can
//     therefore never leave a rail sideways, and does not need to be prevented
//     from doing so.
//   * **Its speed is re-pointed rather than re-computed.** The magnitude of
//     `(motionX, motionZ)` is kept and the direction is replaced by the rail's,
//     with the sign chosen by which way the cart was already going. That is why
//     a cart takes a corner at full speed.
//   * **A slope pushes with a constant `0.0078125`** -- one 128th of a block
//     per tick per tick -- added on the axis the slope descends. There is no
//     component of gravity involved; gravity is subtracted from `motionY`
//     every tick and then thrown away by the rail branch.
//   * **The vertical is a lookup, not a simulation.** `getPosOnRail` answers
//     "what height is the track here", and the cart is placed at it. A cart on
//     a slope is not falling.
//
// **Only the plain cart can be ridden**, and the other two do the other two
// things `oc.a(Ldm;)Z` does: type 1 opens its own 27-slot inventory and type 2
// takes a piece of coal and is aimed away from whoever clicked it. See
// `MinecartSystem::interact`.
//
// **The chest cart's slots do not live in `Minecart`.** The pool is a
// `SegmentedPool`, which requires a trivially destructible element, and an
// `ItemStack` is not one -- it carries the preserved-tag vector every stack in
// this project carries. They live in a side store keyed by the cart's `id`, so
// that a cart moved by the pool's swap-with-last removal keeps its contents.
//
// The pool survives world saves through core/entity/persistence.hpp. Native
// Java chunk entity import/export remain unimplemented.
//
// The rail *shapes* are not this file's -- `core/tick/rail.hpp` derives them,
// and the connection matrix below is those ten shapes written as offsets.

#include "core/entity/rider.hpp"
#include "core/item/item_def.hpp"
#include "core/item/item_stack.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `jo`'s own `a` field, which the three minecart items are constructed with.
// The numbering is the game's; a save file would store it.
enum class MinecartType : u8 {
    Rideable = 0,
    Chest = 1,
    Furnace = 2,
};

// `oc.c()` -- getSizeInventory, a flat 27. The same number a chest has, and
// deliberately the same constant is *not* shared with `world::kChestSlots`:
// one is a tile entity's and one is an entity's, and a version that changed
// either would not change both.
inline constexpr int kMinecartChestSlots = 27;

// **What one piece of coal is worth to a furnace cart** -- `fuel += 1200`,
// which at the quarter-rate burn below is four minutes of pushing.
inline constexpr int kMinecartCoalFuel = 1200;

// `setSize(0.98F, 0.7F)`, and `yOffset = height / 2.0F`.
//
// **The halving is in float and it has to be.** `Entity.setPosition` builds the
// box at `posY - yOffset` and the rail branch sets `posY` to `j + yOffset`, so
// the box's bottom is `j + yOffset - yOffset` -- and whether that is exactly
// `j` depends on which `yOffset` it is:
//
//     float:  64.0 + 0.34999999403953552 - 0.34999999403953552 == 64.0
//     double: 64.0 + 0.34999999999999998 - 0.34999999999999998 == 63.99999999999999
//
// A box bottom a fraction of an ulp *below* the block it is standing on
// overlaps that block, and `calculateOffset` then refuses to let the cart move
// at all. **A cart written with the double constant simply does not go**, which
// is what this port did until the arithmetic was checked -- the same trap
// core/entity/item_entity.hpp documents from the other direction.
inline constexpr double kMinecartWidth = double(0.98f);
inline constexpr double kMinecartHeight = double(0.7f);
inline constexpr double kMinecartYOffset = double(0.7f / 2.0f);

// `getMountedYOffset()` is `height * 0.0D - 0.3D` -- the same constant, and the
// same multiplied-out zero, that `EntityBoat` has.
inline constexpr double kMinecartMountedYOffset = -0.30000001192092896;

// Gravity, and it is **half the player's**: 0.04 rather than 0.08. Subtracted
// every tick and then discarded by the rail branch, which sets `posY` outright.
inline constexpr double kMinecartGravity = 0.03999999910593033;

// The hard cap on each horizontal axis. Same 0.4 the boat has.
inline constexpr double kMinecartSpeedCap = 0.4;

// **The port's bound, not a1.1.2's.** A cart-on-cart collision leaves the pair
// with 1.2 times the motion it had, and nothing in `oc` bounds motion -- only
// the move above. Carts that stay in contact (a stack, or a booster pair riding
// the same track at the cap) compound that every tick until the double
// overflows to infinity and then NaN, which takes the cart, its rider and the
// save with it. Past `kMinecartSpeedCap` extra motion does not move a cart any
// faster; it only decides how long a cart keeps top speed once it is clear,
// and at 10 a ridden cart still coasts at the cap for most of a minute.
inline constexpr double kMinecartMotionLimit = 10.0;

// **The slope push**: one 128th of a block per tick per tick, added on the axis
// an ascending rail descends towards. Not a component of gravity -- a constant.
inline constexpr double kMinecartSlopePush = 0.0078125;

// A rider slows the cart to three quarters of its speed for the move, and drags
// it at 0.997 instead of 0.96 afterwards -- so an occupied cart coasts much
// further than an empty one.
inline constexpr double kMinecartRiddenMove = 0.75;
inline constexpr double kMinecartRiddenDrag = 0.996999979019165;

// The drag an unoccupied cart on rails gets every tick.
inline constexpr double kMinecartRailDrag = 0.9599999785423279;

// Off the rails: halved on the ground, and 0.95 in the air.
inline constexpr double kMinecartGroundDrag = 0.5;
inline constexpr double kMinecartAirDrag = 0.949999988079071;

// How much of the height change between two ticks is fed back into the speed --
// `(before.y - after.y) * 0.05`. It is what makes a cart gain speed going down
// and lose it going up beyond what the slope push accounts for.
inline constexpr double kMinecartSlopeFeedback = 0.05;

// The furnace cart's own push, and the drags that go with it. Transcribed
// because the type exists; nothing fuels one yet.
inline constexpr double kMinecartFurnacePush = 0.04;
inline constexpr double kMinecartFurnaceDragOn = 0.800000011920929;
inline constexpr double kMinecartFurnaceDragOff = 0.8999999761581421;

// The yaw flip threshold: a turn of more than 170 degrees in a tick is read as
// the cart running backwards rather than spinning, and it flips instead.
inline constexpr double kMinecartFlipAngle = 170.0;

// A point on the track, or nothing. `getPosOnRail` answers "no rail here" with
// a null in the original and this is that null.
struct RailPoint {
    bool valid = false;
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Minecart {
    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;
    AABB box{};

    float yaw = 0.0f;
    float prevYaw = 0.0f;

    MinecartType type = MinecartType::Rideable;

    // `isFlipped`. A cart that reverses does not spin round; it flips, and this
    // is which way round it currently is.
    bool flipped = false;

    // The furnace cart's stored push and its fuel counter.
    double pushX = 0.0, pushZ = 0.0;
    int fuel = 0;

    int damage = 0;
    int timeSinceHit = 0;
    int forwardDirection = 1;

    // **A handle that survives removal.** The pool removes by swapping the
    // last element into the hole, so an index is only good until the next
    // tick -- and a container screen open on a chest cart outlives several.
    // Assigned once, from the system's own counter, and saved with the cart so
    // that its contents find it again.
    u32 id = 0;

    bool onGround = false;
    bool onRail = false;
    bool ridden = false;

    // `kh.aT` -- the fire counter. A cart pushed into a flame chars like
    // anything else that moves; see core/entity/fire_entry.hpp.
    i16 fire = 0;

    u8 light = 0;
    bool alive = false;

    void setPosition(double px, double py, double pz);
};

// **No cap**, as the original has none. The first 32 are held from
// construction; past that the pool grows until the heap says stop -- see
// core/util/segmented_pool.hpp.
class MinecartSystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 32;


    explicit MinecartSystem(i64 seed) : rand_(seed) {}

    // `jo.a(...)` -- and it is an `onItemUse`, so it takes the block the
    // crosshair found. **Refuses anything that is not a rail**, which is the
    // method's first and only precondition.
    bool place(const tick::TickWorld& world, i32 blockX, int blockY, i32 blockZ,
               MinecartType type);

    // One 20 Hz tick of `oc.e_()` for every live cart.
    void tick(const tick::TickWorld& world, const VehicleRider& rider);

    // `EntityMinecart.applyEntityCollision` with the player side supplied by
    // the caller.  The body owns its motion, so the equal-and-opposite shove
    // is returned through the two pointers rather than making core depend on
    // PlayerBody.  This is used in every physical gamemode; Creative flight
    // still has a body and therefore still pushes carts.
    void collideWithPlayer(const AABB& playerBox, double playerX, double playerZ,
                           double* playerMotionX, double* playerMotionZ);

    int riddenIndex() const { return ridden_; }
    RiderSeat seat() const;

    // Only a plain cart can be ridden -- `interact` mounts for type 0 and does
    // something else for the other two. See the header.
    bool mount(int index);

    // **`oc.a(Ldm;)Z` -- interact**, and it is one method with three bodies:
    //
    // ```
    // if (type == 0) { player.mountEntity(this); }
    // else if (type == 1) { player.displayGUIChest(this); }
    // else if (type == 2) {
    //     ItemStack held = player.inventory.getCurrentItem();
    //     if (held != null && held.itemID == Item.coal.shiftedIndex) {
    //         if (--held.stackSize == 0) inventory.setInventorySlotContents(currentItem, null);
    //         fuel += 1200;
    //     }
    //     pushX = posX - player.posX;
    //     pushZ = posZ - player.posZ;
    // }
    // return true;
    // ```
    //
    // **The aim is taken whether or not the coal was.** Clicking a furnace cart
    // with an empty hand still points it away from you, which is how one is
    // turned round -- and it is the half of this method that reads as a bug
    // until the two lines outside the `if` are noticed.
    //
    // The caller spends the coal: this build has no stack in core to decrement,
    // exactly as the placement path has none. `spentFuel` is the cue.
    struct Interaction {
        enum class Kind : u8 { None, Mounted, Chest, Furnace };
        Kind kind = Kind::None;
        // The cart's `id`, for `Chest` -- what a screen is opened on.
        u32 cart = 0;
        // A piece of coal went in, so the caller takes one off the stack.
        bool spentFuel = false;

        bool taken() const { return kind != Kind::None; }
    };
    Interaction interact(int index, item::ItemId held, double playerX, double playerZ);

    // **The 27 slots of a chest cart**, or null for a cart that is not one or
    // an id that is not a live cart. The pointer is into a `std::vector` and is
    // good only until the next `place` or `interact`; callers copy, exactly as
    // `ContainerSession` copies a tile entity's stacks.
    item::ItemStack* chestSlots(u32 cartId);
    const item::ItemStack* chestSlots(u32 cartId) const;

    // Which cart carries this id, or -1. A linear scan, which is what every
    // other lookup over this pool is.
    int indexOfId(u32 cartId) const;
    // Returns where the rider lands -- the cart's roof, which is
    // `mountEntity`'s own answer. Invalid when nothing was aboard.
    RiderSeat dismount();

    // `oc.a(Lkh;I)Z` -- **attackEntityFrom**, shared by the arrow and the hand.
    // The cart flips its `forwardDirection`, `timeSinceHit` goes to 10 and the
    // damage counter takes `amount * 10`; **past 40 the cart breaks** and
    // leaves item 328, **plus the chest or the furnace it was carrying** --
    // which is the one place a minecart's type reaches the ground.
    //
    // Returns false only for an index that is not a live cart.
    bool attack(const tick::TickWorld& world, int index, int amount);

    // `attackEntityFrom(arrow.shootingEntity, 4)`, which is what an arrow that
    // sticks in a cart delivers. Kept as its own name because arrow.cpp reads
    // as the original does with it.
    bool hitByArrow(const tick::TickWorld& world, int index);

    void clear()
    {
        carts_.clear();
        chests_.clear();
        ridden_ = -1;
        nextId_ = 1;
    }

    // **What a chest cart spills when it is broken** -- `oc.F()`, which is a
    // chest's own spill by another name. Called by `attack`; exposed because
    // the save path wants to know a cart has contents at all.
    bool hasChest(u32 cartId) const { return chestSlots(cartId) != nullptr; }

    int count() const { return carts_.size(); }
    const Minecart& operator[](int i) const { return carts_[i]; }
    u32 refused() const { return refused_; }

    // **Where the track is**, exposed because the renderer needs it: a cart is
    // drawn tilted along the rail, and the tilt comes from sampling the track a
    // third of a block either side of the cart.
    static RailPoint railPointAt(const tick::TickWorld& world, double x, double y, double z);
    static RailPoint railPointAlong(const tick::TickWorld& world, double x, double y,
                                    double z, double offset);

    // The sample distance `RenderMinecart` uses either side of the cart.
    static constexpr double kRenderRailProbe = 0.30000001192092896;

private:
    // The save path reads and rebuilds the chest store directly, and sets the
    // id counter past every cart it restores; the chest store is the one thing
    // about a cart that is not in the cart. See core/entity/persistence.hpp.
    friend struct PersistentEntities;
    void removeAt(int index);

    // `oc.F()`'s spill: every stack in a chest cart, thrown the way a broken
    // chest throws its own. Empties the store as it goes.
    void spillChest(const tick::TickWorld& world, const Minecart& c);

    // Item 328 and whatever the type adds, spawned through the world's drop
    // sink exactly as a broken block's drop is.
    void dropAndRemove(const tick::TickWorld& world, int index);

    // **The chest carts' contents, keyed by the cart's id rather than held in
    // it.** See the header: a pool element has to be trivially destructible and
    // an `ItemStack` is not. A `std::vector` because there are usually none and
    // rarely more than a handful, and because the lookup is a scan either way.
    struct CartChest {
        u32 cart = 0;
        item::ItemStack slots[kMinecartChestSlots];
    };
    std::vector<CartChest> chests_;

    // Makes the entry for a chest cart if it does not have one, and returns it.
    CartChest* openChestStore(u32 cartId);
    void dropChestStore(u32 cartId);

    SegmentedPool<Minecart, kInitialCapacity> carts_;
    int ridden_ = -1;
    u32 refused_ = 0;
    // The next `Minecart::id`. Never reused inside a session, and set past
    // every loaded cart's when a world is restored.
    u32 nextId_ = 1;
    JavaRandom rand_;
};

}  // namespace mc::entity
