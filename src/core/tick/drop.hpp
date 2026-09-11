#pragma once

// **What a block leaves on the ground when it goes** -- `ly.b_(Lcn;IIIII)V`,
// which is `dropBlockAsItem`, and the two generated methods it reads.
//
// This is the half of block removal that had a name and no body. `behaviour.cpp`
// has called `dropBlockAsItem` at nine sites since the tick landed -- the reed
// whose sand went, the cactus with a wall against it, the snow with nothing
// under it, the torch, the plant, the rail -- and every one of them was a stub
// that returned. So a1.1.2's most ordinary consequence did not happen: knock a
// block out from under a torch and the torch was simply deleted.
//
// The method itself is four lines:
//
// ```
// int count = quantityDropped(world.rand);
// for (int n = 0; n < count; n++) {
//     if (world.rand.nextFloat() > chance) continue;         // chance is 1.0F here
//     int id = idDropped(metadata, world.rand);
//     if (id <= 0) continue;
//     float f = 0.7F;
//     double dx = world.rand.nextFloat() * f + (1 - f) * 0.5;   // and the same for y, z
//     EntityItem e = new EntityItem(world, i + dx, j + dy, k + dz, new ItemStack(id));
//     e.delayBeforeCanPickup = 10;
//     world.spawnEntityInWorld(e);
// }
// ```
//
// Three things in it are load-bearing and none is obvious:
//
//   * **The draws come out of the world's own Random**, so a drop shifts the
//     stream every later random tick reads. That is why this takes a
//     `TickWorld&` and not a random of its own: getting it from somewhere else
//     would make a world that dropped a torch generate different flowers.
//   * **`new ItemStack(id)` -- one item, damage zero.** `damageDropped` is
//     never called on this path in a1.1.2, so wool does not keep its colour and
//     a log does not keep its kind. See data/<version>/drops.json.
//   * **`nextFloat() > chance` is evaluated even at chance 1.0**, and it
//     consumes a draw per item dropped whether or not it can ever fail. Skipping
//     it would be one line shorter and a different world.
//
// The per-block numbers are generated -- `tools/genref.java --drops` -- because
// `idDropped` and `quantityDropped` take a Random and cannot be read as
// constants. See the header of that emitter for how they are characterised.

#include "core/block/block_def.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `ly.b_(Lcn;IIIII)V` -- dropBlockAsItem, which is `dropBlockAsItemWithChance`
// at chance 1. `self` is the block that was there and `metadata` is what it had;
// both are the caller's because by the time a1.1.2 calls this the cell has
// usually already been written to air.
//
// Does nothing at all when no drop sink is set, which is the honest answer for
// every headless tool here -- see `TickWorld::setDropSink`. It still consumes
// the same draws either way, so a world ticked with no entity pool stays the
// same world.
void dropBlockAsItem(TickWorld& world, i32 x, int y, i32 z, block::BlockId self, u8 metadata);

// `ly.b(Lcn;IIII)V` -- **onBlockDestroyedByPlayer**, the hook `hq.b(IIII)Z`
// runs on the block it has just written to air, with the metadata it read
// before the write.
//
// It is empty on `Block` and there are exactly **three** overrides in a1.1.2,
// which is why this was easy to miss and why it is worth naming all three:
//
//   * `hd` (BlockCrops) -- **three rolls for a seed**, and this is the one that
//     had a visible consequence: a crop's `idDropped` answers only at growth
//     stage 7, so breaking anything younger left nothing at all on the ground.
//     The seeds are not in the drop table because they are not `idDropped`;
//     they are this method.
//   * `km` (BlockStairs) -- forwards to the model block, whose own override is
//     the empty one. Nothing to do.
//   * `q` (BlockTNT) -- primes it. There is no primed-TNT entity in this build
//     and `core/tick/fire.cpp` already names the same deviation at the other
//     call site; it is named again here rather than left silent.
//
// **Its draws come out of the world's random**, exactly as `dropBlockAsItem`'s
// do, so a build that skipped it would generate different flowers downstream.
//
// `self` and `metadata` are the caller's, for the same reason they are above:
// the cell is already air by the time a1.1.2 calls this.
void blockDestroyedByPlayer(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                            u8 metadata);

}  // namespace mc::tick
