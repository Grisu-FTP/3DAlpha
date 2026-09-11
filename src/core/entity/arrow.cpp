// See arrow.hpp. `kg`'s constructor, `setThrowableHeading` and `e_()`
// (onUpdate), transcribed.

#include "core/entity/arrow.hpp"

#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/painting.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::entity {
namespace {

constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;

// The class file's own literal for degrees-to-radians, and it is stored as
// `3.1415927F` divided into 180 rather than as a double pi.
constexpr float kPiF = 3.1415927f;

// `180.0D / 3.1415927410125732D`, which is the *float* pi widened -- not
// `M_PI`. The difference is in the eighth digit and it is what the class file
// divides by.
constexpr double kRadiansToDegrees = 180.0 / 3.1415927410125732;

u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

// The wrap-then-lerp the last twenty lines of `onUpdate` apply to both angles:
// the current angle becomes the new heading, while `prev` moves a fifth of the
// way toward it.  The renderer interpolates between those two fields, so
// smoothing the current field instead produces a visible one-tick lag.
float approachPreviousAngle(float previous, float target)
{
    float prev = previous;
    while (target - prev < -180.0f) {
        prev -= 360.0f;
    }
    while (target - prev >= 180.0f) {
        prev += 360.0f;
    }
    return prev + (target - prev) * kArrowTurnRate;
}

// `AxisAlignedBB.calculateIntercept`, reduced to the distance along a segment.
// Keeping the nearest t lets the arrow prefer a vehicle in front of a block,
// exactly as `EntityArrow` does after shortening its segment to the block hit.
bool segmentBoxHit(const AABB& box, double ox, double oy, double oz, double dx, double dy,
                   double dz, double maxT, double* hitT)
{
    double lo = 0.0;
    double hi = maxT;
    const double origin[3] = {ox, oy, oz};
    const double direction[3] = {dx, dy, dz};
    const double minimum[3] = {box.minX, box.minY, box.minZ};
    const double maximum[3] = {box.maxX, box.maxY, box.maxZ};
    for (int axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return false;
            continue;
        }
        double a = (minimum[axis] - origin[axis]) / direction[axis];
        double b = (maximum[axis] - origin[axis]) / direction[axis];
        if (a > b) {
            const double swap = a;
            a = b;
            b = swap;
        }
        if (a > lo) lo = a;
        if (b < hi) hi = b;
        if (lo > hi) return false;
    }
    *hitT = lo;
    return true;
}

struct EntityHit {
    enum Kind { None, Painting, Boat, Minecart };
    Kind kind = None;
    int index = -1;
    double distance = 0.0;
};

// `entityHit.attackEntityFrom(shootingEntity, 4)`. Each pool drops what it
// leaves through the world's drop sink, exactly as the hand's hit does.
bool strike(const tick::TickWorld& world, const ArrowTargets& targets, const EntityHit& hit)
{
    switch (hit.kind) {
    case EntityHit::Painting:
        return targets.paintings->attack(world, hit.index);
    case EntityHit::Boat:
        return targets.boats->attack(world, hit.index, kArrowDamage);
    case EntityHit::Minecart:
        return targets.minecarts->attack(world, hit.index, kArrowDamage);
    case EntityHit::None:
        break;
    }
    return false;
}

}  // namespace

bool ArrowSystem::shoot(const tick::TickWorld& world, double eyeX, double eyeY, double eyeZ,
                        float yawDegrees, float pitchDegrees)
{
    // Taken first, so a refused shot draws nothing from the random stream --
    // the order the fixed pool's refusal had.
    Arrow* slot = arrows_.push();
    if (slot == nullptr) {
        ++refused_;
        return false;
    }

    Arrow a{};
    a.alive = true;

    // `setLocationAndAngles(shooter.posX, shooter.posY, shooter.posZ, yaw, pitch)`
    // followed by the muzzle offset: back along the heading and down a tenth,
    // so the arrow leaves beside the bow rather than out of the player's face.
    const float yawRad = yawDegrees / 180.0f * kPiF;
    const float pitchRad = pitchDegrees / 180.0f * kPiF;
    double px = eyeX - double(MathHelper::cos(yawRad)) * kArrowMuzzleBack;
    double py = eyeY - kArrowMuzzleDrop;
    double pz = eyeZ - double(MathHelper::sin(yawRad)) * kArrowMuzzleBack;
    a.setPosition(px, py, pz);
    a.prevX = px;
    a.prevY = py;
    a.prevZ = pz;
    a.yaw = a.prevYaw = yawDegrees;
    a.pitch = a.prevPitch = pitchDegrees;

    // The launch heading, straight out of the constructor. Note motionX uses
    // **-sin(yaw)** and motionZ **+cos(yaw)** -- the opposite pairing to the
    // muzzle offset two lines up, which is not a slip in either place.
    double mx = double(-MathHelper::sin(yawRad) * MathHelper::cos(pitchRad));
    double my = double(-MathHelper::sin(pitchRad));
    double mz = double(MathHelper::cos(yawRad) * MathHelper::cos(pitchRad));

    // `setThrowableHeading(mx, my, mz, 1.5F, 1.0F)`: normalise, scatter each
    // axis by a gaussian, then scale by the velocity.
    const float length = MathHelper::sqrtDouble(mx * mx + my * my + mz * mz);
    if (length <= 0.0f) {
        arrows_.swapRemove(arrows_.size() - 1);
        return false;
    }
    mx /= double(length);
    my /= double(length);
    mz /= double(length);
    mx += rand_.nextGaussian() * kArrowScatter * double(kArrowInaccuracy);
    my += rand_.nextGaussian() * kArrowScatter * double(kArrowInaccuracy);
    mz += rand_.nextGaussian() * kArrowScatter * double(kArrowInaccuracy);
    mx *= double(kArrowVelocity);
    my *= double(kArrowVelocity);
    mz *= double(kArrowVelocity);

    a.motionX = mx;
    a.motionY = my;
    a.motionZ = mz;

    const float horizontal = MathHelper::sqrtDouble(mx * mx + mz * mz);
    a.yaw = a.prevYaw = float(std::atan2(mx, mz) * kRadiansToDegrees);
    a.pitch = a.prevPitch = float(std::atan2(my, double(horizontal)) * kRadiansToDegrees);
    a.ticksInAir = 0;
    a.light = packedLightAt(world, px, py, pz);

    *slot = a;
    return true;
}

void ArrowSystem::tick(const tick::TickWorld& world, const ArrowTargets& targets)
{
    struck_ = 0;

    for (int i = 0; i < arrows_.size();) {
        Arrow& a = arrows_[i];
        a.prevX = a.x;
        a.prevY = a.y;
        a.prevZ = a.z;

        // `Entity.onEntityUpdate`'s last line. Everything else that method does
        // -- fire, drowning, the portal counter -- has no counterpart here.
        if (a.y < kArrowVoidFloor) {
            arrows_.swapRemove(i);
            continue;
        }

        // **An arrow outside a loaded column does not tick**, the rule every
        // pool here follows: ours outlive the columns under them and a1.1.2's
        // do not, so without this an arrow flying out of the render distance
        // would fall through unloaded ground for ever.
        if (!world.chunkResident(MathHelper::floorDouble(a.x) >> 4,
                                 MathHelper::floorDouble(a.z) >> 4)) {
            ++i;
            continue;
        }

        if (a.shake > 0) {
            --a.shake;
        }

        if (a.inGround) {
            // Still in the block it stuck to?
            const int id = int(world.blockAt(a.tileX, a.tileY, a.tileZ));
            if (id == a.inTile) {
                ++a.ticksInGround;
                if (a.ticksInGround == kArrowMaxStuck) {
                    arrows_.swapRemove(i);
                    continue;
                }
                // **A stuck arrow's tick ends here.** No motion, no gravity,
                // no ray -- the original returns.
                ++i;
                continue;
            }
            // The block was mined. It comes loose with a fraction of whatever
            // motion it still had, which is what makes an arrow drop out of a
            // wall rather than shoot out of it.
            a.inGround = false;
            a.motionX *= double(rand_.nextFloat() * 0.2f);
            a.motionY *= double(rand_.nextFloat() * 0.2f);
            a.motionZ *= double(rand_.nextFloat() * 0.2f);
            a.ticksInGround = 0;
            a.ticksInAir = 0;
        } else {
            ++a.ticksInAir;
        }

        // **The ray, from where it is to where it would be.** This is the whole
        // of an arrow's collision: `rayTraceBlocks(pos, pos + motion)`, with no
        // liquids, and no sweep anywhere.
        const double step = std::sqrt(a.motionX * a.motionX + a.motionY * a.motionY
                                      + a.motionZ * a.motionZ);
        if (step > 0.0) {
            const RayHit hit = rayTrace(world, a.x, a.y, a.z, a.motionX / step,
                                        a.motionY / step, a.motionZ / step, step);
            double entityReach = step;
            if (hit.hit) {
                const double hx = hit.hitX - a.x;
                const double hy = hit.hitY - a.y;
                const double hz = hit.hitZ - a.z;
                entityReach = std::sqrt(hx * hx + hy * hy + hz * hz);
            }

            // **The nearest target along the shortened segment**, whichever
            // pool it is in: `if (d < best || best == 0.0D)`.
            EntityHit nearest{};
            const double dirX = a.motionX / step;
            const double dirY = a.motionY / step;
            const double dirZ = a.motionZ / step;
            auto consider = [&](EntityHit::Kind kind, int index, const AABB& box) {
                double distance = 0.0;
                if (segmentBoxHit(box.expand(kArrowTargetGrow, kArrowTargetGrow,
                                             kArrowTargetGrow),
                                  a.x, a.y, a.z, dirX, dirY, dirZ, entityReach, &distance)
                    && (nearest.kind == EntityHit::None || distance < nearest.distance)) {
                    nearest = EntityHit{kind, index, distance};
                }
            };
            if (targets.paintings != nullptr) {
                for (int n = 0; n < targets.paintings->count(); ++n) {
                    if ((*targets.paintings)[n].alive) {
                        consider(EntityHit::Painting, n, (*targets.paintings)[n].box);
                    }
                }
            }
            if (targets.boats != nullptr) {
                for (int n = 0; n < targets.boats->count(); ++n) {
                    if ((*targets.boats)[n].alive) {
                        consider(EntityHit::Boat, n, (*targets.boats)[n].box);
                    }
                }
            }
            if (targets.minecarts != nullptr) {
                for (int n = 0; n < targets.minecarts->count(); ++n) {
                    if ((*targets.minecarts)[n].alive) {
                        consider(EntityHit::Minecart, n, (*targets.minecarts)[n].box);
                    }
                }
            }
            if (nearest.kind != EntityHit::None && strike(world, targets, nearest)) {
                ++struck_;
                arrows_.swapRemove(i);
                continue;
            }
            if (hit.hit) {
                a.tileX = hit.x;
                a.tileY = hit.y;
                a.tileZ = hit.z;
                a.inTile = int(world.blockAt(a.tileX, a.tileY, a.tileZ));

                // **Narrowed to float and widened back**, which is what the
                // class file does (`d2f` then `f2d`) and is not a rounding
                // this port introduced.
                a.motionX = double(float(hit.hitX - a.x));
                a.motionY = double(float(hit.hitY - a.y));
                a.motionZ = double(float(hit.hitZ - a.z));
                const float travelled =
                    MathHelper::sqrtDouble(a.motionX * a.motionX + a.motionY * a.motionY
                                           + a.motionZ * a.motionZ);
                if (travelled > 0.0f) {
                    // Back out of the face by a twentieth of a block, so the
                    // shaft is drawn standing out of the wall.
                    a.x -= a.motionX / double(travelled) * kArrowEmbed;
                    a.y -= a.motionY / double(travelled) * kArrowEmbed;
                    a.z -= a.motionZ / double(travelled) * kArrowEmbed;
                }
                a.inGround = true;
                a.shake = kArrowShake;
                ++struck_;
            }
        }

        a.x += a.motionX;
        a.y += a.motionY;
        a.z += a.motionZ;

        const float horizontal =
            MathHelper::sqrtDouble(a.motionX * a.motionX + a.motionZ * a.motionZ);
        const float targetYaw = float(std::atan2(a.motionX, a.motionZ) * kRadiansToDegrees);
        const float targetPitch =
            float(std::atan2(a.motionY, double(horizontal)) * kRadiansToDegrees);
        a.prevPitch = approachPreviousAngle(a.prevPitch, targetPitch);
        a.prevYaw = approachPreviousAngle(a.prevYaw, targetYaw);
        a.pitch = targetPitch;
        a.yaw = targetYaw;

        float drag = kArrowAirDrag;
        // `g_()` -- handleWaterMovement. The original also spawns four bubble
        // particles per tick here; that is left to the caller, which owns a
        // particle pool, rather than given this file one.
        double pushX = a.motionX;
        double pushY = a.motionY;
        double pushZ = a.motionZ;
        if (block::handleWaterMovement(world, a.box, kWaterMaterial, &pushX, &pushY, &pushZ)) {
            drag = kArrowWaterDrag;
        }

        a.motionX *= double(drag);
        a.motionY *= double(drag);
        a.motionZ *= double(drag);
        a.motionY -= double(kArrowGravity);
        a.setPosition(a.x, a.y, a.z);
        a.light = packedLightAt(world, a.x, a.y, a.z);
        ++i;
    }
    arrows_.trim();
}

}  // namespace mc::entity
