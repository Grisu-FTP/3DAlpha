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

// `ly.d()Z` -- Block.canProvidePower, derived from the behaviour column
// because it is a property of the class in the original rather than a table.
//
// **Public because the renderer asks it too.** `kf.b(Lnm;III)Z` --
// isPowerProviderOrWire, the test a wire uses to decide which neighbours it is
// wired to -- is also the test `RenderBlocks` uses to decide which neighbours
// the wire is *drawn* reaching towards. Two copies of the list is how a wire
// ends up drawn connected to something it does not power.
bool canProvidePower(block::BlockId b);

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

// `al.h(Lcn;III)V` -- **setStateIfMobInteractsWithPlate**, and the whole of
// what a pressure plate does.
//
// It looks in a box an eighth of a block inset on the four sides and a quarter
// of a block tall, asks the world whether anything of the plate's own kind is
// standing in it, and arms or disarms to match -- writing metadata 1 or 0,
// telling its own neighbours and the block underneath, and clicking. **While
// something is standing on it, it re-schedules itself**, which is the twenty
// ticks a plate stays down after you step off.
//
// Both of its callers are here rather than inside it, because they gate on
// different things: `al.b(Lcn;IIILkh;)V` (onEntityCollidedWithBlock) runs it
// only when the plate is *up*, and `al.a(Lcn;IIILjava/util/Random;)V`
// (updateTick) only when it is *down*. Between them that is "arm on contact,
// disarm on the timer" and nothing else.
void pressurePlateSense(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `al.a(Lcn;IIILjava/util/Random;)V` -- the scheduled half: a plate that is
// down looks again.
void pressurePlateTick(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `al.b(Lcn;IIILkh;)V` -- the contact half: a plate that is up looks again.
void pressurePlateCollided(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `fw.a(Lcn;IIII)V` -- a door checking that it is still whole and still
// standing on something, and then opening or closing to match the power around
// it. Both halves are kept in step from the lower one.
void doorNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                          block::BlockId fromId);

// **What a hand does**, as against what redstone does. These are the four
// `blockActivated` overrides that are mechanisms; `tick::blockActivated`
// dispatches to them and is the only thing that should call them, because it
// is also what knows the order a right-click goes in.
//
// The three that return `bool` return **whether the click was consumed**, which
// is the original's contract and is what stops a lever being buried under the
// block you were holding.

// `fw.a(Lcn;IIILdm;)Z` -- open or close a door, from either half. An iron door
// consumes the click and does nothing.
bool doorActivated(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `no.e(Lcn;III)V` -- **BlockLever.onBlockAdded**, which gives a lever an
// orientation from whatever wall is beside it. Only ever fills in a zero, for
// the ordering reason the definition gives.
void leverPlaced(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `no.a(Lcn;IIILdm;)Z` -- flick a lever, and wake the block it hangs on.
bool leverActivated(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `hu.a(Lcn;IIILdm;)Z` -- press a button, and schedule the release. A button
// already in is a click consumed and nothing else.
bool buttonActivated(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

// `ai.h(Lcn;III)V` -- light redstone ore. **Not** a consumed click: the
// override lights the ore and then returns the base class's `false`.
void redstoneOreActivated(TickWorld& world, i32 x, int y, i32 z, block::BlockId self);

}  // namespace mc::tick
