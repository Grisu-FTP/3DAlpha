#pragma once

// An axis-aligned box in doubles, and the geometry that does not need a world.
//
// This is the original's `AxisAlignedBB`, minus its object pool. a1.1.2 pools
// these because it allocates one per block per movement step and is running on
// a 2010 JVM with a stop-the-world collector; we return them by value into
// caller-owned storage, so there is nothing to pool and nothing to reset. The
// pool is lag, not design -- see the note in CLAUDE.md about being faithful to
// the game rather than to its limits.
//
// **Doubles, not floats.** A block's own bounds are all multiples of a
// sixteenth and would fit in a float with room to spare, but a box is also used
// in world coordinates, and a float world coordinate snaps to the block grid
// out at the Far Lands -- the same reason `Camera` is double. See
// docs/architecture.md, "Rendering far from the origin".
//
// It lives in util rather than beside the player body because two unrelated
// layers need the type: core/block/collision.hpp answers with boxes, and
// core/entity moves a box through them. Neither should have to include the
// other.

#include "core/util/types.hpp"

namespace mc {

struct AABB {
    double minX = 0.0, minY = 0.0, minZ = 0.0;
    double maxX = 0.0, maxY = 0.0, maxZ = 0.0;

    // Moved bodily, without changing its size.
    constexpr AABB offset(double dx, double dy, double dz) const
    {
        return AABB{minX + dx, minY + dy, minZ + dz, maxX + dx, maxY + dy, maxZ + dz};
    }

    // Grown by the same amount on both sides of each axis, the way the
    // original's `expand` works -- a negative value shrinks it.
    constexpr AABB expand(double dx, double dy, double dz) const
    {
        return AABB{minX - dx, minY - dy, minZ - dz, maxX + dx, maxY + dy, maxZ + dz};
    }

    // Grown only in the direction of travel: the swept volume of a box about to
    // move by (dx, dy, dz). This is `addCoord`, and it is what turns "which
    // blocks might I hit" into a single query.
    constexpr AABB extend(double dx, double dy, double dz) const
    {
        AABB out = *this;
        if (dx < 0.0) { out.minX += dx; } else { out.maxX += dx; }
        if (dy < 0.0) { out.minY += dy; } else { out.maxY += dy; }
        if (dz < 0.0) { out.minZ += dz; } else { out.maxZ += dz; }
        return out;
    }

    // **Strictly**, on every axis: boxes that merely share a face do not
    // intersect. That is the original's test, and it is load-bearing -- a
    // player standing exactly on a floor has maxY of the floor equal to minY of
    // the player, and treating that as a collision would wedge them.
    constexpr bool intersects(const AABB& other) const
    {
        return other.maxX > minX && other.minX < maxX
            && other.maxY > minY && other.minY < maxY
            && other.maxZ > minZ && other.minZ < maxZ;
    }

    constexpr bool contains(double x, double y, double z) const
    {
        return x > minX && x < maxX && y > minY && y < maxY && z > minZ && z < maxZ;
    }
};

}  // namespace mc
