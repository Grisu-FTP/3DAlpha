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
namespace detail {

// **Which ids are a unit cube whatever their metadata**, folded at compile time
// out of the generated selection index.
//
// This exists for one reason and it is a measured one. The mesher has to know
// whether a standard block fills its cell before it can pick between the cube
// stream and the box path, and asking `selectionBox` per block cost **13.6 us a
// section** on the dev host -- 22.8 to 36.4 in face emit, a 60 % rise for the
// 36 extra quads a whole 1,119-column world produced. The question is almost
// always "yes, a unit cube", it depends only on the id for all but one block in
// a1.1.2, and a bool per id answers it with one load.
//
// Shape 0 is the unit cube -- `tools/configure.py` refuses to emit a table
// where it is not -- so "every metadata maps to shape 0" is the whole test.
struct UnitCubeTable {
    bool always[mcver::kSelectionIndexSize];
};

constexpr UnitCubeTable buildUnitCubeTable()
{
    UnitCubeTable table{};
    for (int id = 0; id < mcver::kSelectionIndexSize; ++id) {
        bool all = true;
        for (int metadata = 0; metadata < 16; ++metadata) {
            if (mcver::kSelectionIndex[id][metadata] != 0) {
                all = false;
            }
        }
        table.always[id] = all;
    }
    return table;
}

inline constexpr UnitCubeTable kUnitCubeTable = buildUnitCubeTable();

}  // namespace detail

// Whether this block's render bounds fill its cell for every metadata value.
// True for an id past the table, which is the unknown block and is drawn as a
// full cube everywhere else too.
constexpr bool selectionIsAlwaysUnitCube(BlockId id)
{
    return id >= mcver::kSelectionIndexSize || detail::kUnitCubeTable.always[id];
}

inline AABB selectionBox(BlockId id, u8 metadata)
{
    const int shape = id < mcver::kSelectionIndexSize
                          ? int(mcver::kSelectionIndex[id][metadata & 15])
                          : 0;
    const float* b = mcver::kSelectionShapes[shape];
    return AABB{double(b[0]), double(b[1]), double(b[2]),
                double(b[3]), double(b[4]), double(b[5])};
}

// **The shape a block is drawn as when it is an item**, which is neither of the
// other two: `RenderBlocks.renderBlockAsItem` calls
// `Block.setBlockBoundsForItemRender` on the singleton and draws what that
// leaves behind. There is no metadata anywhere on that path -- a hand, an
// inventory slot and a dropped stack all hold a bare id -- so this table is one
// box per block.
//
// `Block`'s own method is empty, so for all but three blocks this is the
// constructor's bounds and agrees with `selectionBox(id, 0)`. The three are the
// button and the two pressure plates, whose classes override it, and they are
// exactly the blocks whose world shape is written in
// `setBlockBoundsBasedOnState` and is therefore a **leftover** at metadata 0 --
// which is why reading the selection table for them put a full stone cube in
// the hand where a1.1.2 draws a button. Measured, not reasoned: see
// `kItemRenderBoxes` in tests/collision_box_vectors.hpp.
inline AABB itemRenderBox(BlockId id)
{
    const int shape = id < mcver::kSelectionIndexSize ? int(mcver::kItemRenderIndex[id]) : 0;
    const float* b = mcver::kSelectionShapes[shape];
    return AABB{double(b[0]), double(b[1]), double(b[2]),
                double(b[3]), double(b[4]), double(b[5])};
}

// **What the struck face makes of a block a player puts down.**
// `Block.onBlockPlaced`, which `ItemBlock.onItemUse` runs straight after
// `setBlockWithNotify` -- so a torch clicked onto a wall becomes a wall torch.
//
// Six blocks have one: the torch, both redstone torches, the ladder, the lever
// and the button. Every other row is 0. **A furnace and a staircase are not
// here**: they turn from their neighbours in onBlockAdded, which is
// `tick::blockAdded`, and nothing in a1.1.2 reads the player's heading. See
// docs/physics-a1.1.2.md.
inline u8 placementMetadata(BlockId id, int face)
{
    if (id >= mcver::kPlacementTableSize || face < 0 || face > 5) {
        return 0;
    }
    return mcver::kPlacementMetadata[id][face];
}

}  // namespace mc::block
