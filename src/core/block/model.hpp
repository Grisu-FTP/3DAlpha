#pragma once

// What shape a block is **to look at** -- as a list of boxes.
//
// **Not the same question as `collision.hpp`, and the fence is why.** A fence
// collides as a single block-sized box a block and a half tall, because that is
// what stops you walking over one; it *looks* like a post with rails hanging
// off it. A cactus collides inset by a sixteenth on all four sides and is drawn
// with its top and bottom at full width. A ladder has no collision box at all
// and is plainly visible. So the two tables answer different questions and this
// one exists rather than being folded into that one.
//
// **Two callers, and that is the reason it is here rather than in the mesher.**
// The world mesher turns these boxes into quads (core/mesh/shapes.cpp), and the
// bottom screen draws them as a small isometric picture in an inventory slot
// (core/gui/item_icon.cpp). Before this they were two sets of constants: the
// mesher had the fence's post and rails, and the icon drew a flat plank tile,
// which is what "the fence texture is just the wood texture in 2d" was.
//
// It answers for the four render types a1.1.2 itself draws in three dimensions
// -- `RenderBlocks.renderItemIn3d` is a static method in the jar and returns
// true for render types 0, 10, 11 and 13, the standard block, stairs, the fence
// and the cactus -- plus the standard blocks whose bounds are smaller than
// their cell. Everything else is a sheet or a cross rather than a box, answers
// zero here, and is drawn flat in a slot exactly as the original draws it.

#include "core/block/block_def.hpp"
#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::block {

// A fence is a post and up to four rails, which is the most any shape here
// produces when every side connects.
inline constexpr int kMaxRenderBoxes = 9;

// Boxes in **block-local** coordinates, the same convention `collisionBoxes`
// uses: 0,0,0 is the block's own corner and a full cube is 0,0,0 -> 1,1,1.
//
// `connections` is a bit per horizontal direction -- -X, +X, -Z, +Z -- for the
// shapes that reach towards their neighbours. Zero is the right answer for an
// inventory icon, which has no neighbours, and gives a bare fence post.
int renderBoxes(BlockId id, u8 metadata, int connections, AABB* out, int max);

// Bit positions for `connections`, in the order the four horizontal
// `mc::mesh::Face` values run.
inline constexpr int kConnectNegX = 1 << 0;
inline constexpr int kConnectPosX = 1 << 1;
inline constexpr int kConnectNegZ = 1 << 2;
inline constexpr int kConnectPosZ = 1 << 3;

}  // namespace mc::block
