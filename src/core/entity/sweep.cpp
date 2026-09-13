// The shared block sweep. See sweep.hpp.

#include "core/entity/sweep.hpp"

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

// The sink `clipAxis` hands `forEachSolidBox`, and the one number it carries
// back. A free function and a struct rather than a capturing lambda, because
// the seam is a plain function pointer -- see core/tick/tick_world.hpp.
struct Fold {
    const AABB* mover;
    int axis;
    double delta;
};

void foldSolidBox(void* ctx, const AABB& box)
{
    Fold* fold = static_cast<Fold*>(ctx);
    fold->delta = calculateOffset(box, *fold->mover, fold->axis, fold->delta);
}

}  // namespace

BlockRange sweepRange(const AABB& swept)
{
    BlockRange r;
    r.x0 = MathHelper::floorDouble(swept.minX);
    r.x1 = MathHelper::floorDouble(swept.maxX + 1.0);
    r.y0 = MathHelper::floorDouble(swept.minY) - 1;
    r.y1 = MathHelper::floorDouble(swept.maxY + 1.0);
    r.z0 = MathHelper::floorDouble(swept.minZ);
    r.z1 = MathHelper::floorDouble(swept.maxZ + 1.0);
    r.swept = swept;
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
                const AABB& mover, int axis, double delta, const Mover& who)
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

    // **The rest of the same list**: a boat and a minecart hand
    // `getCollidingBoundingBoxes` their own bounding box, so they are folded in
    // here exactly as a block's box is -- same function, same clamps, same
    // indifference to order. This is the whole of standing on a minecart. When
    // the mover is itself a boat or a cart the list also holds every other
    // entity near it, which is `getCollisionBox`; see `Mover`.
    //
    // The mover's box is the one this axis is being asked about, not the one
    // the move started from, which is why the fold is here rather than gathered
    // once by the caller: the original walks its list three times for the same
    // reason.
    Fold fold{&mover, axis, delta};
    world.forEachSolidBox(range.swept, who.self, who.collidesWithEntities, &foldSolidBox,
                          &fold);
    return fold.delta;
}

}  // namespace mc::entity
