#pragma once

// Redstone: the wire that carries a signal, and the torch that inverts one.
//
// **What a "side" means here**, because everything below depends on it: the
// power queries name the face of the *asking* block that the answer arrives
// through, in a1.1.2's numbering -- 0 is -y, 1 is +y, 2 is -z, 3 is +z, 4 is
// -x, 5 is +x. A circuit built on the wrong numbering works in some directions
// and not others, which is the hardest kind of wrong to see.
//
// **Two questions, not one.** `providesPowerTo` is "does this block hand power
// out of that face itself". `indirectlyProvidesPowerTo` is the same question
// asked *through* solids: an opaque cube answers with whatever is powering it,
// which is the entire reason a torch under a block powers what stands on top.
// `TickWorld` owns both because the second has to be able to recurse into the
// first through a block.
//
// **The wire's strength is its metadata, 0 to 15.** A wire recomputes its own
// strength from its four horizontal neighbours -- reaching one block up when
// the neighbour is a solid it can climb, and one block down when it is not --
// takes the strongest, subtracts one, and writes it back. Fifteen blocks is
// therefore not a rule anywhere in the code; it is what falls out of starting
// at 15 and losing one per block.
//
// **The torch is the only active element in a1.1.2.** It is lit unless the
// block it is attached to is powered, which makes it a NOT gate, and every
// other circuit in the game is built out of that. It has a tick rate of 2,
// which is the delay, and a burnout rule -- eight toggles at one position
// inside 100 ticks and it stays off -- which is what stops a torch wired to
// itself from oscillating for ever.

#include "core/block/block_def.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `ly.c(Lcn;IIII)Z` -- Block.isProvidingPowerTo, dispatched on behaviour.
bool providesPowerTo(const TickWorld& world, i32 x, int y, i32 z, int side,
                     block::BlockId self);

// `ly.b(Lnm;IIII)Z` -- Block.isIndirectlyProvidingPowerTo.
bool indirectlyProvidesPowerTo(const TickWorld& world, i32 x, int y, i32 z, int side,
                               block::BlockId self);

// `kf.h(Lcn;III)V` -- updateAndPropagateCurrentStrength, the wire's whole
// behaviour. Public because onBlockAdded, onBlockRemoval and a neighbour
// change all call it.
void wirePropagate(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

void wireNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);
void wirePlaced(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);
void wireRemoved(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `bg.a(Lcn;IIILjava/util/Random;)V` -- the torch deciding whether to be lit.
void redstoneTorchTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                       JavaRandom& rand);
void redstoneTorchNeighbourChanged(TickWorld& world, i32 x, int y, i32 z,
                                   block::BlockId self);
void redstoneTorchPlaced(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);
void redstoneTorchRemoved(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `ai.a(...)` -- lit redstone ore going dark again.
void redstoneOreTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// ---- switches and the door -------------------------------------------
//
// **What needs a player and what does not.** Pressing a button, flipping a
// lever and opening a door by hand are inputs, and inputs need a player this
// port does not have yet. Everything *else* about these blocks is reachable
// and testable now: which sides they power when their metadata says they are
// on, whether they stay attached when their wall goes away, a button
// un-pressing itself twenty ticks later, and -- the one with a visible payoff
// -- a door opening and closing because a circuit told it to.
//
// Metadata is the same shape for a lever and a button: the low three bits name
// the face it is attached to (1 = -x, 2 = +x, 3 = -z, 4 = +z, 5 = the floor,
// which only a lever uses) and bit 3 is "on".

// `hu.a(Lcn;IIILjava/util/Random;)V` -- the button letting itself back out,
// twenty ticks after it was pressed. The only scheduled behaviour of the three.
void buttonTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `no.a(Lcn;IIII)V`, `hu.a(Lcn;IIII)V`, `al.a(Lcn;IIII)V` -- each drops when
// the thing it is attached to stops being an opaque cube.
void switchNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `fw.a(Lcn;IIII)V` -- a door checking that it is still whole and still
// standing on something, and then opening or closing to match the power around
// it. Both halves are kept in step from the lower one.
void doorNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                          block::BlockId fromId);

}  // namespace mc::tick
