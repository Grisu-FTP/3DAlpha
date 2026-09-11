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
// behaviour depends on something this port does not have yet -- a
// falling-block entity, or the drop table that says what a broken block leaves
// behind -- it is stated in place and the block does the part it can, rather
// than doing nothing quietly. (There *are* item entities now, in
// core/entity/item_entity.hpp; what is still missing is `idDropped` and
// `quantityDropped`, which is a table rather than an entity.)

#include "core/block/block_def.hpp"
#include "core/util/aabb.hpp"
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

// `Block.canPlaceBlockAt` and its overrides: **may this block go in this
// cell at all?**
//
// The base answer is "the cell is air or a liquid", and about a dozen classes
// add a condition on top -- a sapling wants dirt under it, a cactus wants sand
// and clear sides, a torch wants a face to hang on, a rail wants a solid floor.
// Every one of those conditions was already transcribed in this file, because
// the *tick* has to ask the same question when a neighbour changes: that is
// what makes a flower pop when you mine the dirt under it.
//
// **What was missing was anyone asking it at placement time.** The edit path
// tested `blockAt(...) == air` and nothing else, so a cactus could be put on
// glass, wheat on stone and a sapling in mid-air -- and the tick would then
// quietly delete them, which looks like the block failing to place rather than
// like a rule. Now the placement is refused up front, which is what the
// original does.
//
// A liquid in the cell is *replaceable*, so this also ends the "placement is
// air-only" deviation: water and lava can be built into, as they can in the
// game.
bool canPlaceAt(const TickWorld& world, block::BlockId id, i32 x, int y, i32 z);

// `Block.blockActivated` -- **what the block does when it is right-clicked**,
// and whether that click is finished with.
//
// `PlayerController.onPlayerRightClick` (`hq.a(Ldm;Lcn;Lev;IIII)Z`) is four
// lines, and their order is the whole rule:
//
// ```
// int id = world.getBlockId(i, j, k);
// if (id > 0 && Block.blocksList[id].blockActivated(world, i, j, k, player)) return true;
// if (itemstack == null) return false;
// return itemstack.useItem(player, world, i, j, k, l);
// ```
//
// **The block is asked first and there is no sneak override** -- that is a
// later version's -- so a door opens rather than taking a block to the face,
// and a hand holding nothing still opens it.
//
// Returning true means the click is spent: nothing is placed. A door, a lever
// and a button all consume it; **redstone ore does not**, because its override
// lights the ore and then returns the base class's false, so you can light a
// block of it and place against it in the same press.
//
// **Four blocks that consume a click in a1.1.2 do not here**, and it is one
// reason rather than four: a chest, a workbench, a furnace and a jukebox all
// answer a right-click by opening a screen, and there are no screens. Eating
// the click to show nothing would be indistinguishable from the placement being
// broken, so until those screens exist they are ordinary blocks to build
// against. That is a deviation and it is named; it reverses the day a container
// screen lands.
bool blockActivated(TickWorld& world, i32 x, int y, i32 z);

// `kh.c(DDD)V`'s tail -- **the block-collision scan**, and the only way
// anything in the world learns that an entity is touching it.
//
// Every block whose cell the box overlaps is handed
// `ly.b(Lcn;IIILkh;)V` -- onEntityCollidedWithBlock -- which is empty on the
// base class and is a pressure plate arming itself on `al`. Nothing else in
// a1.1.2 overrides it, so this is a one-block dispatch that will grow.
//
// **The bounds are the floors of both corners, inclusive.** `for (x =
// floor(minX); x <= floor(maxX); x++)`, not the half-open range every other
// box walk in this project uses -- so a box whose far face sits exactly on a
// boundary reaches one cell past it. Transcribed rather than tidied: it is the
// difference between a plate arming when you stand on its edge and not.
//
// **Not called from inside the body.** `moveEntity` is where the original puts
// it, and `PlayerBody::move` takes a `const TickWorld&` on purpose -- moving a
// body must not be able to write blocks. So the caller that owns the tick runs
// this immediately after the move, once per entity, which is the same order.
void entityCollidedWithBlocks(TickWorld& world, const AABB& box);

// `gb.b()`, the material predicate grass reads through the block above it and
// `World.getPrecipitationHeight` reads on the way down. Verified against a
// running jar to be exactly `Material.isSolid() || Material.isLiquid()` for
// every block a1.1.2 constructs, and false for air -- so it is derived from
// the table here rather than carried as a fourteenth boolean column.
bool solidOrLiquid(block::BlockId id);

}  // namespace mc::tick
