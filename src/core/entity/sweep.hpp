#pragma once

// **The block sweep every moving box shares** -- the half of
// `Entity.moveEntity` that asks the world what is in the way.
//
// It lived inside `PlayerBody` until a second thing needed to move: a digging
// particle is an `Entity` in a1.1.2 and runs the *same* `moveEntity`, so it
// bounces off the ground and slides down a slope for the same reason the player
// stops at a wall. Copying forty lines to say that a second time would have
// been two things to keep in step; this is one.
//
// What is **not** here is the rest of `moveEntity` -- the step up, the sneak
// probe, the fall distance, the footstep. Those are the player's, and a
// particle has none of them (its `stepHeight` is zero and it never sneaks). See
// core/entity/player_body.cpp for that half.
//
// Derived from `cn.a(Lkh;Lcf;)Ljava/util/List;` and `cf.a/b/c(Lcf;D)D`; the
// numbers and the argument order are argued at each function.

#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

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

BlockRange sweepRange(const AABB& swept);

// `AxisAlignedBB.calculateXOffset` and its two siblings. The receiver is the
// **block's** box and `mover` is the entity's, which is the way round the
// original calls them and the easiest thing here to invert by accident.
double calculateOffset(const AABB& blockBox, const AABB& mover, int axis, double delta);

// One axis of the sweep: the largest part of `delta` the mover can take before
// something in `range` stops it.
double clipAxis(const tick::TickWorld& world, const BlockRange& range,
                const AABB& mover, int axis, double delta);

}  // namespace mc::entity
