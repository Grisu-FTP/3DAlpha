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
// **Only the plain cart can be ridden.** `interact` mounts for type 0, opens a
// chest for type 1 and takes coal for type 2 -- and the chest and the furnace
// need an inventory and a fuel loop that this build has nowhere to put, so they
// are placeable and drawable and do nothing else. That is stated rather than
// hidden; see `MinecartType`.
//
// The pool survives world saves through core/entity/persistence.hpp. Native
// Java chunk entity import/export remain unimplemented.
//
// The rail *shapes* are not this file's -- `core/tick/rail.hpp` derives them,
// and the connection matrix below is those ten shapes written as offsets.

#include "core/entity/rider.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

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

    bool onGround = false;
    bool onRail = false;
    bool ridden = false;

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
    void dismount();

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
        ridden_ = -1;
    }

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
    friend struct PersistentEntities;
    void removeAt(int index);

    // Item 328 and whatever the type adds, spawned through the world's drop
    // sink exactly as a broken block's drop is.
    void dropAndRemove(const tick::TickWorld& world, int index);

    SegmentedPool<Minecart, kInitialCapacity> carts_;
    int ridden_ = -1;
    u32 refused_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
