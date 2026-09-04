#pragma once

// What a block is shaped like to something walking into it -- the collision
// boxes for a (block, metadata) pair, with no world and no allocation.
//
// **It takes no world, and that is a measured fact rather than a convenience.**
// Every collision box in a1.1.2 is a pure function of the block's own id and
// its own metadata. Nothing consults a neighbour. The obvious counterexample
// looked like the top half of a door, which plainly ought to ask the half below
// it which way the hinge is -- it does not; it reads its own low three bits and
// ignores the top-half flag entirely. Fences do not connect either. This was
// established by asking a running jar for all 1,120 combinations, and the
// answers are checked in as tests/collision_box_vectors.hpp.
//
// The consequence is worth the paragraph: collision needs no chunk lookup
// beyond reading the block and its metadata, the resolver is testable with no
// world at all, and the per-frame path allocates nothing.
//
// **Dispatch is on `block::Shape`, never on a block id**, for the same reason
// the mesher dispatches on RenderType and the ticker on TickBehaviour. See
// CONTRIBUTING.md.

#include "core/block/block_def.hpp"
#include "core/util/aabb.hpp"
#include "placement.hpp"  // generated; see tools/configure.py
#include "selection.hpp"  // generated; see tools/configure.py

namespace mc::block {

// Stairs are the only shape in a1.1.2 that answers with more than one box, and
// they answer with two. The generator asserts this when it builds the fixture,
// so a version that grows a three-box block fails there rather than here.
inline constexpr int kMaxCollisionBoxes = 2;

// Boxes in **block-local** coordinates: 0,0,0 is the block's own corner, so a
// full cube is 0,0,0 -> 1,1,1 and a fence rises to 1.5. Add the block position
// to put them in the world.
//
// Returns how many boxes were written, which is 0 for everything that does not
// collide -- air, fluids, plants, torches, rails, snow and fire. Never writes
// more than `max`, and never more than kMaxCollisionBoxes.
int collisionBoxes(BlockId id, u8 metadata, AABB* out, int max);

// **The shape a ray is tested against, which is not the shape you walk into.**
// `Block.collisionRayTrace` sets the block's own bounds and tests those, and
// for a torch those are a small post while its collision box does not exist at
// all -- which is exactly why a torch can be aimed at and broken but not stood
// on. Always one box, never none.
//
// Generated data rather than a switch: seventy blocks make eighteen families,
// seven of which change with metadata, and hand-transcribing that is how a
// wrong number survives a year. See tools/gen_selection.py.
inline AABB selectionBox(BlockId id, u8 metadata)
{
    const int shape = id < mcver::kSelectionIndexSize
                          ? int(mcver::kSelectionIndex[id][metadata & 15])
                          : 0;
    const float* b = mcver::kSelectionShapes[shape];
    return AABB{double(b[0]), double(b[1]), double(b[2]),
                double(b[3]), double(b[4]), double(b[5])};
}

// **Which way a block ends up facing when a player puts it down.**
// `Block.onBlockPlaced`, which `ItemBlock.onItemUse` runs straight after
// `setBlockWithNotify` -- so a torch clicked onto a wall becomes a wall torch
// and a staircase faces the way it was clicked.
//
// **The face, not the player.** Measured, and it surprised: only the lever
// consults the player's heading in a1.1.2, and only on its top face. Stairs,
// furnaces, ladders and buttons all take the struck face and nothing else,
// which is why this needs no yaw. See docs/physics-a1.1.2.md.
inline u8 placementMetadata(BlockId id, int face)
{
    if (id >= mcver::kPlacementTableSize || face < 0 || face > 5) {
        return 0;
    }
    return mcver::kPlacementMetadata[id][face];
}

}  // namespace mc::block
