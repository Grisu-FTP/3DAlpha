#pragma once

// **How far an entity is drawn, and the units that let it be drawn that far.**
//
// `kh.a(D)Z` (Entity.isInRangeToRenderDist) draws an entity while its squared
// distance from the camera is under `(averageEdge * 64 * ac)^2`. No subclass
// overrides it, and `ac` is 1.0 for everything but `nt` (the other player,
// 10.0), so the range is the box alone: 64 blocks for a zombie, 79 for a
// spider, 21 for a chicken, 77 for a boat, 57 for a minecart, 63 for a falling
// block or primed TNT -- and 16 for a dropped item, whose box is a quarter of a
// block.
//
// **A detail vertex cannot reach most of that.** It is a signed short at 1/1024
// of a block from the eye, which stops at 31.25 blocks; an entity used to vanish
// there, and a mob whose centre was just inside that line had its feet or its
// legs outside it, wrapped to the far side of the short and stretched across
// the screen. The passes whose entities reach further write 1/256 of a block
// instead -- 125 blocks -- and the renderer scales the matrix by
// `kEntityUnitScale` to put them back, the trick the sign pass plays at 1/512.
// The rounding is a 512th of a block, under half a pixel at arm's length on the
// top screen.
//
// Those passes: the mobs (and everything drawn in their buffer -- shells,
// spider eyes, spawner miniatures, other players), boats, minecarts, falling
// blocks, primed TNT, and the flames on any of them. A dropped item's 16 blocks
// fit the detail format, so it keeps 1/1024.

#include "core/mesh/vertex.hpp"

namespace mc::render {

inline constexpr int kEntityUnitsPerBlock = 256;
inline constexpr float kEntityUnitScale =
    float(mesh::kDetailUnitsPerBlock) / float(kEntityUnitsPerBlock);

// How far a model reaches from its entity's position, generously: a size-4
// slime mid-bounce is about three blocks tall, a swollen creeper under two and
// a half, a spider's legs and a boat's hull under a block and a half to the
// side. The admission test takes it off the short's reach so that no vertex of
// an admitted entity is out of range (`buildBox` backstops a miss by dropping
// the box).
inline constexpr double kEntityModelReach = 4.0;

// Where an entity's position may be, on any axis, for its model to fit: 121
// blocks. **Past it nothing is drawn regardless**, a stated deviation for
// exactly one entity: a size-4 slime's range is 154 blocks, which a New 3DS at
// twelve chunks can see into.
inline constexpr double kEntityPlacementLimit =
    32000.0 / double(kEntityUnitsPerBlock) - kEntityModelReach;

// The same bound for the passes still at 1/1024 of a block.
inline constexpr double kDetailPlacementLimit = 32000.0 / double(mesh::kDetailUnitsPerBlock);

// `kh.a(D)Z` with `ac` at 1.0: `cf.b()` -- the average of the box's three edges,
// which are width, height and width -- times 64, as a squared distance. A
// slime's box is its live one, so a big slime is seen from further off than a
// small one.
inline double entityDrawDistanceSq(double width, double height)
{
    const double edge = (width + height + width) / 3.0 * 64.0;
    return edge * edge;
}

// Whether a position relative to the origin is inside both: the game's range
// and the short's.
inline bool entityInDrawRange(double dx, double dy, double dz, double width, double height,
                              double limit = kEntityPlacementLimit)
{
    return dx * dx + dy * dy + dz * dz < entityDrawDistanceSq(width, height) && dx >= -limit
           && dx <= limit && dy >= -limit && dy <= limit && dz >= -limit && dz <= limit;
}

}  // namespace mc::render
