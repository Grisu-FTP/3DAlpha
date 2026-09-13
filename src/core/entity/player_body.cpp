// a1.1.2's entity physics, transcribed. See docs/physics-a1.1.2.md for the
// derivation and the obfuscated names; every constant lives in the header.

#include "core/entity/player_body.hpp"

#include "core/entity/sweep.hpp"

#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

// The materials are found through the block table rather than by id, because
// that is what `isMaterialInBB` compares: `blockMaterial == Material.water`.
// Naming the block to get at its material is the one place a name is allowed to
// become a number, exactly as the tick behaviours do it. The footstep trigger
// reads them too -- a liquid makes no sound to walk in.
constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;

// True when the box, dropped a block, would land on nothing -- the test the
// sneak walk-back uses to find the edge of a ledge.
//
// It is `getCollidingBoundingBoxes(...).isEmpty()`, so a boat or a minecart
// counts as ground here the same way a block does: sneaking off a ledge stops
// at the edge of a cart parked below it.
bool nothingBelow(const tick::TickWorld& world, const AABB& probe)
{
    if (world.anySolidBoxIn(probe)) {
        return false;
    }
    const BlockRange range = sweepRange(probe);
    AABB boxes[block::kMaxCollisionBoxes];
    for (i32 bx = range.x0; bx < range.x1; ++bx) {
        for (i32 bz = range.z0; bz < range.z1; ++bz) {
            if (!world.chunkResident(bx >> 4, bz >> 4)) {
                continue;
            }
            for (int by = range.y0; by < range.y1; ++by) {
                const block::BlockId id = world.blockAt(bx, by, bz);
                if (id == block::kAir) {
                    continue;
                }
                const int count = block::collisionBoxes(id, world.dataAt(bx, by, bz), boxes,
                                                        block::kMaxCollisionBoxes);
                for (int i = 0; i < count; ++i) {
                    if (boxes[i].offset(double(bx), double(by), double(bz)).intersects(probe)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

// One horizontal component walked back towards zero while the ground under it
// is missing. a1.1.2 has this on each axis separately and has no combined pass.
double sneakBack(const tick::TickWorld& world, const AABB& box, int axis, double delta)
{
    while (delta != 0.0) {
        const AABB probe = axis == kAxisX ? box.offset(delta, -1.0, 0.0)
                                          : box.offset(0.0, -1.0, delta);
        if (!nothingBelow(world, probe)) {
            break;
        }
        if (delta < kSneakProbe && delta >= -kSneakProbe) {
            delta = 0.0;
        } else if (delta > 0.0) {
            delta -= kSneakProbe;
        } else {
            delta += kSneakProbe;
        }
    }
    return delta;
}

}  // namespace

void PlayerBody::tickRiding(const PlayerInput& input, double seatX, double seatY,
                            double seatZ)
{
    // Where the body was, so the camera still interpolates across the tick --
    // a rider's view has to glide the same way a walker's does.
    prevX = x;
    prevEyeY = posY;
    prevZ = z;

    sneaking = input.sneak;

    // **The heading, applied exactly as it would be on foot.** `EntityLiving`
    // has no idea it is riding anything -- see the declaration -- so this is
    // `moveEntityWithHeading`'s air branch and nothing is special about it.
    applyHeading(input.strafe, input.forward, input.yawDegrees, kRiderAcceleration);

    // `moveEntity` would run here and the position it produced would be thrown
    // away, so it is skipped: what survives it is the friction, and the fall
    // distance and collision flags of a body that is not touching the world.
    motionX *= double(kAirFriction);
    motionZ *= double(kAirFriction);
    motionY = 0.0;
    fallDistance = 0.0f;
    onGround = true;
    collidedHorizontally = false;
    collidedVertically = false;

    // **A rider takes no steps.** `moveEntity` is what earns one and it is the
    // call being skipped, so neither half of the footstep may be left over from
    // the tick before the mount -- a stale `steppedOn` would trample the cell
    // under the vehicle once a tick.
    stepSoundDue = block::kAir;
    steppedOn = false;

    // `Entity.updateRidden`: the position is the vehicle's, plus the vehicle's
    // mounted offset (already in `seatY`), plus the rider's own `yOffset`.
    //
    // **The feet land exactly on `seatY`**, and the arithmetic is worth
    // spelling out because it cancels: the original passes
    // `vehicle.posY + mountedOffset + rider.yOffset` to `setPosition`, which
    // stores it as `posY` and puts the box at `posY - yOffset`. The 1.62 goes
    // in and comes straight back out, so the box's bottom is the vehicle's
    // seat and the eye is 1.62 above it.
    x = seatX;
    y = seatY;
    z = seatZ;
    const double half = double(width / 2.0f);
    box = AABB{x - half, y, z - half, x + half, y + double(height), z + half};
    ySize = 0.0f;
    posY = y + double(yOffset);
}

void PlayerBody::followSeat(double seatX, double seatY, double seatZ)
{
    x = seatX;
    y = seatY;
    z = seatZ;
    const double half = double(width / 2.0f);
    box = AABB{x - half, y, z - half, x + half, y + double(height), z + half};
    ySize = 0.0f;
    posY = y + double(yOffset);
}

void PlayerBody::setSize(float w, float h, float offset, float step)
{
    width = w;
    height = h;
    yOffset = offset;
    stepHeight = step;
    setFeet(x, y, z);
}

void PlayerBody::setFeet(double fx, double fy, double fz)
{
    x = fx;
    y = fy;
    z = fz;
    // Halved in float and widened once, the way `Entity.setPosition` does it.
    const double half = double(width / 2.0f);
    box = AABB{fx - half, fy, fz - half,
               fx + half, fy + double(height), fz + half};
    posY = fy + double(yOffset) - double(ySize);
    // A placement is a teleport, and a teleport has no previous position to
    // interpolate from. `Entity.setPositionAndRotation` does the same thing for
    // the same reason.
    snapRenderPosition();
}

bool PlayerBody::insideGround(const tick::TickWorld& world) const
{
    const BlockRange range = sweepRange(box);
    AABB boxes[block::kMaxCollisionBoxes];
    for (i32 bx = range.x0; bx < range.x1; ++bx) {
        for (i32 bz = range.z0; bz < range.z1; ++bz) {
            if (!world.chunkResident(bx >> 4, bz >> 4)) {
                continue;
            }
            for (int by = range.y0; by < range.y1; ++by) {
                const block::BlockId id = world.blockAt(bx, by, bz);
                if (id == block::kAir) {
                    continue;
                }
                const int count = block::collisionBoxes(id, world.dataAt(bx, by, bz), boxes,
                                                        block::kMaxCollisionBoxes);
                for (int i = 0; i < count; ++i) {
                    if (boxes[i].offset(double(bx), double(by), double(bz)).intersects(box)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

int PlayerBody::liftOutOfGround(const tick::TickWorld& world, int maxBlocks)
{
    for (int lifted = 0; lifted <= maxBlocks; ++lifted) {
        if (!insideGround(world)) {
            return lifted;
        }
        setFeet(x, y + 1.0, z);
    }
    // Buried deeper than the search: leave it where the caller put it rather
    // than teleporting it somewhere arbitrary, and let them see it stuck.
    return maxBlocks;
}

void PlayerBody::applyHeading(float strafe, float forward, float yawDegrees, float acceleration)
{
    float magnitude = MathHelper::sqrtFloat(strafe * strafe + forward * forward);
    if (magnitude < 0.01f) {
        return;
    }
    // Clamped **up** to one, never down: half deflection gives half speed, but
    // two axes at full deflection do not give root two.
    if (magnitude < 1.0f) {
        magnitude = 1.0f;
    }
    magnitude = acceleration / magnitude;
    strafe *= magnitude;
    forward *= magnitude;

    // The quantised table, not libm, and the float literal pi rather than a
    // double one -- both are what the original does, and both change the
    // answer in the last bits.
    const float radians = yawDegrees * kHeadingPi / 180.0f;
    const float s = MathHelper::sin(radians);
    const float c = MathHelper::cos(radians);

    // Each sum is formed in float and widened once, the way the bytecode does
    // it, rather than accumulated in double.
    motionX += double(strafe * c - forward * s);
    motionZ += double(forward * c + strafe * s);
}

void PlayerBody::move(const tick::TickWorld& world, double dx, double dy, double dz)
{
    double origDx = dx;
    const double origDy = dy;
    double origDz = dz;
    const AABB startBox = box;

    // Read before anything moves, because that is where `moveEntity` reads it:
    // it decides both the sneak probe below and, at the very end, whether this
    // move makes a footstep.
    const bool sneakingOnGround = onGround && sneaking;

    if (sneakingOnGround) {
        dx = sneakBack(world, box, kAxisX, dx);
        origDx = dx;
        dz = sneakBack(world, box, kAxisZ, dz);
        origDz = dz;
    }

    // Computed once, from the pre-clip deltas, and reused for all three axes.
    const BlockRange range = sweepRange(box.extend(dx, dy, dz));

    dy = clipAxis(world, range, box, kAxisY, dy);
    box = box.offset(0.0, dy, 0.0);

    // Whether a step up is even worth trying: standing on something, or having
    // just been stopped on the way down.
    const bool stepCandidate = onGround || (origDy != dy && origDy < 0.0);

    dx = clipAxis(world, range, box, kAxisX, dx);
    box = box.offset(dx, 0.0, 0.0);

    dz = clipAxis(world, range, box, kAxisZ, dz);
    box = box.offset(0.0, 0.0, dz);

    // The step up: rewind to where the move started, lift by the step height
    // and try the whole thing again. `ySize < 0.05` is what stops a staircase
    // being climbed two steps in one tick.
    if (stepCandidate && double(ySize) < 0.05 && (origDx != dx || origDz != dz)) {
        const double flatDx = dx;
        const double flatDy = dy;
        const double flatDz = dz;
        const AABB flatBox = box;

        dx = origDx;
        dy = double(stepHeight);
        dz = origDz;
        box = startBox;

        const BlockRange lifted = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, lifted, box, kAxisY, dy);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, lifted, box, kAxisX, dx);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, lifted, box, kAxisZ, dz);
        box = box.offset(0.0, 0.0, dz);

        // Keep the step only if it got strictly further along the ground.
        // Ties go to the flat attempt, which is why walking into a wall does
        // not levitate.
        if (flatDx * flatDx + flatDz * flatDz >= dx * dx + dz * dz) {
            dx = flatDx;
            dy = flatDy;
            dz = flatDz;
            box = flatBox;
        } else {
            // A double 0.5 literal in the original, not stepHeight widened.
            ySize = float(double(ySize) + 0.5);
        }
    }

    x = (box.minX + box.maxX) / 2.0;
    y = box.minY;
    z = (box.minZ + box.maxZ) / 2.0;
    // Taken here, with this tick's ySize, and **before** the decay below.
    posY = box.minY + double(yOffset) - double(ySize);

    collidedHorizontally = origDx != dx || origDz != dz;
    collidedVertically = origDy != dy;
    onGround = origDy != dy && origDy < 0.0;

    if (onGround) {
        // `if (fallDistance > 0.0F) { c(fallDistance); fallDistance = 0.0F; }`
        // -- fall damage, handed out through `landedFall` because the body has
        // no health to spend it on. See core/entity/player_vitals.hpp.
        if (fallDistance > 0.0f) {
            landedFall = fallDistance;
        }
        fallDistance = 0.0f;
    } else if (dy < 0.0) {
        // Widened, subtracted **in double**, then narrowed -- `f2d dsub d2f`,
        // not a float subtraction. Doing it in float drifts by about 1e-7 a
        // tick, which is invisible until it is not.
        fallDistance = float(double(fallDistance) - dy);
    }

    // Motion is killed only on the axes that were actually clipped, which is
    // what lets a player slide along a wall instead of sticking to it.
    if (origDx != dx) {
        motionX = 0.0;
    }
    if (origDy != dy) {
        motionY = 0.0;
    }
    if (origDz != dz) {
        motionZ = 0.0;
    }

    // ---- the footstep ------------------------------------------------
    //
    // **`moveEntity`'s, not the player's**, and measured on the distance
    // actually covered rather than the distance asked for -- so walking into a
    // wall is silent and being pushed along one is not.
    //
    // `distanceWalkedModified` accumulates *outside* the test below, which is
    // easy to get wrong by one indent: a sneaking player still banks the
    // distance they covered and pays for it with a footstep the moment they
    // stand up.
    const double movedX = x - (startBox.minX + startBox.maxX) / 2.0;
    const double movedZ = z - (startBox.minZ + startBox.maxZ) / 2.0;
    distanceWalked += float(double(MathHelper::sqrtDouble(movedX * movedX + movedZ * movedZ))
                            * 0.6);

    stepSoundDue = block::kAir;
    steppedOn = false;
    if (!sneakingOnGround) {
        // **The block under the feet, found by flooring `posY - 0.2 -
        // yOffset`** -- in that order, and with 0.2f widened. `posY - yOffset`
        // is the box's bottom less this tick's ySize, so a player part-way up
        // a step still asks about the block they are standing on.
        const i32 bx = MathHelper::floorDouble(x);
        const int by = int(MathHelper::floorDouble(posY - 0.20000000298023224
                                                   - double(yOffset)));
        const i32 bz = MathHelper::floorDouble(z);
        const block::BlockId under = world.blockAt(bx, by, bz);

        // **A whole block of distance per step, and the counter is
        // incremented, not reset.** `nextStepDistance++` rather than
        // `(int)distanceWalked + 1`, which is what makes a fall that covers
        // ground pay out its steps one at a time afterwards.
        if (distanceWalked > float(nextStepDistance) && under != block::kAir) {
            ++nextStepDistance;

            // Snow on top wins outright -- an inch of snow is what you hear,
            // whatever is under it -- and it wins even over a liquid, which
            // the `else if` below would have silenced.
            constexpr block::BlockId kSnowLayer = block::BlockId(mcver::Block::SnowLayer);
            const u8 material = block::def(under).material;
            if (world.blockAt(bx, by + 1, bz) == kSnowLayer) {
                stepSoundDue = kSnowLayer;
            } else if (material != kWaterMaterial && material != kLavaMaterial) {
                stepSoundDue = under;
            }

            // **`Block.onEntityWalking` is paid out by the same `if`**, on
            // the cell underfoot rather than on whatever the snow and liquid
            // rules above left in `stepSoundDue`. Two blocks in a1.1.2 answer
            // it: farmland, which is what makes a field trample back to dirt
            // as it is walked over, and redstone ore, which lights up.
            //
            // Recorded rather than run, because this method takes a const
            // world on purpose -- see `tick::entityWalkedOnBlock`, which the
            // owner of the tick calls with this the moment the move returns.
            steppedOn = true;
            stepBlockX = bx;
            stepBlockY = by;
            stepBlockZ = bz;
        }
    }

    ySize *= kYSizeDecay;
}

namespace {

// ---------------------------------------------------------------------------
// Liquids
// ---------------------------------------------------------------------------
//
// **The two branches `moveEntityWithHeading` takes before it reaches land**,
// and the predicates they turn on. Transcribed from the class file the same way
// the land branch was; docs/physics-a1.1.2.md carries the listing and used to
// end with "not yet written", which is what this is.
//
// `kh.g_()` and `kh.G()` both shrink the box by this much top and bottom before
// asking. **The same number for both** -- the lava one does not also pull in
// horizontally, which is what reading the two methods side by side settles.
// **The same number for both**, and water's copy of it lives in
// core/block/fluid_flow.hpp beside the push it belongs to.
constexpr double kLiquidProbeInset = block::kWaterProbeInset;

// `cn.a(cf,gb)Z` -- World.isMaterialInBB. The loop bounds are the floor of the
// minimum and the floor of the maximum **plus one**, which is a half-open range
// over every cell the box touches.
bool materialInBox(const tick::TickWorld& world, const AABB& probe, u8 material)
{
    const i32 x0 = MathHelper::floorDouble(probe.minX);
    const i32 x1 = MathHelper::floorDouble(probe.maxX + 1.0);
    const int y0 = MathHelper::floorDouble(probe.minY);
    const int y1 = MathHelper::floorDouble(probe.maxY + 1.0);
    const i32 z0 = MathHelper::floorDouble(probe.minZ);
    const i32 z1 = MathHelper::floorDouble(probe.maxZ + 1.0);

    for (i32 bx = x0; bx < x1; ++bx) {
        for (int by = y0; by < y1; ++by) {
            for (i32 bz = z0; bz < z1; ++bz) {
                if (block::def(world.blockAt(bx, by, bz)).material == material) {
                    return true;
                }
            }
        }
    }
    return false;
}

// `cn.b(cf)Z` -- World.isAnyLiquid.
//
// **It floors the minimum twice for negative coordinates**, and that is the
// original's, not a transcription slip: the method takes `floor_double` of each
// minimum and then decrements it again if the value was below zero. It makes
// the probe one cell wider on the negative side of each axis. Reproduced
// because this project tests negative coordinates on purpose and a quiet
// disagreement there is exactly the sort that survives for a year.
bool anyLiquidInBox(const tick::TickWorld& world, const AABB& probe)
{
    i32 x0 = MathHelper::floorDouble(probe.minX);
    const i32 x1 = MathHelper::floorDouble(probe.maxX + 1.0);
    int y0 = MathHelper::floorDouble(probe.minY);
    const int y1 = MathHelper::floorDouble(probe.maxY + 1.0);
    i32 z0 = MathHelper::floorDouble(probe.minZ);
    const i32 z1 = MathHelper::floorDouble(probe.maxZ + 1.0);

    if (probe.minX < 0.0) --x0;
    if (probe.minY < 0.0) --y0;
    if (probe.minZ < 0.0) --z0;

    for (i32 bx = x0; bx < x1; ++bx) {
        for (int by = y0; by < y1; ++by) {
            for (i32 bz = z0; bz < z1; ++bz) {
                const u8 material = block::def(world.blockAt(bx, by, bz)).material;
                if (material == kWaterMaterial || material == kLavaMaterial) {
                    return true;
                }
            }
        }
    }
    return false;
}

// The friction of whatever is under the feet. Note it floors **the box's
// bottom minus one**, not the eye and not the feet -- and that the original
// asks this question twice per tick, once to size the acceleration and once to
// damp the result, because the first `moveFlying` can leave the ground.
float groundFriction(const tick::TickWorld& world, const PlayerBody& body)
{
    if (!body.onGround) {
        return kAirFriction;
    }
    const i32 bx = MathHelper::floorDouble(body.x);
    const int by = MathHelper::floorDouble(body.box.minY) - 1;
    const i32 bz = MathHelper::floorDouble(body.z);
    const block::BlockId below = world.blockAt(bx, by, bz);
    if (below == block::kAir) {
        return kGroundFrictionBase;
    }
    return block::slipperinessOf(below) * kAirFriction;
}

}  // namespace

bool PlayerBody::handleWaterMovement(const tick::TickWorld& world)
{
    return block::handleWaterMovement(world, box, kWaterMaterial, &motionX, &motionY,
                                      &motionZ);
}

// `kh.y()`'s water branch. `g_()` is the mutating `handleWaterMovement`, so
// asking whether the body is in water is also being carried by the current --
// which is why this runs before the tick rather than being folded into a test
// the tick already makes.
WaterEntryResult PlayerBody::updateWaterEntry(const tick::TickWorld& world)
{
    const WaterEntryResult result =
        entity::updateWaterEntry(water, handleWaterMovement(world), motionX, motionY, motionZ);
    if (result.inWater) {
        fallDistance = 0.0f;
    }
    return result;
}

bool PlayerBody::inLava(const tick::TickWorld& world) const
{
    return materialInBox(world, box.expand(0.0, kLiquidProbeInset, 0.0), kLavaMaterial);
}

bool PlayerBody::onLadder(const tick::TickWorld& world) const
{
    constexpr block::BlockId kLadder = block::BlockId(mcver::Block::Ladder);
    const i32 bx = MathHelper::floorDouble(x);
    const int by = MathHelper::floorDouble(box.minY);
    const i32 bz = MathHelper::floorDouble(z);
    return world.blockAt(bx, by, bz) == kLadder || world.blockAt(bx, by + 1, bz) == kLadder;
}

// `kh.b(DDD)Z` -- isOffsetPositionInLiquid, which despite the name answers
// **"is the box, moved there, completely free"**: no collision box in it and no
// liquid in it. The swimming branch uses it to decide whether pushing against a
// wall should lift you, which is how you climb out of water onto land.
bool PlayerBody::offsetPositionFree(const tick::TickWorld& world, double dx, double dy,
                                    double dz) const
{
    const AABB probe = box.offset(dx, dy, dz);

    if (world.anySolidBoxIn(probe)) {
        return false;
    }

    const BlockRange range = sweepRange(probe);
    AABB boxes[block::kMaxCollisionBoxes];
    for (i32 bx = range.x0; bx < range.x1; ++bx) {
        for (i32 bz = range.z0; bz < range.z1; ++bz) {
            for (int by = range.y0; by < range.y1; ++by) {
                const block::BlockId id = world.blockAt(bx, by, bz);
                if (id == block::kAir) {
                    continue;
                }
                const int count = block::collisionBoxes(id, world.dataAt(bx, by, bz), boxes,
                                                        block::kMaxCollisionBoxes);
                for (int i = 0; i < count; ++i) {
                    if (boxes[i].offset(double(bx), double(by), double(bz)).intersects(probe)) {
                        return false;
                    }
                }
            }
        }
    }
    return !anyLiquidInBox(world, probe);
}

// One of the two liquid branches. They are the same code with a different drag
// factor -- 0.8 in water, 0.5 in lava -- which is how the class file has it, so
// this is one function rather than two nearly-identical ones.
void PlayerBody::swim(const tick::TickWorld& world, const PlayerInput& input, double drag)
{
    const double startY = posY;

    // **A flat 0.02, whatever the ground is.** No friction lookup, no
    // acceleration normaliser and no sprint: swimming ignores every one of the
    // land branch's terms.
    applyHeading(input.strafe, input.forward, input.yawDegrees, kSwimAcceleration);
    move(world, motionX, motionY, motionZ);

    motionX *= drag;
    motionY *= drag;
    motionZ *= drag;
    motionY -= kSwimSink;

    // Pushing into a wall lifts you, but only if there is somewhere clear to go
    // -- which is what turns swimming at a shore into climbing out of it.
    if (collidedHorizontally
        && offsetPositionFree(world, motionX, motionY + kSwimLedgeReach - posY + startY,
                              motionZ)) {
        motionY = kSwimLedgeLift;
    }
}

void PlayerBody::tick(const tick::TickWorld& world, const PlayerInput& input)
{
    // First thing in the tick, before anything moves -- which is where
    // `Entity.onUpdate` puts it.
    snapRenderPosition();

    sneaking = input.sneak;

    // **The jump branch is three-way, and in a liquid it is not a jump.**
    // `EntityLiving.onLivingUpdate` tries water, then lava, then the ground:
    // in either liquid the button adds a flat kLiquidRise to the motion and
    // `jump()` is never reached, which is what swimming up is. Getting this
    // wrong is not subtle -- a player holding the button in water launched
    // themselves off the bottom instead of rising.
    //
    // On land it still gates on standing on something, and there is no
    // cooldown: a1.1.2 jumps again the tick it lands.
    //
    // **This used to share one answer with the branch below, and it cannot any
    // more.** `handleWaterMovement` is not a question: it adds the river's
    // push to the motion as a side effect of answering. `ge.j()` calls it
    // here and `ge.b(FF)` calls it again a few lines later, so a player
    // holding the button in a current is pushed **twice** in that tick and
    // once in every other. That is the original's, it is four thousandths of a
    // block either way, and collapsing the two calls back into one would be a
    // quiet disagreement with it rather than a tidy-up.
    //
    // **The ladder branch is ours and is not in the jar.** a1.1.2 climbs a
    // ladder only by walking into it -- `collidedHorizontally && isOnLadder()`
    // a few lines below -- and on a mouse that is free, because the hand that
    // holds W is not the hand that aims. On a console it is the same thumb: the
    // circle pad both steers and looks away, so the moment a player turns their
    // head to see where they are going the stick stops pressing the wall and
    // they slide back down. Jump is the button that is otherwise dead on a
    // ladder, it means "up" everywhere else in the game, and it climbs at
    // `kLadderClimb` -- the same 0.2 a tick the wall-press gives -- so the two
    // routes up a ladder are the same speed and neither is a shortcut.
    //
    // **It is asked before `onGround`**, so standing at the foot of a ladder
    // climbs rather than jumps. That is the whole request: a ladder you have to
    // step onto before the button changes meaning is one you fall off.
    if (input.jump) {
        if (handleWaterMovement(world) || inLava(world)) {
            motionY += kLiquidRise;
        } else if (onLadder(world)) {
            motionY = kLadderClimb;
        } else if (onGround) {
            jump();
        }
    }

    // **Water first, lava second, land last**, which is the order `ge.b(FF)`
    // tests them in -- so a player standing in lava under water swims rather
    // than wades. Lava is asked only when water said no, which is the
    // original's `else if` and not an optimisation.
    const bool water = handleWaterMovement(world);
    const bool lava = !water && inLava(world);

    if (water) {
        swim(world, input, kWaterDrag);
        return;
    }
    if (lava) {
        swim(world, input, kLavaDrag);
        return;
    }

    const float frictionForAccel = groundFriction(world, *this);
    const float accel =
        kAccelNormaliser / (frictionForAccel * frictionForAccel * frictionForAccel);
    // **Sprint multiplies the ground term and nothing else**, which is where
    // Beta applies it: air control, drag, gravity and the jump are all
    // untouched, so a sprint that leaves the ground carries its speed as
    // momentum rather than steering faster. See PlayerInput::sprint.
    const float ground = input.sprint ? kSprintAcceleration : kGroundAcceleration;
    applyHeading(input.strafe, input.forward, input.yawDegrees,
                 onGround ? ground * accel : kAirAcceleration);

    // Asked again, deliberately: `applyHeading` above can have taken the player
    // off the ground, and the original re-reads it here before damping.
    const float frictionForDamping = groundFriction(world, *this);

    // **A ladder is not a lift; it is a clamped fall and a wall you can push
    // into.** Both halves are in the land branch and both are needed:
    //
    //   * before the move, a body on a ladder stops accumulating fall damage
    //     and cannot descend faster than 0.15 a tick, which is the slide;
    //   * after the move, a body that ran into something *while on a ladder*
    //     has its vertical motion set to 0.2 outright, which is the climb.
    //
    // The second is why climbing needs the stick held *into* the wall: the
    // ladder itself is a collision box two sixteenths thick, so walking at it
    // is what makes `collidedHorizontally` true. Let go and you slide back
    // down at the clamped rate. There is no separate "climb" input in a1.1.2
    // and there is none here.
    if (onLadder(world)) {
        fallDistance = 0.0f;
        if (motionY < kLadderSlide) {
            motionY = kLadderSlide;
        }
    }

    move(world, motionX, motionY, motionZ);

    if (collidedHorizontally && onLadder(world)) {
        motionY = kLadderClimb;
    }

    motionY -= kGravity;
    motionY *= kVerticalDrag;
    motionX *= double(frictionForDamping);
    motionZ *= double(frictionForDamping);
}


void PlayerBody::tickFlying(const tick::TickWorld& world, const PlayerInput& input, bool ascend,
                            bool descend, double speed)
{
    snapRenderPosition();

    sneaking = input.sneak;

    // **Cleared first, so `applyHeading` sets rather than accumulates.**
    // `moveFlying` is written as an acceleration -- it adds to the motion it
    // finds -- and what flight wants is a velocity. Zeroing first is what turns
    // one into the other, and it is also what makes the stick's own deflection
    // the speed: moveFlying normalises the pair only when its magnitude is at
    // least one, so half a stick is half a speed.
    motionX = 0.0;
    motionY = 0.0;
    motionZ = 0.0;
    applyHeading(input.strafe, input.forward, input.yawDegrees, float(speed));
    motionY = ascend ? speed : (descend ? -speed : 0.0);

    // The whole difference from Spectator: this is `Entity.moveEntity`, so the
    // box is swept and clipped against the world's collision boxes and a wall
    // stops you. `move` is a swept AABB rather than a ray, so even the sprint
    // speed's two blocks a tick cannot pass through a one-block wall.
    //
    // **Flight banks no walking distance and earns no footstep**, and that is a
    // rule of ours rather than a transcription: a1.1.2 has no flight, so
    // `moveEntity` never runs on an airborne player and the question never
    // comes up there. Left alone it does come up, and audibly. `moveEntity`
    // accumulates `distanceWalkedModified` unconditionally and only *spends* it
    // where the cell below the feet is not air, so flying banks a block of
    // credit per block travelled and pays every one of them out the instant you
    // cross low ground -- a burst of footsteps from a player who never touched
    // it. Saving and restoring the two counters around the move is what says
    // "this travel was not walking"; clearing `stepSoundDue` and `steppedOn`
    // covers the one tick that could still have earned a step -- and a
    // trampled furrow -- while skimming a floor.
    const float bankedDistance = distanceWalked;
    const int bankedStep = nextStepDistance;
    move(world, motionX, motionY, motionZ);
    distanceWalked = bankedDistance;
    nextStepDistance = bankedStep;
    stepSoundDue = block::kAir;
    steppedOn = false;

    // No fall is in progress and none is banked. See the note on the header.
    fallDistance = 0.0f;

    // Stop dead. Anything left here would be momentum the next tick adds to,
    // and the next tick sets rather than adds -- so leaving it would only
    // matter on the tick flight is switched off, which is exactly where a
    // sudden inherited velocity would be most surprising.
    motionX = 0.0;
    motionY = 0.0;
    motionZ = 0.0;
}

}  // namespace mc::entity
