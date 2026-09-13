#pragma once

// **The entity half of `cn.a(Lkh;Lcf;)Ljava/util/List;`** --
// `World.getCollidingBoundingBoxes`, which is not only a block loop.
//
// After it has gathered the blocks in the swept volume it asks every entity
// within a quarter of a block two questions, and **every `moveEntity` in the
// game clips against the answers**:
//
//     AABB b = e.getBoundingBox();          // kh.f_()  -- the neighbour's
//     if (b != null && b.intersectsWith(box)) list.add(b);
//     AABB c = entity.getCollisionBox(e);   // kh.b_(kh) -- the MOVER's
//     if (c != null && c.intersectsWith(box)) list.add(c);
//
// **`f_()` is answered by exactly two classes**, and all 402 in this jar were
// disassembled to be sure of it: `dc` (EntityBoat) and `oc` (EntityMinecart)
// both return their own `boundingBox`, while `kh`'s own -- inherited by every
// mob, item, arrow, painting and particle -- returns null. That is the whole
// reason a cow can be walked through and a minecart cannot, and why "you can
// stand on top of a minecart" is a statement about two classes rather than
// about entities in general.
//
// **`b_(kh)` is the other side of the same coin.** It is asked of the thing
// doing the moving, not of the neighbour, and `dc.b_` and `oc.b_` are one
// instruction each: `return e.boundingBox`, with no null, liveness or
// can-be-collided-with test in front of it. So a boat and a cart are not only
// solid to everything, they are stopped by everything -- the cow standing on
// the track, the item lying on it, the arrow stuck in the gravel, the player
// waiting at the station. Nothing else in the game has this: `kh.b_` is null,
// so the branch is empty for every other mover.
//
// Which pools that means is a question about a1.1.2's *world* entity list,
// because `getEntitiesWithinAABBExcludingEntity` walks the chunks'. Every
// entity here is in it but one: `nq` (EntityFX) goes into `bq`'s own
// `List[]` -- `bq.a(nq)` is `lists[particle.getFXLayer()].add(particle)` and
// nothing else -- so **a cart is not stopped by smoke**, and the particle pool
// is deliberately absent below.
//
// The pools belong to the frame loop, the way the particles and the dropped
// items do, so this is the seam that ties them to the world -- same shape as
// `bindParticles`, and a harness that installs nothing gets a world in which
// nothing is solid but the blocks.
//
// **What it costs**: with the boats and carts empty, which is most worlds most
// of the time, a moving player pays a null check and two empty loops per axis
// and allocates nothing -- the second half is not walked at all, because only a
// boat or a cart asks for it. A moving cart pays one box test per live entity
// per axis, which is the same shape as the original's list, and there are at
// most a few hundred of those in a loaded world. If it ever shows up on a frame
// graph the fix is a per-chunk index on the pools, not a cap here. No hardware
// measurement has been taken.

#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

class ArrowSystem;
class BoatSystem;
class FallingBlockSystem;
class ItemEntitySystem;
class MinecartSystem;
class MobSystem;
class PaintingSystem;
class PrimedTntSystem;
struct PlayerBody;

// The world's entity list, as the sweep will ask about it. Every pointer may
// be null, which means there are none of that kind.
//
// **It must outlive the world it is bound to**, which it does for the one
// caller that matters: the pools and the world both belong to the frame loop.
struct EntityBoxes {
    // **The two that are solid to everything** -- the `f_()` half, which every
    // mover sees.
    const BoatSystem* boats = nullptr;
    const MinecartSystem* minecarts = nullptr;

    // **The rest of the world entity list** -- the `b_(kh)` half, which only a
    // boat or a minecart ever asks for. The boats and the carts are in this
    // half too; they are already above, and folding the same box twice changes
    // nothing, so they are not repeated.
    const MobSystem* mobs = nullptr;
    const ItemEntitySystem* items = nullptr;
    const ArrowSystem* arrows = nullptr;
    const PaintingSystem* paintings = nullptr;
    const PrimedTntSystem* primedTnt = nullptr;
    const FallingBlockSystem* fallingBlocks = nullptr;

    // **The player**, which is one body rather than a pool -- the same shape
    // `ArrowTargets` takes and for the same reason. A cart rolling at someone
    // standing on the track stops at them.
    //
    // A *rider* is in this list too, because
    // `getEntitiesWithinAABBExcludingEntity` leaves out only the mover itself
    // and not whatever is sitting on it. That costs a ridden cart nothing, and
    // the jar is why: `oc.h()` and `dc.h()` are both `height * 0.0 - 0.3`, so
    // a rider's feet are placed 0.3 *below* the vehicle's centre and its box
    // overlaps the vehicle's on all three axes -- and `calculateOffset` clips
    // only against a box that is ahead and clear, never one already overlapping.
    const PlayerBody* player = nullptr;
};

// Installs `entities` as the world's solid-box query. Null clears it, which
// puts the sweep back to blocks alone.
void bindEntityBoxes(tick::TickWorld& world, const EntityBoxes* entities);

}  // namespace mc::entity
