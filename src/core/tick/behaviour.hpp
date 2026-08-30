#pragma once

// What each kind of block does when the world ticks it, and when a neighbour
// changes under it.
//
// Two entry points, matching the two the original has:
//
//   * `updateTick`      -- `Block.updateTick(World, x, y, z, Random)`, reached
//                          either by a random tick or by a scheduled one.
//   * `neighbourChanged` -- `Block.onNeighborBlockChange(World, x, y, z, id)`,
//                          reached from `World.notifyBlocksOfNeighborChange`.
//
// Dispatch is a `switch` on `BlockDef::tick`, never on a block id, for exactly
// the reason the mesher switches on `RenderType`: the behaviour of grass is a
// fact about a1.1.2's `BlockGrass` class, and which numeric id that class was
// registered under is a fact about the version. Behaviours that need to *name*
// a block -- grass turning into dirt -- use the generated `mcver::Block`
// enumerators, which is the one place a name is allowed to become a number.
//
// **Every rule here was read out of the client jar**, method by method, and
// the obfuscated name it came from is quoted above each one. Where a1.1.2's
// behaviour depends on something this port does not have yet -- item entities
// to drop, a falling-block entity, redstone power -- it is stated in place and
// the block does the part it can, rather than doing nothing quietly.

#include "core/block/block_def.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `Block.updateTick`. `id` is the block that is actually there, already read.
void updateTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId id, JavaRandom& rand);

// `Block.onBlockAdded` (`ly.e(Lcn;III)V`) -- run by `Chunk.setBlockID` on the
// block that has just appeared, and **not** the same thing as a neighbour
// notification. It is what schedules a flowing fluid and a sand block: without
// it a fluid spreads one block and stops for ever, which is exactly the shape
// of bug it caused here before this existed.
void blockAdded(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `Block.onBlockRemoval` (`ly.b(Lcn;III)V`) -- run on the block being replaced,
// while its metadata is still readable. Empty on the base class and overridden
// by the containers and by redstone, none of which are ported, so this is a
// named hook rather than behaviour.
void blockRemoved(TickWorld& world, i32 x, int y, i32 z, block::BlockId old);

// `World.notifyBlockOfNeighborChange` -> `Block.onNeighborBlockChange`.
// `fromId` is the block that changed, which a1.1.2 passes along and which only
// redstone reads.
void neighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId fromId);

// `BlockSnow.canPlaceBlockAt` -- needed by the world's own snow pass as well
// as by the block, which is why it is public.
bool canPlaceSnowAt(const TickWorld& world, i32 x, int y, i32 z);

// `gb.b()`, the material predicate grass reads through the block above it and
// `World.getPrecipitationHeight` reads on the way down. Verified against a
// running jar to be exactly `Material.isSolid() || Material.isLiquid()` for
// every block a1.1.2 constructs, and false for air -- so it is derived from
// the table here rather than carried as a fourteenth boolean column.
bool solidOrLiquid(block::BlockId id);

}  // namespace mc::tick
