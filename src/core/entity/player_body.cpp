// a1.1.2's entity physics, transcribed. See docs/physics-a1.1.2.md for the
// derivation and the obfuscated names; every constant lives in the header.

#include "core/entity/player_body.hpp"

#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

enum Axis { kAxisX = 0, kAxisY, kAxisZ };

// The block volume `World.getCollidingBoundingBoxes` sweeps, as integers.
//
// **`y0` is one below the box, and that is not an off-by-one.** A fence stands
// a block and a half tall, so the fence in the block *beneath* the swept volume
// still reaches into it. Starting at `floor(minY)` instead would let a player
// walk through the top half of every fence in the world.
struct BlockRange {
    i32 x0, x1;
    int y0, y1;
    i32 z0, z1;
};

BlockRange sweepRange(const AABB& swept)
{
    BlockRange r;
    r.x0 = MathHelper::floorDouble(swept.minX);
    r.x1 = MathHelper::floorDouble(swept.maxX + 1.0);
    r.y0 = MathHelper::floorDouble(swept.minY) - 1;
    r.y1 = MathHelper::floorDouble(swept.maxY + 1.0);
    r.z0 = MathHelper::floorDouble(swept.minZ);
    r.z1 = MathHelper::floorDouble(swept.maxZ + 1.0);
    return r;
}

// `AxisAlignedBB.calculateXOffset` and its two siblings. The receiver is the
// **block's** box and `mover` is the entity's, which is the way round the
// original calls them and the easiest thing in this file to invert by accident.
//
// The two guards ask whether the boxes overlap on the *other* two axes; if they
// do not, the mover slides past and the delta is untouched. The clamps then
// only ever shrink the delta towards zero, which is why folding them over every
// candidate box in any order gives the same answer.
double calculateOffset(const AABB& blockBox, const AABB& mover, int axis, double delta)
{
    double moverMin[3] = {mover.minX, mover.minY, mover.minZ};
    double moverMax[3] = {mover.maxX, mover.maxY, mover.maxZ};
    double blockMin[3] = {blockBox.minX, blockBox.minY, blockBox.minZ};
    double blockMax[3] = {blockBox.maxX, blockBox.maxY, blockBox.maxZ};

    const int a = (axis + 1) % 3;
    const int b = (axis + 2) % 3;
    if (moverMax[a] <= blockMin[a] || moverMin[a] >= blockMax[a]) {
        return delta;
    }
    if (moverMax[b] <= blockMin[b] || moverMin[b] >= blockMax[b]) {
        return delta;
    }

    if (delta > 0.0 && moverMax[axis] <= blockMin[axis]) {
        const double gap = blockMin[axis] - moverMax[axis];
        if (gap < delta) {
            delta = gap;
        }
    }
    if (delta < 0.0 && moverMin[axis] >= blockMax[axis]) {
        const double gap = blockMax[axis] - moverMin[axis];
        if (gap > delta) {
            delta = gap;
        }
    }
    return delta;
}

// Folds every block box in `range` into one clipped delta on one axis.
//
// The original gathers the whole list once and walks it three times; this walks
// the same volume three times and gathers nothing. The result is identical --
// the range is computed once, before any axis moves, and each fold is a min or
// a max, so neither order nor repetition changes it -- and it keeps a fast
// player's swept volume, which can be a hundred and forty boxes, off a 32 KB
// stack that the 3DS build caps at 8 KB a frame.
double clipAxis(const tick::TickWorld& world, const BlockRange& range,
                const AABB& mover, int axis, double delta)
{
    AABB boxes[block::kMaxCollisionBoxes];
    for (i32 bx = range.x0; bx < range.x1; ++bx) {
        for (i32 bz = range.z0; bz < range.z1; ++bz) {
            // The original's chunk-loaded guard, and it is load-bearing rather
            // than an optimisation: an unloaded chunk contributes no boxes at
            // all, so a player who outruns the streamer falls through the
            // world exactly as they do in the original.
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
                    const AABB placed = boxes[i].offset(double(bx), double(by), double(bz));
                    delta = calculateOffset(placed, mover, axis, delta);
                }
            }
        }
    }
    return delta;
}

// True when the box, dropped a block, would land on nothing -- the test the
// sneak walk-back uses to find the edge of a ledge.
bool nothingBelow(const tick::TickWorld& world, const AABB& probe)
{
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

void PlayerBody::setFeet(double fx, double fy, double fz)
{
    x = fx;
    y = fy;
    z = fz;
    // Halved in float and widened once, the way `Entity.setPosition` does it.
    const double half = double(kPlayerWidth / 2.0f);
    box = AABB{fx - half, fy, fz - half,
               fx + half, fy + double(kPlayerHeight), fz + half};
    posY = fy + double(kEyeHeight) - double(ySize);
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

    if (onGround && sneaking) {
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
        dy = double(kStepHeight);
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
    posY = box.minY + double(kEyeHeight) - double(ySize);

    collidedHorizontally = origDx != dx || origDz != dz;
    collidedVertically = origDy != dy;
    onGround = origDy != dy && origDy < 0.0;

    if (onGround) {
        // Where the original would deal fall damage. That is a Survival rule
        // and it is not implemented; the distance is still tracked, so it is
        // there when it is.
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

    ySize *= kYSizeDecay;
}

namespace {

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

void PlayerBody::tick(const tick::TickWorld& world, const PlayerInput& input)
{
    sneaking = input.sneak;

    // `EntityLiving.onLivingUpdate` gates the jump on standing on something;
    // holding the button does not pump.
    if (input.jump && onGround) {
        jump();
    }

    const float frictionForAccel = groundFriction(world, *this);
    const float accel =
        kAccelNormaliser / (frictionForAccel * frictionForAccel * frictionForAccel);
    applyHeading(input.strafe, input.forward, input.yawDegrees,
                 onGround ? kGroundAcceleration * accel : kAirAcceleration);

    // Asked again, deliberately: `applyHeading` above can have taken the player
    // off the ground, and the original re-reads it here before damping.
    const float frictionForDamping = groundFriction(world, *this);

    move(world, motionX, motionY, motionZ);

    motionY -= kGravity;
    motionY *= kVerticalDrag;
    motionX *= double(frictionForDamping);
    motionZ *= double(frictionForDamping);
}


void PlayerBody::tickFlying(const tick::TickWorld& world, const PlayerInput& input, bool ascend,
                            bool descend, double speed)
{
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
    move(world, motionX, motionY, motionZ);

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
