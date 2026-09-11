#pragma once

// **Which way a rail lies, and what it does when the track beside it changes**
// -- `if` (BlockMinecartTrack) and `mk` (RailLogic), the second of which is the
// largest single behaviour in a1.1.2's block set and the reason rails were the
// last shape in this port still drawn but not wired.
//
// Without it a rail was a decoration: every one lay north-south whatever was
// next to it, nothing curved, and a staircase of rails did not exist because
// nothing ever wrote metadata 2..5. The shape is not a placement decision at
// all -- `Block.onBlockPlaced` returns 0 for a rail, which is why
// `data/a1.1.2/placement.json` has six zeroes on that row -- it is worked out
// afterwards, from the neighbours, by this.
//
// **The whole of it is one question asked four ways**: is there a rail I can
// join at each of my four sides, counting one block up and one block down?
// `mk` answers it by building a *second* RailLogic at the candidate, refreshing
// that one's own connections, and asking whether it has room for another.
// Nothing here is recursive past that second frame, which matters on a 32 KB
// stack: `refreshTrackShape` builds neighbours, and a neighbour's
// `refreshConnectedTracks` builds *its* candidates but only to read them.
//
// Three things in it are worth knowing before reading the code:
//
//   * **A freshly placed rail is written with metadata 15 first.** `if.e` does
//     that before it refreshes, and 15 is not a shape the game ever draws --
//     `setBasicRail` matches none of its ten cases and leaves the connection
//     list empty. It is "I have no previous shape to be biased by", spelled as
//     a metadata value because a1.1.2 has nowhere else to put it.
//   * **Power reorders the two curve preferences.** `refreshTrackShape` takes
//     `isBlockIndirectlyGettingPowered` as its argument and uses it only to
//     flip the order the four corner cases are tried in, which decides which
//     way a T-junction points. That is a1.1.2's rail switch, two years before
//     powered rails.
//   * **A rail whose support goes drops itself**, and so does an ascending one
//     whose *uphill* neighbour goes -- which is the block the slope leans on.
//
// The shapes, and they are the same numbering the mesher draws from:
//
// | metadata | shape |
// |---|---|
// | 0 | flat, along z |
// | 1 | flat, along x |
// | 2 | ascending towards +x |
// | 3 | ascending towards -x |
// | 4 | ascending towards -z |
// | 5 | ascending towards +z |
// | 6 | curve joining +x and +z |
// | 7 | curve joining -x and +z |
// | 8 | curve joining -x and -z |
// | 9 | curve joining +x and -z |

#include "core/block/block_def.hpp"
#include "core/util/types.hpp"

namespace mc::tick {

class TickWorld;

// `if.e(Lcn;III)V` -- onBlockAdded. Writes 15, then works the shape out.
void railPlaced(TickWorld& world, i32 x, int y, i32 z);

// `if.a(Lcn;IIII)V` -- onNeighborBlockChange. `fromId` is the block that
// changed, which the method reads for one thing only: whether it is a source of
// power, in which case a three-way junction is re-pointed.
void railNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                          block::BlockId fromId);

}  // namespace mc::tick
