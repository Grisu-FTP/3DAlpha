// The collision-box tables, transcribed from tests/collision_box_vectors.hpp.
//
// Every number below appears in that fixture, and collision_box_test.cpp checks
// this file against all 1,120 rows of it, so the two cannot drift apart.

#include "core/block/collision.hpp"

#include "core/block/registry.hpp"

namespace mc::block {
namespace {

constexpr AABB kUnitCube{0.0, 0.0, 0.0, 1.0, 1.0, 1.0};

// a1.1.2's stairs are two boxes: a half-height step over one half of the
// footprint, and a full-height block over the other. Metadata 0..3 is the
// direction it faces; 4..15 is unreachable and collides with nothing at all.
constexpr AABB kStairs[4][2] = {
    {{0.0, 0.0, 0.0, 0.5, 0.5, 1.0}, {0.5, 0.0, 0.0, 1.0, 1.0, 1.0}},
    {{0.0, 0.0, 0.0, 0.5, 1.0, 1.0}, {0.5, 0.0, 0.0, 1.0, 0.5, 1.0}},
    {{0.0, 0.0, 0.0, 1.0, 0.5, 0.5}, {0.0, 0.0, 0.5, 1.0, 1.0, 1.0}},
    {{0.0, 0.0, 0.0, 1.0, 1.0, 0.5}, {0.0, 0.0, 0.5, 1.0, 0.5, 1.0}},
};

// Three sixteenths thick, against one of the four walls.
constexpr AABB kDoor[4] = {
    {0.0,    0.0, 0.0,    0.1875, 1.0, 1.0},
    {0.0,    0.0, 0.0,    1.0,    1.0, 0.1875},
    {0.8125, 0.0, 0.0,    1.0,    1.0, 1.0},
    {0.0,    0.0, 0.8125, 1.0,    1.0, 1.0},
};

// Two sixteenths thick. Indexed by metadata - 2, so only 2..5 are in range.
constexpr AABB kLadder[4] = {
    {0.0,   0.0, 0.875, 1.0, 1.0, 1.0},
    {0.0,   0.0, 0.0,   1.0, 1.0, 0.125},
    {0.875, 0.0, 0.0,   1.0, 1.0, 1.0},
    {0.0,   0.0, 0.0,   0.125, 1.0, 1.0},
};

}  // namespace

int collisionBoxes(BlockId id, u8 metadata, AABB* out, int max)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }

    switch (shapeOf(id)) {
    case Shape::None:
        return 0;

    case Shape::FullCube:
        out[0] = kUnitCube;
        return 1;

    case Shape::Slab:
        out[0] = AABB{0.0, 0.0, 0.0, 1.0, 0.5, 1.0};
        return 1;

    // **A block and a half tall**, so a fence cannot be jumped. The extra half
    // is invisible -- the mesher draws a cube-high post -- and that is the
    // original's behaviour, not an oversight here.
    case Shape::Fence:
        out[0] = AABB{0.0, 0.0, 0.0, 1.0, 1.5, 1.0};
        return 1;

    // Inset a sixteenth all round and a sixteenth short on top, which is why
    // standing on a cactus leaves a visible gap at the edges.
    case Shape::Cactus:
        out[0] = AABB{0.0625, 0.0, 0.0625, 0.9375, 0.9375, 0.9375};
        return 1;

    case Shape::Stairs: {
        const int facing = metadata & 0x0F;
        if (facing > 3) {
            // Not reachable in a real world: the game only ever writes 0..3.
            // The original collides with nothing here, so neither do we.
            return 0;
        }
        out[0] = kStairs[facing][0];
        if (max < 2) {
            return 1;
        }
        out[1] = kStairs[facing][1];
        return 2;
    }

    case Shape::Door: {
        // Low two bits are the wall it hangs on; bit 2 is "open", which swings
        // it a quarter turn; bit 3 is the top half and does **not** affect the
        // box. Both halves of a door therefore report the same shape.
        int side = metadata & 3;
        if ((metadata & 4) != 0) {
            side = (side + 1) & 3;
        }
        out[0] = kDoor[side];
        return 1;
    }

    case Shape::Ladder: {
        // 2..5 are the four walls. Anything else sets no bounds at all in the
        // original, which then answers with whatever the shared Block singleton
        // was left holding by the previous query -- an order-dependent bug. We
        // answer with the constructor's default, a full cube, which is both
        // deterministic and what a freshly loaded jar says. The game never
        // writes those values.
        if (metadata < 2 || metadata > 5) {
            out[0] = kUnitCube;
            return 1;
        }
        out[0] = kLadder[metadata - 2];
        return 1;
    }

    case Shape::Count:
        break;
    }

    // Unreachable while every enumerator above is handled, and the switch has
    // no default so adding one is a compile error rather than a silent fall
    // through to "you can walk into it".
    out[0] = kUnitCube;
    return 1;
}

}  // namespace mc::block
