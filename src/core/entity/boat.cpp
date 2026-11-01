// See boat.hpp. `dc`'s constructor and `dc.e_()` (onUpdate), transcribed.

#include "core/entity/boat.hpp"

#include "core/entity/block_contact.hpp"
#include "core/entity/fire_entry.hpp"
#include "core/entity/wire_settle.hpp"

#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
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

// `Entity.applyEntityCollision`: `other` is pushed away from `self` and `self`
// from `other`, along the larger of the two axis gaps, by 0.05 over the root of
// that gap (capped at 0.05). Two boats on the very same spot have no line to
// push along, and the original does nothing for a gap under 0.01: boats placed
// into each other sit still until something nudges one, and then burst apart.
void shove(Boat& self, Boat& other)
{
    double dx = other.x - self.x;
    double dz = other.z - self.z;
    double gap = std::fabs(dx) > std::fabs(dz) ? std::fabs(dx) : std::fabs(dz);
    if (gap < 0.01) {
        return;
    }
    gap = std::sqrt(gap);
    dx /= gap;
    dz /= gap;
    double fall = 1.0 / gap;
    if (fall > 1.0) {
        fall = 1.0;
    }
    dx *= fall * 0.05000000074505806;
    dz *= fall * 0.05000000074505806;
    self.motionX -= dx;
    self.motionZ -= dz;
    other.motionX += dx;
    other.motionZ += dz;
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

RiderSeat BoatSystem::dismount()
{
    RiderSeat off;
    if (ridden_ >= 0 && ridden_ < boats_.size()) {
        Boat& b = boats_[ridden_];
        b.ridden = false;
        // `mountEntity`'s tail -- the rider is put on the boat's roof. See
        // core/entity/rider.hpp.
        off.valid = true;
        off.x = b.x;
        off.y = b.box.minY + kBoatHeight;
        off.z = b.z;
        off.yaw = b.yaw;
    }
    ridden_ = -1;
    return off;
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
    if (guest_ && b.remote) {
        return true;  // the host's; see `setGuest`
    }
    b.forwardDirection = -b.forwardDirection;
    b.timeSinceHit = kVehicleHitTime;
    b.damage += amount * kVehicleDamageScale;
    if (b.damage > kVehicleBreakDamage) {
        dropAndRemove(world, index);
    }
    return true;
}

int BoatSystem::indexOfEntity(i32 entityId) const
{
    if (entityId == 0) {
        return -1;
    }
    for (int i = 0; i < boats_.size(); ++i) {
        if (boats_[i].alive && boats_[i].entityId == entityId) {
            return i;
        }
    }
    return -1;
}

bool BoatSystem::spawnRemote(const tick::TickWorld& world, i32 entityId, double x, double y,
                             double z)
{
    removeById(entityId);
    Boat* slot = boats_.push();
    if (slot == nullptr) {
        ++refused_;
        return false;
    }
    Boat b{};
    b.alive = true;
    b.entityId = entityId;
    b.remote = true;
    b.forwardDirection = 1;
    b.setPosition(x, y, z);
    b.prevX = x;
    b.prevY = y;
    b.prevZ = z;
    b.track.reset(x, y, z, 0.0f);
    b.light = packedLightAt(world, x, y, z);
    *slot = b;
    return true;
}

bool BoatSystem::placeRemote(i32 entityId, double x, double y, double z, float yaw)
{
    const int index = indexOfEntity(entityId);
    if (index < 0 || !boats_[index].remote) {
        return false;
    }
    auto& b = boats_[index];
    if (b.track.aim(x, y, z, yaw)) {
        b.yaw = b.prevYaw = yaw;
    }
    return true;
}

bool BoatSystem::removeById(i32 entityId)
{
    const int index = indexOfEntity(entityId);
    if (index < 0) {
        return false;
    }
    removeAt(index);
    return true;
}

bool BoatSystem::hurtRemote(i32 entityId)
{
    const int index = indexOfEntity(entityId);
    if (index < 0) {
        return false;
    }
    Boat& b = boats_[index];
    b.forwardDirection = -b.forwardDirection;
    b.timeSinceHit = kVehicleHitTime;
    b.damage += kVehicleDamageScale;
    return true;
}

bool BoatSystem::driveHere(i32 entityId)
{
    const int index = indexOfEntity(entityId);
    if (index < 0) {
        return false;
    }
    Boat& b = boats_[index];
    b.remote = false;
    b.riderId = 0;
    b.ridden = false;
    b.motionX = b.motionY = b.motionZ = 0.0;
    b.track.reset(b.x, b.y, b.z, b.yaw);
    b.settle = true;
    return mount(index);
}

bool BoatSystem::driveElsewhere(i32 entityId, i32 riderId)
{
    const int index = indexOfEntity(entityId);
    if (index < 0) {
        return false;
    }
    if (ridden_ == index) {
        ridden_ = -1;
    }
    Boat& b = boats_[index];
    b.remote = true;
    b.riderId = riderId;
    b.ridden = riderId != 0;
    b.motionX = b.motionY = b.motionZ = 0.0;
    b.track.reset(b.x, b.y, b.z, b.yaw);
    return true;
}

bool BoatSystem::takeBack(i32 entityId)
{
    const int index = indexOfEntity(entityId);
    if (index < 0) {
        return false;
    }
    Boat& b = boats_[index];
    // **Where the rider last put it, and still.** The reports carried no
    // motion, so the host's own physics starts it from rest -- afloat, it
    // simply bobs where it was left.
    b.remote = false;
    b.riderId = 0;
    b.ridden = ridden_ == index;
    b.motionX = b.motionY = b.motionZ = 0.0;
    b.track.reset(b.x, b.y, b.z, b.yaw);
    b.settle = true;
    return true;
}

bool BoatSystem::setRider(i32 entityId, i32 riderId)
{
    const int index = indexOfEntity(entityId);
    if (index < 0 || ridden_ == index) {
        return false;
    }
    boats_[index].riderId = riderId;
    boats_[index].ridden = riderId != 0;
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

        // **Another console's boat walks to where it was last said to be**, and
        // nothing else: no buoyancy, no wall to break on, no spray. Only the
        // rocking a hit started still runs down, so a struck boat settles here
        // as it does there. See core/entity/server_track.hpp.
        if (b.remote) {
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
            double px = b.x;
            double py = b.y;
            double pz = b.z;
            float yaw = b.yaw;
            if (b.track.step(&px, &py, &pz, &yaw)) {
                b.setPosition(px, py, pz);
                b.yaw = yaw;
            }
            b.light = packedLightAt(world, b.x, b.y, b.z);
            ++i;
            continue;
        }

        // Out of the floor the wire's rounding sank it into, once, before the
        // first sweep can let it fall through. See core/entity/wire_settle.hpp.
        if (b.settle) {
            b.settle = false;
            const double lift = wireSettleLift(world, b.box);
            if (lift > 0.0) {
                b.setPosition(b.x, b.y + lift, b.z);
            }
        }

        // **`kh.y()`'s splash**, and a boat is one of the four entities that
        // reaches it: `dc.e_()` opens with `super.e_()`. Two things follow
        // from that and both are audible.
        //
        // A boat is *placed* into water, so without `firstUpdate` every boat
        // would announce itself on the tick it was put down; with it, none
        // does. And a floating boat never leaves the water, so `inWater` stays
        // set and the splash is a genuine edge -- the sound comes back only
        // after one is dragged onto land and pushed off again.
        //
        // The position is `posY - yOffset`, which for a hull whose box
        // straddles its position is the bottom of that box.
        {
            const bool inWater = block::handleWaterMovement(world, b.box, kWaterMaterial,
                                                            &b.motionX, &b.motionY, &b.motionZ);
            const WaterEntryResult wet =
                updateWaterEntry(b.water, inWater, b.motionX, b.motionY, b.motionZ,
                                 [&] {
                                     return block::isMaterialInBox(world, b.box,
                                                                   kWaterMaterial);
                                 });
            if (wet.splash) {
                world.playSoundAt(kSplashSound, b.x, b.y - kBoatYOffset, b.z, wet.volume,
                                  splashPitch(rand_));
            }
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

        // **Who is moving**, which `getCollidingBoundingBoxes` takes as its
        // first argument and asks two things of. `&b` leaves the boat out of
        // its own list -- a boat is itself one of the two solid entities in
        // a1.1.2, and finding its own box in the way of every move it tries is
        // the difference between a boat that moves and one that does not. The
        // `true` is `dc.b_(kh)`, which answers with the *other* entity's box
        // for any other entity at all: a boat is stopped by the cow, the item
        // and the player in front of it, and not only by another boat. See
        // core/entity/entity_boxes.hpp.
        const Mover who{&b, true};
        const BlockRange range = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, range, box, kAxisY, dy, who);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, range, box, kAxisX, dx, who);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, range, box, kAxisZ, dz, who);
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

        // **The blocks it is standing in come first in that tail**, and a
        // cactus is the only one in a1.1.2 that answers. One point per cell
        // per tick reaches `attackEntityFrom`, which for a boat is ten of the
        // forty it takes to break one -- so a boat pushed against a cactus is
        // kindling in four seconds. See core/entity/block_contact.hpp.
        {
            const int hits = blockContactHits(world, b.box);
            bool broken = false;
            for (int hit = 0; hit < hits && !broken; ++hit) {
                const int before = boats_.size();
                attack(world, i, kContactDamage);
                broken = boats_.size() != before;
            }
            if (broken) {
                continue;
            }
        }

        // **`moveEntity`'s tail.** A boat pushed into a fire chars: `dealFire-
        // Damage(1)` reaches `dc.a(Lkh;I)Z`, which is ten points of the forty
        // it takes to break one, so four seconds in a flame is a pile of
        // planks. The hull catches light on the same tick and hisses if it is
        // then floated -- which, for a boat, is most ticks. See
        // core/entity/fire_entry.hpp.
        {
            const FireEntryResult burn = updateFireEntry(
                &b.fire, boundingBoxBurning(world, b.box), fireWetProbe(world, b.box));
            if (burn.fizz) {
                world.playSoundAt(kFizzSound, b.x, b.y - kBoatYOffset, b.z, 0.7f,
                                  fizzPitch(rand_));
            }
            if (burn.damage) {
                // `attack` can break the boat, and then this index is another
                // boat or none.
                const int before = boats_.size();
                attack(world, i, 1);
                if (boats_.size() != before) {
                    continue;
                }
            }
        }

        const double speed =
            std::sqrt(b.motionX * b.motionX + b.motionZ * b.motionZ);

        // **The bow wave**, and it comes off the same speed that breaks the
        // boat -- `1 + speed * 60` splashes a tick past 0.15, so a boat at a
        // crawl leaves a couple and one at full tilt leaves a dozen.
        //
        // Each one is thrown by a coin flip at one of **two different places**:
        // heads puts it out to one side of the hull, a random distance along
        // and 0.7 of a block across; tails puts it a whole block *ahead* of the
        // bow. The two together are what read as a wake rather than as spray.
        // An eighth of a block below the waterline on both.
        if (speed > kBoatBreakSpeed) {
            const double forwardX = std::cos(double(b.yaw) * kPi / 180.0);
            const double forwardZ = std::sin(double(b.yaw) * kPi / 180.0);
            for (double n = 0.0; n < 1.0 + speed * 60.0; n += 1.0) {
                const double along = double(rand_.nextFloat() * 2.0f - 1.0f);
                const double across = double(rand_.nextInt(2) * 2 - 1) * 0.7;
                if (rand_.nextBoolean()) {
                    world.spawnParticle(int(ParticleKind::Splash),
                                        b.x - forwardX * along * 0.8 + forwardZ * across,
                                        b.y - 0.125,
                                        b.z - forwardZ * along * 0.8 - forwardX * across,
                                        b.motionX, b.motionY, b.motionZ);
                } else {
                    world.spawnParticle(int(ParticleKind::Splash),
                                        b.x + forwardX + forwardZ * along * 0.7,
                                        b.y - 0.125,
                                        b.z + forwardZ - forwardX * along * 0.7,
                                        b.motionX, b.motionY, b.motionZ);
                }
            }
        }

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

        // **Boats shove each other apart.** The end of `dc.e_()`: every other
        // boat within 0.2 of this box's footprint gets `applyEntityCollision`,
        // which pushes the pair away from each other along the line between
        // them at 0.05 a tick, less the further apart they are. Each pair is
        // reached from both ends, so it is shoved twice a tick, and boats
        // placed into each other burst outwards instead of sitting overlapped.
        {
            const AABB reach = b.box.expand(0.2, 0.0, 0.2);
            for (int j = 0; j < boats_.size(); ++j) {
                if (j == i || !reach.intersects(boats_[j].box)) {
                    continue;
                }
                shove(b, boats_[j]);
            }
        }

        b.light = packedLightAt(world, b.x, b.y, b.z);
        ++i;
    }
    boats_.trim();
}

}  // namespace mc::entity
