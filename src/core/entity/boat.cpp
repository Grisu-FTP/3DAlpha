// See boat.hpp. `dc`'s constructor and `dc.e_()` (onUpdate), transcribed.

#include "core/entity/boat.hpp"

#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include "items.hpp"  // generated; see tools/configure.py

#include <cmath>

namespace mc::entity {
namespace {

constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;

constexpr double kPi = 3.141592653589793;

u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

}  // namespace

void Boat::setPosition(double px, double py, double pz)
{
    x = px;
    y = py;
    z = pz;
    const double half = kBoatWidth / 2.0;
    // `yOffset` is half the height, so the box straddles the position
    // vertically rather than hanging below it.
    box = AABB{px - half, py - kBoatYOffset, pz - half,
               px + half, py - kBoatYOffset + kBoatHeight, pz + half};
}

bool BoatSystem::place(const tick::TickWorld& world, i32 blockX, int blockY, i32 blockZ)
{
    Boat b{};
    b.alive = true;
    // `new dc(world, x + 0.5, y + 1.5, z + 0.5)`, and the constructor then adds
    // its own `yOffset` on top -- so a boat placed on the surface of a water
    // cell sits a block and a half above that cell's floor.
    const double px = double(blockX) + 0.5;
    const double py = double(blockY) + 1.5;
    const double pz = double(blockZ) + 0.5;
    b.setPosition(px, py + kBoatYOffset, pz);
    b.prevX = px;
    b.prevY = py;
    b.prevZ = pz;
    b.forwardDirection = 1;
    b.light = packedLightAt(world, px, py, pz);

    // Refused only when the heap would not hold another; see
    // core/util/segmented_pool.hpp.
    Boat* slot = boats_.push();
    if (slot == nullptr) {
        ++refused_;
        return false;
    }
    *slot = b;
    return true;
}

bool BoatSystem::mount(int index)
{
    if (index < 0 || index >= boats_.size() || boats_[index].ridden) {
        return false;
    }
    if (ridden_ >= 0 && ridden_ < boats_.size()) {
        boats_[ridden_].ridden = false;
    }
    ridden_ = index;
    boats_[index].ridden = true;
    return true;
}

void BoatSystem::dismount()
{
    if (ridden_ >= 0 && ridden_ < boats_.size()) {
        boats_[ridden_].ridden = false;
    }
    ridden_ = -1;
}

RiderSeat BoatSystem::seat() const
{
    RiderSeat s;
    if (ridden_ < 0 || ridden_ >= boats_.size()) {
        return s;
    }
    const Boat& b = boats_[ridden_];
    s.valid = true;
    s.x = b.x;
    // `posY + getMountedYOffset()`. The rider's own `yOffset` is added by
    // whatever is riding, which is why it is not here.
    s.y = b.y + kBoatMountedYOffset;
    s.z = b.z;
    s.yaw = b.yaw;
    return s;
}

void BoatSystem::removeAt(int index)
{
    if (ridden_ == index) {
        ridden_ = -1;
    } else if (ridden_ == boats_.size() - 1) {
        // The last entry is about to be swapped into the hole.
        ridden_ = index;
    }
    boats_.swapRemove(index);
}

void BoatSystem::dropAndRemove(const tick::TickWorld& world, int index)
{
    const Boat& b = boats_[index];
    // `dropItemWithOffset(id, 1, 0.0F)` -- the entity's own position, with no
    // spread of any kind. That is not `dropBlockAsItem`'s scatter and it is not
    // an oversight: the scatter belongs to the block path.
    for (int n = 0; n < kBoatPlanksDropped; ++n) {
        world.spawnItem(b.x, b.y, b.z, u16(mcver::Block::Planks), 1);
    }
    for (int n = 0; n < kBoatSticksDropped; ++n) {
        world.spawnItem(b.x, b.y, b.z, u16(mcver::Item::Stick), 1);
    }
    removeAt(index);
}

bool BoatSystem::attack(const tick::TickWorld& world, int index, int amount)
{
    if (index < 0 || index >= boats_.size() || !boats_[index].alive) {
        return false;
    }
    Boat& b = boats_[index];
    b.forwardDirection = -b.forwardDirection;
    b.timeSinceHit = kVehicleHitTime;
    b.damage += amount * kVehicleDamageScale;
    if (b.damage > kVehicleBreakDamage) {
        dropAndRemove(world, index);
    }
    return true;
}

void BoatSystem::tick(const tick::TickWorld& world, const VehicleRider& rider)
{
    for (int i = 0; i < boats_.size();) {
        Boat& b = boats_[i];

        // Ours outlive the columns under them; a1.1.2's do not. Without this a
        // boat left at the edge of the render distance would sink through
        // unloaded water.
        if (!world.chunkResident(MathHelper::floorDouble(b.x) >> 4,
                                 MathHelper::floorDouble(b.z) >> 4)) {
            ++i;
            continue;
        }

        if (b.timeSinceHit > 0) {
            --b.timeSinceHit;
        }
        if (b.damage > 0) {
            --b.damage;
        }

        b.prevX = b.x;
        b.prevY = b.y;
        b.prevZ = b.z;
        b.prevYaw = b.yaw;

        // **Buoyancy, by slice.** Five horizontal slices of the box, each
        // dropped by an eighth of a block, each asked whether it is in water.
        // The fraction that is, doubled and less one, is the push -- so a boat
        // exactly half under water neither rises nor sinks and there is no
        // water level in it anywhere.
        double submerged = 0.0;
        for (int slice = 0; slice < kBoatBuoyancySlices; ++slice) {
            const double height = b.box.maxY - b.box.minY;
            const double y0 = b.box.minY + height * double(slice) / kBoatBuoyancySlices
                              - kBoatBuoyancyDrop;
            const double y1 = b.box.minY + height * double(slice + 1) / kBoatBuoyancySlices
                              - kBoatBuoyancyDrop;
            const AABB probe{b.box.minX, y0, b.box.minZ, b.box.maxX, y1, b.box.maxZ};
            if (block::isMaterialInBox(world, probe, kWaterMaterial)) {
                submerged += 1.0 / double(kBoatBuoyancySlices);
            }
        }
        b.motionY += kBoatBuoyancy * (submerged * 2.0 - 1.0);

        // **Steering.** A fifth of the rider's own motion, and that is the
        // whole of it -- see core/entity/rider.hpp.
        if (b.ridden && rider.present) {
            b.motionX += rider.motionX * kBoatRiderPush;
            b.motionZ += rider.motionZ * kBoatRiderPush;
        }

        if (b.motionX < -kBoatSpeedCap) b.motionX = -kBoatSpeedCap;
        if (b.motionX > kBoatSpeedCap) b.motionX = kBoatSpeedCap;
        if (b.motionZ < -kBoatSpeedCap) b.motionZ = -kBoatSpeedCap;
        if (b.motionZ > kBoatSpeedCap) b.motionZ = kBoatSpeedCap;

        if (b.onGround) {
            b.motionX *= kBoatGroundDrag;
            b.motionY *= kBoatGroundDrag;
            b.motionZ *= kBoatGroundDrag;
        }

        // `moveEntity`, through the shared sweep. A boat's `stepHeight` is the
        // default 0, so there is no step-up here.
        AABB box = b.box;
        double dx = b.motionX;
        double dy = b.motionY;
        double dz = b.motionZ;
        const double wantX = dx;
        const double wantY = dy;
        const double wantZ = dz;

        const BlockRange range = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, range, box, kAxisY, dy);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, range, box, kAxisX, dx);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, range, box, kAxisZ, dz);
        box = box.offset(0.0, 0.0, dz);

        b.box = box;
        b.x = (box.minX + box.maxX) / 2.0;
        b.y = box.minY + kBoatYOffset;
        b.z = (box.minZ + box.maxZ) / 2.0;
        b.onGround = wantY != dy && wantY < 0.0;
        b.hitWall = wantX != dx || wantZ != dz;

        if (b.motionX != dx) b.motionX = 0.0;
        if (b.motionY != dy) b.motionY = 0.0;
        if (b.motionZ != dz) b.motionZ = 0.0;

        const double speed =
            std::sqrt(b.motionX * b.motionX + b.motionZ * b.motionZ);

        // **Into a wall faster than 0.15 and the boat is gone**, leaving what
        // a broken boat leaves.
        if (b.hitWall && speed > kBoatBreakSpeed) {
            dropAndRemove(world, i);
            continue;
        }

        b.motionX *= kBoatDragX;
        b.motionY *= kBoatDragY;
        b.motionZ *= kBoatDragZ;

        // **The yaw chases where the boat is actually going**, at most twenty
        // degrees a tick, and only once it has moved. Note the arguments to
        // atan2 are the *previous* position minus the current one -- the boat
        // faces backwards along its own travel, which is what the model's own
        // orientation expects.
        double heading = double(b.yaw);
        const double backX = b.prevX - b.x;
        const double backZ = b.prevZ - b.z;
        if (backX * backX + backZ * backZ > kBoatTurnThreshold) {
            heading = double(float(std::atan2(backZ, backX) * 180.0 / kPi));
        }
        double delta = heading - double(b.yaw);
        while (delta >= 180.0) {
            delta -= 360.0;
        }
        while (delta < -180.0) {
            delta += 360.0;
        }
        if (delta > kBoatTurnLimit) {
            delta = kBoatTurnLimit;
        }
        if (delta < -kBoatTurnLimit) {
            delta = -kBoatTurnLimit;
        }
        b.yaw = float(double(b.yaw) + delta);

        b.light = packedLightAt(world, b.x, b.y, b.z);
        ++i;
    }
    boats_.trim();
}

}  // namespace mc::entity
