#include "core/block/model.hpp"

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"

namespace mc::block {

namespace {

// `bc.m`'s own `setBounds` calls: the post is 6/16 to 10/16 across and full
// height, and each rail is 7/16 to 9/16 thick at 12/16..15/16 and 6/16..9/16.
constexpr double kPostLo = 0.375;
constexpr double kPostHi = 0.625;
constexpr double kRailLo = 0.4375;
constexpr double kRailHi = 0.5625;
constexpr double kUpperBottom = 0.75;
constexpr double kUpperTop = 0.9375;
constexpr double kLowerBottom = 0.375;
constexpr double kLowerTop = 0.5625;

int fenceBoxes(int connections, AABB* out, int max)
{
    int count = 0;
    if (count < max) {
        out[count++] = AABB{kPostLo, 0.0, kPostLo, kPostHi, 1.0, kPostHi};
    }

    const int bits[4] = {kConnectNegX, kConnectPosX, kConnectNegZ, kConnectPosZ};
    for (int side = 0; side < 4; ++side) {
        if ((connections & bits[side]) == 0) {
            continue;
        }
        for (int level = 0; level < 2 && count < max; ++level) {
            const double lo = level == 0 ? kLowerBottom : kUpperBottom;
            const double hi = level == 0 ? kLowerTop : kUpperTop;
            AABB rail{kRailLo, lo, kRailLo, kRailHi, hi, kRailHi};
            switch (bits[side]) {
                case kConnectNegX:
                    rail.minX = 0.0;
                    rail.maxX = kPostLo;
                    break;
                case kConnectPosX:
                    rail.minX = kPostHi;
                    rail.maxX = 1.0;
                    break;
                case kConnectNegZ:
                    rail.minZ = 0.0;
                    rail.maxZ = kPostLo;
                    break;
                default:
                    rail.minZ = kPostHi;
                    rail.maxZ = 1.0;
                    break;
            }
            out[count++] = rail;
        }
    }
    return count;
}

}  // namespace

int renderBoxes(BlockId id, u8 metadata, int connections, AABB* out, int max)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }

    switch (def(id).render) {
        case RenderType::Cube:
            // The bounds the standard renderer draws, which the selection table
            // already holds for every metadata value -- a full cube for most, a
            // half for a slab, a plate for a pressure plate.
            out[0] = selectionBox(id, metadata);
            return 1;

        case RenderType::Stairs:
            // The only shape in a1.1.2 with two boxes, and they are the two you
            // walk into as well as the two that are drawn.
            return collisionBoxes(id, metadata, out, max);

        case RenderType::Cactus:
            // The sides only. `bc.l` draws the top and bottom at the cell's full
            // extent and the four sides pulled in by a sixteenth, and it is
            // that gap that makes a column of cactus read as segments -- but as
            // a *shape* the cactus is its inset box, which is also what it
            // collides as.
            return collisionBoxes(id, metadata, out, max);

        case RenderType::Fence:
            return fenceBoxes(connections, out, max);

        default:
            // Sheets, crosses, torches, fluids and fire. Not boxes, and the
            // original does not draw them in three dimensions in a slot either.
            return 0;
    }
}

}  // namespace mc::block
