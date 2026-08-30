#pragma once

// Water and lava: how they spread, how they settle, and what they do to each
// other.
//
// Its own file because it is the largest single block behaviour in a1.1.2 --
// three classes, `jp` (BlockFluid), `hv` (BlockFlowing) and `hn`
// (BlockStationary), and a recursive flow-cost search -- and because the mesher
// already gives fluids a file of their own for the same reason.
//
// **The shape, before the detail.** Every fluid block is one of a pair: a
// *flowing* block that ticks and a *still* block that does not. Metadata is the
// flow level, 0 at a source and rising to 7 as it thins, with bit 3 (a value of
// 8 or more) marking water that is falling rather than spreading. A flowing
// block that finds nothing to change turns itself into the still block and
// stops costing anything; a still block that hears a neighbour change turns
// back into the flowing one and schedules itself. That pair is why a lake is
// free and a waterfall is not.
//
// **Where the levels come from.** The block recomputes its own level from its
// four horizontal neighbours and the block above every tick: the lowest
// neighbouring level plus 1 for water and 2 for lava, or the level of whatever
// is falling into it from above, or -1 -- vanish -- if nothing feeds it. Two
// adjacent sources make a new source, which is what makes an infinite water
// pool. Lava climbs towards a *higher* level only on one roll in four, which is
// what makes it visibly slower to settle than water at the same tick rate.
//
// **Which way it goes** is not "all four ways". `getOptimalFlowDirections`
// searches up to four blocks ahead down each of the four horizontal directions
// for somewhere the fluid could fall, and spreads only along the directions
// tied for the shortest such path. That search is why water finds a hole
// across the room instead of creeping outwards evenly, and it is the part of
// this file with a cost worth watching: it is recursive, four-way, and
// depth-limited at 4.

#include "core/block/block_def.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `hv.a(Lcn;IIILjava/util/Random;)V` -- BlockFlowing.updateTick.
void fluidFlowingTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                      JavaRandom& rand);

// `hn.a(Lcn;IIILjava/util/Random;)V` -- BlockStationary.updateTick, which is
// lava setting light to what is above it and nothing else. Water's still form
// does not tick randomly at all.
void fluidStillTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                    JavaRandom& rand);

// `jp.a(Lcn;IIII)V` and `hn.a(Lcn;IIII)V` -- onNeighborBlockChange for both
// forms: lava checks whether it has been quenched, and a still block wakes up.
void fluidNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `jp.e(Lcn;III)V` / `hv.e(Lcn;III)V` -- onBlockAdded. Not reached yet (nothing
// places blocks outside the tick), but it is what a player's bucket will call.
void fluidPlaced(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

}  // namespace mc::tick
