#pragma once

// **The collision sweep every moving box shares** -- the half of
// `Entity.moveEntity` that asks the world what is in the way: the blocks in the
// swept volume, and then the two entities a1.1.2 makes solid.
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
//
// It carries the box it was built from as well, because the list
// `getCollidingBoundingBoxes` returns has a second half: the boxes of the
// entities that are solid, which are asked for by box and not by block. The
// integer volume is no use for that question -- it is a block wider on five
// sides and a block taller on one -- so the double one comes along. See
// `TickWorld::forEachSolidBox`.
struct BlockRange {
    i32 x0, x1;
    int y0, y1;
    i32 z0, z1;
    AABB swept{};
};

BlockRange sweepRange(const AABB& swept);

// `AxisAlignedBB.calculateXOffset` and its two siblings. The receiver is the
// **block's** box and `mover` is the entity's, which is the way round the
// original calls them and the easiest thing here to invert by accident.
double calculateOffset(const AABB& blockBox, const AABB& mover, int axis, double delta);

// `getCollidingBoundingBoxes`'s first argument, as much of it as the sweep
// needs: **who is moving**. Both halves of it are facts about the mover and
// neither is a fact about what it might hit, which is why they travel together.
//
// `self` is the identity left out of the entity's own collision list. Only a
// boat and a minecart need to give one, because they are the only two things
// in a1.1.2 that can be in that list at all; everything else has nothing to
// exclude. Pointer identity is enough because the pools never move an element
// within a tick -- see core/util/segmented_pool.hpp.
//
// `collidesWithEntities` is `kh.b_(kh)`, getCollisionBox, asked of the mover.
// It is null on `kh`, so a player, a mob, an item, an arrow or a falling block
// leaves it false and collides with nothing but the boats and carts every
// mover sees. `dc` and `oc` both answer `return e.boundingBox` for any `e`,
// which makes a moving boat or minecart collide with **every** entity near it
// -- the cow on the track stops the cart, and so does the dropped item, the
// arrow stuck in the ground and the player standing in front of it.
struct Mover {
    const void* self = nullptr;
    bool collidesWithEntities = false;
};

// One axis of the sweep: the largest part of `delta` the mover can take before
// something in `range` stops it -- blocks, and then the entities in the same
// list.
double clipAxis(const tick::TickWorld& world, const BlockRange& range,
                const AABB& mover, int axis, double delta, const Mover& who = Mover{});

}  // namespace mc::entity
