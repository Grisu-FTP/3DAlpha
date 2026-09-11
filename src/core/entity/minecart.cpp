// See minecart.hpp. `oc.e_()` (onUpdate), `oc.g(DDD)` (getPosOnRail) and
// `oc.a(DDDD)` (the same offset along the track), transcribed.

#include "core/entity/minecart.hpp"

#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include "items.hpp"  // generated; see tools/configure.py

#include <cmath>

namespace mc::entity {
namespace {

constexpr double kPi = 3.141592653589793;

// **`oc.j`, the connection matrix**, and it is not an independent table: it is
// exactly the ten rail shapes `core/tick/rail.hpp` derives, written as the two
// neighbours each shape joins. Shape 2 ascends towards +x, so its low end is
// one block down at -x; shape 6 curves between +x and +z; and so on down the
// list in that header. Checked against the class file's own array, which is
// what tests/minecart_test.cpp pins.
//
// [shape][end][axis], and the middle index is the *y* offset -- non-zero only
// on the four ascending shapes, which is how the cart knows to climb.
constexpr int kRailLinks[10][2][3] = {
    {{0, 0, -1}, {0, 0, 1}},    // 0 flat along z
    {{-1, 0, 0}, {1, 0, 0}},    // 1 flat along x
    {{-1, -1, 0}, {1, 0, 0}},   // 2 ascending towards +x
    {{-1, 0, 0}, {1, -1, 0}},   // 3 ascending towards -x
    {{0, 0, -1}, {0, -1, 1}},   // 4 ascending towards -z
    {{0, -1, -1}, {0, 0, 1}},   // 5 ascending towards +z
    {{0, 0, 1}, {1, 0, 0}},     // 6 curve +x/+z
    {{0, 0, 1}, {-1, 0, 0}},    // 7 curve -x/+z
    {{0, 0, -1}, {-1, 0, 0}},   // 8 curve -x/-z
    {{0, 0, -1}, {1, 0, 0}},    // 9 curve +x/-z
};

constexpr int kRailShapeCount = 10;

block::BlockId railBlock()
{
    return block::BlockId(mcver::Block::Rail);
}

bool isRail(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    return world.blockAt(x, y, z) == railBlock();
}

u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

// See kMinecartMotionLimit. The collision is the only step that compounds
// motion; everything else in the tick drags it or adds a bounded amount.
double limitMotion(double m)
{
    return m < -kMinecartMotionLimit ? -kMinecartMotionLimit
         : m > kMinecartMotionLimit  ? kMinecartMotionLimit
                                     : m;
}

// `EntityMinecart.applyEntityCollision`, without Entity's virtual dispatch.
// The caller supplies the two carts in the same order every time, so one pair
// receives one impulse per tick rather than being accidentally doubled.
void collideCarts(Minecart& first, Minecart& second)
{
    double dx = second.x - first.x;
    double dz = second.z - first.z;
    const double distance = std::sqrt(dx * dx + dz * dz);
    if (distance < 0.01) {
        return;
    }
    dx /= distance;
    dz /= distance;
    const double push = (distance < 1.0 ? 1.0 : 1.0 / distance) * 0.1;
    dx *= push;
    dz *= push;

    const double sharedX = (first.motionX + second.motionX) * 0.5;
    const double sharedZ = (first.motionZ + second.motionZ) * 0.5;
    first.motionX = limitMotion(first.motionX * 0.2 + sharedX - dx);
    first.motionZ = limitMotion(first.motionZ * 0.2 + sharedZ - dz);
    second.motionX = limitMotion(second.motionX * 0.2 + sharedX + dx);
    second.motionZ = limitMotion(second.motionZ * 0.2 + sharedZ + dz);
}

}  // namespace

void Minecart::setPosition(double px, double py, double pz)
{
    x = px;
    y = py;
    z = pz;
    const double half = kMinecartWidth / 2.0;
    box = AABB{px - half, py - kMinecartYOffset, pz - half,
               px + half, py - kMinecartYOffset + kMinecartHeight, pz + half};
}

RailPoint MinecartSystem::railPointAt(const tick::TickWorld& world, double x, double y,
                                      double z)
{
    i32 i = MathHelper::floorDouble(x);
    int j = int(MathHelper::floorDouble(y));
    i32 k = MathHelper::floorDouble(z);

    // **A rail one block down still counts.** That is what lets a cart run off
    // the top of an ascending rail onto the flat one beyond it without ever
    // being airborne.
    if (isRail(world, i, j - 1, k)) {
        --j;
    }
    if (!isRail(world, i, j, k)) {
        return RailPoint{};
    }

    const int meta = int(world.dataAt(i, j, k)) % kRailShapeCount;
    double py = double(j);
    if (meta >= 2 && meta <= 5) {
        py = double(j + 1);
    }

    const int(*link)[3] = kRailLinks[meta];
    const double ax = double(i) + 0.5 + double(link[0][0]) * 0.5;
    const double ay = double(j) + 0.5 + double(link[0][1]) * 0.5;
    const double az = double(k) + 0.5 + double(link[0][2]) * 0.5;
    const double bx = double(i) + 0.5 + double(link[1][0]) * 0.5;
    const double by = double(j) + 0.5 + double(link[1][1]) * 0.5;
    const double bz = double(k) + 0.5 + double(link[1][2]) * 0.5;

    const double dx = bx - ax;
    // **Doubled**, and only on y. The two ends are half a block apart on each
    // axis, so a unit of the parameter is a whole block horizontally and half
    // a block vertically -- the doubling is what makes the slope come out at
    // one block per block rather than at a half.
    const double dy = (by - ay) * 2.0;
    const double dz = bz - az;

    double t = 0.0;
    double px = x;
    double pz = z;
    if (dx == 0.0) {
        px = double(i) + 0.5;
        t = z - double(k);
    } else if (dz == 0.0) {
        pz = double(k) + 0.5;
        t = x - double(i);
    } else {
        const double ox = x - ax;
        const double oz = z - az;
        t = (ox * dx + oz * dz) * 2.0;
    }

    px = ax + dx * t;
    py = ay + dy * t;
    pz = az + dz * t;

    // The two corrections that put the point on the rail's surface rather than
    // on its centre line.
    if (dy < 0.0) {
        py += 1.0;
    }
    if (dy > 0.0) {
        py += 0.5;
    }

    RailPoint out;
    out.valid = true;
    out.x = px;
    out.y = py;
    out.z = pz;
    return out;
}

RailPoint MinecartSystem::railPointAlong(const tick::TickWorld& world, double x, double y,
                                         double z, double offset)
{
    i32 i = MathHelper::floorDouble(x);
    int j = int(MathHelper::floorDouble(y));
    i32 k = MathHelper::floorDouble(z);
    if (isRail(world, i, j - 1, k)) {
        --j;
    }
    if (!isRail(world, i, j, k)) {
        return RailPoint{};
    }

    const int meta = int(world.dataAt(i, j, k)) % kRailShapeCount;
    double py = double(j);
    if (meta >= 2 && meta <= 5) {
        py = double(j + 1);
    }

    const int(*link)[3] = kRailLinks[meta];
    double dx = double(link[1][0] - link[0][0]);
    double dz = double(link[1][2] - link[0][2]);
    const double length = std::sqrt(dx * dx + dz * dz);
    dx /= length;
    dz /= length;

    double px = x + dx * offset;
    double pz = z + dz * offset;

    // Stepping off the end of an ascending rail changes the height it should be
    // sampled at, which is what this pair of tests is for.
    if (link[0][1] != 0 && MathHelper::floorDouble(px) - i == link[0][0]
        && MathHelper::floorDouble(pz) - k == link[0][2]) {
        py += double(link[0][1]);
    } else if (link[1][1] != 0 && MathHelper::floorDouble(px) - i == link[1][0]
               && MathHelper::floorDouble(pz) - k == link[1][2]) {
        py += double(link[1][1]);
    }

    return railPointAt(world, px, py, pz);
}

bool MinecartSystem::place(const tick::TickWorld& world, i32 blockX, int blockY, i32 blockZ,
                           MinecartType type)
{
    // **Rails only.** `jo.a(...)`'s first test, and the reason a minecart put
    // on the ground does nothing -- which is the game, not this port.
    if (!isRail(world, blockX, blockY, blockZ)) {
        return false;
    }
    Minecart c{};
    c.alive = true;
    c.type = type;
    c.forwardDirection = 1;
    const double px = double(blockX) + 0.5;
    const double py = double(blockY) + 0.5;
    const double pz = double(blockZ) + 0.5;
    c.setPosition(px, py + kMinecartYOffset, pz);
    c.prevX = px;
    c.prevY = py;
    c.prevZ = pz;
    c.light = packedLightAt(world, px, py, pz);

    // Refused only when the heap would not hold another; see
    // core/util/segmented_pool.hpp.
    Minecart* slot = carts_.push();
    if (slot == nullptr) {
        ++refused_;
        return false;
    }
    *slot = c;
    return true;
}

bool MinecartSystem::mount(int index)
{
    if (index < 0 || index >= carts_.size() || carts_[index].ridden) {
        return false;
    }
    // Only the plain cart. A chest cart opens an inventory and a furnace cart
    // takes coal, and neither has anywhere to go in this build.
    if (carts_[index].type != MinecartType::Rideable) {
        return false;
    }
    if (ridden_ >= 0 && ridden_ < carts_.size()) {
        carts_[ridden_].ridden = false;
    }
    ridden_ = index;
    carts_[index].ridden = true;
    return true;
}

void MinecartSystem::dismount()
{
    if (ridden_ >= 0 && ridden_ < carts_.size()) {
        carts_[ridden_].ridden = false;
    }
    ridden_ = -1;
}

void MinecartSystem::collideWithPlayer(const AABB& playerBox, double playerX, double playerZ,
                                       double* playerMotionX, double* playerMotionZ)
{
    if (playerMotionX == nullptr || playerMotionZ == nullptr) {
        return;
    }
    for (int i = 0; i < carts_.size(); ++i) {
        Minecart& c = carts_[i];
        // The rider is attached to this cart, not bumping into it.  The 0.2
        // expansion is the original minecart's nearby-entity query.
        if (c.ridden || !c.box.expand(0.2, 0.0, 0.2).intersects(playerBox)) {
            continue;
        }
        double dx = playerX - c.x;
        double dz = playerZ - c.z;
        const double distance = std::sqrt(dx * dx + dz * dz);
        if (distance < 0.01) {
            continue;
        }
        dx /= distance;
        dz /= distance;
        const double push = (distance < 1.0 ? 1.0 : 1.0 / distance) * 0.1;
        dx *= push;
        dz *= push;
        *playerMotionX += dx;
        *playerMotionZ += dz;
        c.motionX -= dx;
        c.motionZ -= dz;
    }
}

RiderSeat MinecartSystem::seat() const
{
    RiderSeat s;
    if (ridden_ < 0 || ridden_ >= carts_.size()) {
        return s;
    }
    const Minecart& c = carts_[ridden_];
    s.valid = true;
    s.x = c.x;
    s.y = c.y + kMinecartMountedYOffset;
    s.z = c.z;
    s.yaw = c.yaw;
    return s;
}

bool MinecartSystem::attack(const tick::TickWorld& world, int index, int amount)
{
    if (index < 0 || index >= carts_.size() || !carts_[index].alive) {
        return false;
    }
    Minecart& c = carts_[index];
    c.forwardDirection = -c.forwardDirection;
    c.timeSinceHit = kVehicleHitTime;
    c.damage += amount * kVehicleDamageScale;
    if (c.damage > kVehicleBreakDamage) {
        dropAndRemove(world, index);
    }
    return true;
}

bool MinecartSystem::hitByArrow(const tick::TickWorld& world, int index)
{
    // `attackEntityFrom(source, 4)`, which at scale 10 is 40 -- one short of
    // the threshold, so an arrow does not break a fresh cart on its own.
    return attack(world, index, 4);
}

void MinecartSystem::dropAndRemove(const tick::TickWorld& world, int index)
{
    const Minecart& c = carts_[index];
    // `dropItemWithOffset(id, 1, 0.0F)` three times over at most: the cart
    // itself always, and then the container it was, if it was one. **The
    // storage and powered cart items are not what drops** -- a chest cart
    // leaves a plain minecart and a chest, which is why this reads the block
    // ids and not items 342 and 343.
    world.spawnItem(c.x, c.y, c.z, u16(mcver::Item::Minecart), 1);
    if (c.type == MinecartType::Chest) {
        world.spawnItem(c.x, c.y, c.z, u16(mcver::Block::Chest), 1);
    } else if (c.type == MinecartType::Furnace) {
        world.spawnItem(c.x, c.y, c.z, u16(mcver::Block::Furnace), 1);
    }
    removeAt(index);
}

void MinecartSystem::removeAt(int index)
{
    if (ridden_ == index) {
        ridden_ = -1;
    } else if (ridden_ == carts_.size() - 1) {
        ridden_ = index;
    }
    carts_.swapRemove(index);
}

void MinecartSystem::tick(const tick::TickWorld& world, const VehicleRider& rider)
{
    for (int index = 0; index < carts_.size();) {
        Minecart& c = carts_[index];

        if (!world.chunkResident(MathHelper::floorDouble(c.x) >> 4,
                                 MathHelper::floorDouble(c.z) >> 4)) {
            ++index;
            continue;
        }

        if (c.timeSinceHit > 0) {
            --c.timeSinceHit;
        }
        if (c.damage > 0) {
            --c.damage;
        }

        c.prevX = c.x;
        c.prevY = c.y;
        c.prevZ = c.z;
        c.prevYaw = c.yaw;

        c.motionY -= kMinecartGravity;

        i32 i = MathHelper::floorDouble(c.x);
        int j = int(MathHelper::floorDouble(c.y));
        i32 k = MathHelper::floorDouble(c.z);
        if (isRail(world, i, j - 1, k)) {
            --j;
        }

        bool smoking = false;
        c.onRail = isRail(world, i, j, k);

        if (c.onRail) {
            const RailPoint before = railPointAt(world, c.x, c.y, c.z);
            const int meta = int(world.dataAt(i, j, k)) % kRailShapeCount;

            // **The height is assigned, not integrated.** A cart on a slope is
            // not falling; it is standing on a number this line looked up.
            c.y = double(j);
            if (meta >= 2 && meta <= 5) {
                c.y = double(j + 1);
            }

            // The slope push, on the axis the rail descends towards.
            if (meta == 2) c.motionX -= kMinecartSlopePush;
            if (meta == 3) c.motionX += kMinecartSlopePush;
            if (meta == 4) c.motionZ += kMinecartSlopePush;
            if (meta == 5) c.motionZ -= kMinecartSlopePush;

            const int(*link)[3] = kRailLinks[meta];
            double dx = double(link[1][0] - link[0][0]);
            double dz = double(link[1][2] - link[0][2]);
            const double diagonal = std::sqrt(dx * dx + dz * dz);

            // **The speed is kept and only the direction is replaced**, which
            // is why a cart takes a corner without slowing down. The sign comes
            // from which way it was already going.
            const double along = c.motionX * dx + c.motionZ * dz;
            if (along < 0.0) {
                dx = -dx;
                dz = -dz;
            }
            const double speed =
                std::sqrt(c.motionX * c.motionX + c.motionZ * c.motionZ);
            c.motionX = speed * dx / diagonal;
            c.motionZ = speed * dz / diagonal;

            // **Snapped onto the centreline.** Not steered towards it --
            // assigned. A cart cannot leave a rail sideways because there is no
            // step in which it could.
            const double ax = double(i) + 0.5 + double(link[0][0]) * 0.5;
            const double az = double(k) + 0.5 + double(link[0][2]) * 0.5;
            const double bx = double(i) + 0.5 + double(link[1][0]) * 0.5;
            const double bz = double(k) + 0.5 + double(link[1][2]) * 0.5;
            const double railX = bx - ax;
            const double railZ = bz - az;

            double t = 0.0;
            if (railX == 0.0) {
                c.x = double(i) + 0.5;
                t = c.z - double(k);
            } else if (railZ == 0.0) {
                c.z = double(k) + 0.5;
                t = c.x - double(i);
            } else {
                const double ox = c.x - ax;
                const double oz = c.z - az;
                t = (ox * railX + oz * railZ) * 2.0;
            }
            c.x = ax + railX * t;
            c.z = az + railZ * t;
            c.setPosition(c.x, c.y + kMinecartYOffset, c.z);

            double moveX = c.motionX;
            double moveZ = c.motionZ;
            if (c.ridden) {
                moveX *= kMinecartRiddenMove;
                moveZ *= kMinecartRiddenMove;
            }
            if (moveX < -kMinecartSpeedCap) moveX = -kMinecartSpeedCap;
            if (moveX > kMinecartSpeedCap) moveX = kMinecartSpeedCap;
            if (moveZ < -kMinecartSpeedCap) moveZ = -kMinecartSpeedCap;
            if (moveZ > kMinecartSpeedCap) moveZ = kMinecartSpeedCap;

            // **A flat move**, with no y at all: the height came from the rail.
            {
                AABB box = c.box;
                double mx = moveX;
                double my = 0.0;
                double mz = moveZ;
                const BlockRange range = sweepRange(box.extend(mx, my, mz));
                my = clipAxis(world, range, box, kAxisY, my);
                box = box.offset(0.0, my, 0.0);
                mx = clipAxis(world, range, box, kAxisX, mx);
                box = box.offset(mx, 0.0, 0.0);
                mz = clipAxis(world, range, box, kAxisZ, mz);
                box = box.offset(0.0, 0.0, mz);
                c.box = box;
                c.x = (box.minX + box.maxX) / 2.0;
                c.y = box.minY + kMinecartYOffset;
                c.z = (box.minZ + box.maxZ) / 2.0;
                c.onGround = true;
                if (moveX != mx) c.motionX = 0.0;
                if (moveZ != mz) c.motionZ = 0.0;
            }

            // Stepping up onto the next rail of a slope.
            if (link[0][1] != 0 && MathHelper::floorDouble(c.x) - i == link[0][0]
                && MathHelper::floorDouble(c.z) - k == link[0][2]) {
                c.setPosition(c.x, c.y + double(link[0][1]), c.z);
            } else if (link[1][1] != 0 && MathHelper::floorDouble(c.x) - i == link[1][0]
                       && MathHelper::floorDouble(c.z) - k == link[1][2]) {
                c.setPosition(c.x, c.y + double(link[1][1]), c.z);
            }

            if (c.ridden) {
                // **An occupied cart barely slows at all** -- 0.997 against the
                // empty cart's 0.96, which is the difference between coasting
                // across a map and stopping in a few blocks.
                c.motionX *= kMinecartRiddenDrag;
                c.motionY = 0.0;
                c.motionZ *= kMinecartRiddenDrag;
            } else {
                if (c.type == MinecartType::Furnace) {
                    const double push =
                        double(MathHelper::sqrtDouble(c.pushX * c.pushX + c.pushZ * c.pushZ));
                    if (push > 0.01) {
                        smoking = true;
                        c.pushX /= push;
                        c.pushZ /= push;
                        c.motionX *= kMinecartFurnaceDragOn;
                        c.motionY = 0.0;
                        c.motionZ *= kMinecartFurnaceDragOn;
                        c.motionX += c.pushX * kMinecartFurnacePush;
                        c.motionZ += c.pushZ * kMinecartFurnacePush;
                    } else {
                        c.motionX *= kMinecartFurnaceDragOff;
                        c.motionY = 0.0;
                        c.motionZ *= kMinecartFurnaceDragOff;
                    }
                }
                c.motionX *= kMinecartRailDrag;
                c.motionY = 0.0;
                c.motionZ *= kMinecartRailDrag;
            }

            // **The height, re-read after the move**, and the difference fed
            // back into the speed. This is what makes a cart accelerate down a
            // slope beyond what the constant push gives it.
            const RailPoint after = railPointAt(world, c.x, c.y, c.z);
            if (after.valid && before.valid) {
                const double drop = (before.y - after.y) * kMinecartSlopeFeedback;
                const double now =
                    std::sqrt(c.motionX * c.motionX + c.motionZ * c.motionZ);
                if (now > 0.0) {
                    c.motionX = c.motionX / now * (now + drop);
                    c.motionZ = c.motionZ / now * (now + drop);
                }
                // **`setPosition(posX, vec3d1.yCoord, posZ)`** -- the rail
                // surface becomes `posY` directly, with no `yOffset` added. So
                // the cart's box straddles the rail block rather than sitting
                // on top of it, which is the original's own arrangement and is
                // why the next tick's `floor(posY)` still finds the rail.
                c.setPosition(c.x, after.y, c.z);
            }

            // Crossing into a new cell re-points the motion at it, which is how
            // a cart leaves a curve on the right heading.
            const i32 nx = MathHelper::floorDouble(c.x);
            const i32 nz = MathHelper::floorDouble(c.z);
            if (nx != i || nz != k) {
                const double now =
                    std::sqrt(c.motionX * c.motionX + c.motionZ * c.motionZ);
                c.motionX = now * double(nx - i);
                c.motionZ = now * double(nz - k);
            }

            if (c.type == MinecartType::Furnace) {
                const double push =
                    double(MathHelper::sqrtDouble(c.pushX * c.pushX + c.pushZ * c.pushZ));
                if (push > 0.01
                    && c.motionX * c.motionX + c.motionZ * c.motionZ > 0.001) {
                    c.pushX /= push;
                    c.pushZ /= push;
                    if (c.pushX * c.motionX + c.pushZ * c.motionZ < 0.0) {
                        c.pushX = 0.0;
                        c.pushZ = 0.0;
                    } else {
                        c.pushX = c.motionX;
                        c.pushZ = c.motionZ;
                    }
                }
            }
        } else {
            // **Off the rails**, which is an ordinary falling box.
            if (c.motionX < -kMinecartSpeedCap) c.motionX = -kMinecartSpeedCap;
            if (c.motionX > kMinecartSpeedCap) c.motionX = kMinecartSpeedCap;
            if (c.motionZ < -kMinecartSpeedCap) c.motionZ = -kMinecartSpeedCap;
            if (c.motionZ > kMinecartSpeedCap) c.motionZ = kMinecartSpeedCap;

            if (c.onGround) {
                c.motionX *= kMinecartGroundDrag;
                c.motionY *= kMinecartGroundDrag;
                c.motionZ *= kMinecartGroundDrag;
            }

            AABB box = c.box;
            double mx = c.motionX;
            double my = c.motionY;
            double mz = c.motionZ;
            const double wantY = my;
            const BlockRange range = sweepRange(box.extend(mx, my, mz));
            my = clipAxis(world, range, box, kAxisY, my);
            box = box.offset(0.0, my, 0.0);
            mx = clipAxis(world, range, box, kAxisX, mx);
            box = box.offset(mx, 0.0, 0.0);
            mz = clipAxis(world, range, box, kAxisZ, mz);
            box = box.offset(0.0, 0.0, mz);
            c.box = box;
            c.x = (box.minX + box.maxX) / 2.0;
            c.y = box.minY + kMinecartYOffset;
            c.z = (box.minZ + box.maxZ) / 2.0;
            c.onGround = wantY != my && wantY < 0.0;
            if (c.motionX != mx) c.motionX = 0.0;
            if (c.motionY != my) c.motionY = 0.0;
            if (c.motionZ != mz) c.motionZ = 0.0;

            if (!c.onGround) {
                c.motionX *= kMinecartAirDrag;
                c.motionY *= kMinecartAirDrag;
                c.motionZ *= kMinecartAirDrag;
            }
        }

        // **The heading, and the flip.** A cart that reverses does not spin --
        // it turns 180 degrees and remembers that it is now backwards, which is
        // what `isFlipped` is for and is why a minecart's model never appears
        // to rotate through a whole turn.
        const double backX = c.prevX - c.x;
        const double backZ = c.prevZ - c.z;
        if (backX * backX + backZ * backZ > 0.001) {
            c.yaw = float(std::atan2(backZ, backX) * 180.0 / kPi);
            if (c.flipped) {
                c.yaw += 180.0f;
            }
        }
        double delta = double(c.yaw) - double(c.prevYaw);
        while (delta >= 180.0) {
            delta -= 360.0;
        }
        while (delta < -180.0) {
            delta += 360.0;
        }
        if (delta < -kMinecartFlipAngle || delta >= kMinecartFlipAngle) {
            c.yaw += 180.0f;
            c.flipped = !c.flipped;
        }

        if (smoking && rand_.nextInt(4) == 0) {
            --c.fuel;
            if (c.fuel < 0) {
                c.pushX = 0.0;
                c.pushZ = 0.0;
            }
        }

        c.light = packedLightAt(world, c.x, c.y, c.z);

        // `oc.e_()` asks for other carts in this slightly enlarged box after
        // moving.  A pair is resolved only when its earlier pool slot ticks;
        // that preserves the intended one-cart-pushes-the-next transfer while
        // avoiding a second, artificial impulse when the later slot runs.
        const AABB nearby = c.box.expand(0.2, 0.0, 0.2);
        for (int other = index + 1; other < carts_.size(); ++other) {
            if (nearby.intersects(carts_[other].box)) {
                collideCarts(c, carts_[other]);
            }
        }
        ++index;
    }
    carts_.trim();
}

}  // namespace mc::entity
