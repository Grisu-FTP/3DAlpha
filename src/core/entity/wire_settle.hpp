#pragma once

// **A body placed from a position that crossed the link, lifted back onto the
// floor it was resting on.**
//
// Entity positions cross in thirty-seconds of a block, floored -- a1.1.2's
// own `floor(posX * 32)`, which `toWirePosition` keeps. That rounds downward,
// and a body whose position is not a whole number of thirty-seconds above its
// own bottom arrives a little lower than it was: a boat resting on a floor at
// 65 sits at 65.3, which goes out as 65.28125, with its bottom 0.019 inside the
// floor. Drawn, nobody can see that. **Simulated, it falls through a whole
// block**: a sweep clips only against a box that is ahead of the mover, never
// one it already overlaps, so the floor under it is not there any more.
//
// That happens exactly where a vehicle changes hands -- a guest taking the
// wheel of the host's boat, or the host taking it back where the guest left
// it -- because those are the two places a body that was being drawn from the
// wire starts being simulated from it. So both mark it, and the first local
// tick lifts it by no more than the rounding could have taken.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::entity {

// One thirty-second and a hair: the most a floored wire position can lose.
inline constexpr double kWireSettleTolerance = 1.0 / 32.0 + 1e-6;

// How far `box` must rise to stand on top of what it is sunk into, when that is
// no more than `tolerance`; zero when it is clear, or buried deeper than any
// rounding could explain (then it is somebody else's problem, not this one's).
template <class Access>
double wireSettleLift(const Access& world, const AABB& box,
                      double tolerance = kWireSettleTolerance)
{
    const BlockRange range = sweepRange(box);
    AABB boxes[block::kMaxCollisionBoxes];
    double lift = 0.0;
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
                    const AABB solid = boxes[i].offset(double(bx), double(by), double(bz));
                    if (!solid.intersects(box)) {
                        continue;
                    }
                    const double sunk = solid.maxY - box.minY;
                    if (sunk > tolerance) {
                        return 0.0;
                    }
                    if (sunk > lift) {
                        lift = sunk;
                    }
                }
            }
        }
    }
    return lift;
}

}  // namespace mc::entity
