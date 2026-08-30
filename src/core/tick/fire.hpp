#pragma once

// Fire: how it ages out, what it sets alight, and how far it jumps.
//
// Its own file for the same reason the fluids have one -- it carries two
// per-block tables of its own and a spread rule with more shape to it than a
// dispatch case can hold.
//
// **Metadata is age, 0 to 15**, and it climbs by one every scheduled tick.
// Fire does not die of old age: at 15 it stops counting and burns for ever as
// long as something beside it is still burnable. What kills it is running out
// of fuel -- and even then only if it is not standing on solid ground, or if it
// has aged past 3.
//
// **Three different questions about burning, and a1.1.2 asks all three of
// different tables.** `BlockDef::burnEncourage` (non-zero) is what "there is
// something to burn here" means to a fire block; `BlockDef::burnCatch` is
// rolled to decide whether that block is actually consumed; and
// `BlockDef::canBurn` -- the material's own answer, a wider set of fourteen
// blocks including chests, signs and fences -- is what *lava* reads when it
// looks for something to set alight. Six blocks are in the first two tables and
// fourteen in the third, so treating them as one lights the wrong things.
//
// **Fire spreads downward more eagerly than upward**, which is worth saying out
// loud because it reads backwards: the per-direction chance is the *bound* of a
// roll that has to come in under the target's `burnCatch`, so a lower number
// means more likely. Below is 200, above is 250, and the four sides are 300.

#include "core/block/block_def.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `og.a(Lcn;IIILjava/util/Random;)V` -- BlockFire.updateTick.
void fireTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self, JavaRandom& rand);

// `og.a(Lcn;IIII)V` -- onNeighborBlockChange: fire with nothing under it and
// nothing beside it to burn goes out.
void fireNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `og.e(Lcn;III)V` -- onBlockAdded.
void firePlaced(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `og.a(Lcn;III)Z` -- BlockFire.canPlaceBlockAt. Public because lava's
// ignition search asks the same question before it sets anything alight.
bool fireCanBeAt(const TickWorld& world, i32 x, int y, i32 z);

}  // namespace mc::tick
