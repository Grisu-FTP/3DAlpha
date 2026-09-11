#pragma once

// The shapes that are not cubes, not crosses, not fluid and not torches: the
// ten render types that had no emitter at all and were therefore **drawn as
// nothing**.
//
// That was deliberate once and stopped being defensible. `meshSection` skips a
// render type it has no emitter for, on the argument that a missing ladder is
// obvious and a cubic one looks deliberate -- which was right while nothing
// could place a ladder. Creative can place all ten, so the choice became
// "invisible" rather than "not yet", and a door you can walk through because
// you cannot see it is worse than a wrong-looking door.
//
// **Where each shape comes from.** `RenderBlocks` dispatches on
// `Block.getRenderType()` in `bc.a(ly,III)`, and that method's if-chain is the
// map from a render type to the method that draws it:
//
//     0 -> k standard   1 -> h cross    2 -> b torch    3 -> d fire
//     4 -> j fluid      5 -> e redstone 6 -> i crops    7 -> o door
//     8 -> g ladder     9 -> f rail    10 -> n stairs  11 -> m fence
//    12 -> c lever     13 -> l cactus
//
// Four of them need no new geometry at all, because the box they draw is the
// box you walk into and `core/block/collision.cpp` already has it, measured
// against a running jar: **stairs** (the only shape with two boxes), **doors**,
// **ladders** and **cactus**. Those go straight through `addBox`.
//
// The rest carry their own constants, read out of the class file's `setBounds`
// calls rather than remembered -- the fence's post and two rails, the crops'
// four planes and their 1/16 drop, the rail's height off the floor.
//
// **One is honestly simplified and says so in place**: fire is four
// wall-hugging sheets rather than the original's flapping diagonals. It is
// visible, the right size and the right colour, and does not pretend to be the
// transcription the others are.
//
// The lever used to be the second. It is not any more: `bc.c` builds its handle
// out of eight corners it rotates -- 40 degrees for the throw, then a quarter
// turn per mounting -- and every one of those is expressible through
// `addDetailQuad`, which takes arbitrary corners. What it is *not* expressible
// through is `addBox`, which derives a face's UV from the box's bounds and gave
// the handle a strip of tile taken from wherever the box happened to sit.
//
// The redstone wire is the other one that stopped being a sketch. It now works
// out which of its four neighbours it is wired to -- the same
// `isPowerProviderOrWire` test `core/tick/redstone.cpp` uses, which is why that
// predicate is public -- picks the crossing or the line tile accordingly, trims
// the arms that lead nowhere, climbs a solid neighbour with wire on top of it,
// and draws from the lit row of the atlas when it is carrying a signal.

#include "core/block/block_def.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/scratch.hpp"
#include "core/util/types.hpp"

namespace mc::mesh {

// **One entry point, dispatching on the render type**, so `meshSection` gains
// one line rather than ten and the "a case here needs a case in hasEmitter"
// pairing stays a two-place edit instead of a twenty-place one.
//
// Takes the block id rather than its definition because four of the shapes ask
// `block::collisionBoxes` for their geometry, and that is keyed by id. Returns
// false for a render type this does not draw, which is what keeps `hasEmitter`
// honest.
bool addShape(const MeshScratch& scratch, int x, int y, int z, block::BlockId id, u8 metadata,
              MeshBuilder& out);

// Whether `addShape` draws this render type. The same list, and a test checks
// the two agree.
bool shapeHasEmitter(block::RenderType type);

}  // namespace mc::mesh
