#pragma once

// What the crosshair is pointing at: a1.1.2's `World.rayTraceBlocks`, which
// walks the ray block by block and asks each one to intersect itself.
//
// **The face comes from the block, not from the grid.** The walk knows which
// cell wall it crossed, but the answer it returns is whichever face of the
// *block's own shape* the ray met -- so aiming at the side of a torch reports
// the side of the torch, not the side of the metre cube it stands in. That is
// why this needs the selection table (`block::selectionBox`) and not just the
// block ids.
//
// The shape a ray tests against is **not** the shape you walk into. A torch has
// no collision box at all and is perfectly targetable; water and lava have the
// opposite problem and are targetable by nothing. See core/block/collision.hpp.
//
// Allocates nothing: the original pools a `Vec3D` per step because it is a 2010
// JVM allocating in a loop, and we return the answer by value into the caller's
// storage instead.
//
// Derived from `cn.a(aj,aj,Z)` and `ly.a(cn,III,aj,aj)`. See
// docs/physics-a1.1.2.md.

#include "core/block/block_def.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// **4.0, not 5.0.** `PlayerController.getBlockReachDistance` returns 5.0f in the
// base class, but both concrete controllers -- the single-player one and the
// multiplayer one -- override it with 4.0f, so 5.0 is a value the game never
// actually uses.
inline constexpr double kBlockReach = 4.0;

// a1.1.2's own step cap: twenty cells, which a four-block ray cannot exhaust.
inline constexpr int kRayStepLimit = 20;

struct RayHit {
    bool hit = false;

    i32 x = 0;
    int y = 0;
    i32 z = 0;

    // Which face of the block's shape the ray met, in `mesh::Face` numbering --
    // the same numbering the original's `sideHit` uses, which is why the two
    // can share an enum.
    mesh::Face face = mesh::kFaceNegY;

    // Where it met it, in world coordinates.
    double hitX = 0.0;
    double hitY = 0.0;
    double hitZ = 0.0;

    // The block the ray would be placed *against*: the neighbour on the struck
    // face. Not bounds-checked against the world height -- the caller decides
    // what to do about a face pointing off the top of it.
    i32 placeX() const { return x + mesh::kFaceOffset[face].dx; }
    int placeY() const { return y + mesh::kFaceOffset[face].dy; }
    i32 placeZ() const { return z + mesh::kFaceOffset[face].dz; }
};

// From `eye` along `dir` -- which must be a unit vector -- for `reach` blocks.
//
// **The block the eye is inside is never tested**, which is the original's
// behaviour and not an oversight: the walk compares against the end cell and
// steps before it looks at anything, so a player standing in tall grass is not
// aiming at it.
RayHit rayTrace(const tick::TickWorld& world, double eyeX, double eyeY, double eyeZ,
                double dirX, double dirY, double dirZ, double reach = kBlockReach);

}  // namespace mc::entity
