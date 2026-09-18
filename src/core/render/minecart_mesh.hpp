#pragma once

// **A minecart as geometry** -- `hj` (ModelMinecart) through `kt`
// (RenderMinecart).
//
// **Six boxes**, and the shape is the boat's argument again: one floor plate,
// one thinner plate under it drawn from a second texture offset, and one side
// panel placed four times at four rotation points. `hj`'s array is seven long
// and only six are ever built; the seventh is never touched.
//
// **The cart is tilted along the track, not along its own motion.** This is the
// one interesting thing `RenderMinecart` does and it is worth stating because
// it is not obvious from the entity: the renderer samples the rail *three
// tenths of a block ahead and behind the cart*, takes the vector between those
// two points, and reads the yaw and the pitch off it. So a cart on a slope
// leans with the rail even when it is standing still, and a cart on a curve
// points along the curve rather than along its velocity.
//
// It also **raises the cart onto the rail**: the drawn y is the mean of the two
// samples' heights rather than the entity's own, which is what stops a cart
// halfway up a slope from cutting into the track.
//
// `atan(slope.y) * 73.0` is the pitch, and the 73 is a literal in the class
// file rather than a conversion of anything -- `180/PI` is 57.3, so this is
// about a quarter more tilt than the geometry has. It exaggerates on purpose.
//
// **A chest or furnace cart carries its block, and that is a second pass.**
// `kt.a` draws it between the tilt and the cart's own model:
//
// ```
// if (minecart.type != 0) {
//     loadTexture("/terrain.png");
//     glScalef(0.75F, 0.75F, 0.75F);
//     glTranslatef(0.0F, 0.3125F, 0.0F);
//     glRotatef(90F, 0.0F, 1.0F, 0.0F);
//     new RenderBlocks().renderBlockOnInventory(type == 1 ? Block.crate
//                                                         : Block.stoneOvenIdle);
//     ...
// }
// ```
//
// It is a separate builder rather than six more boxes in the one above because
// of that first line: the cart is drawn off the **entity** sheet and the block
// off **terrain**, which is a different binding and therefore a different draw.
// `buildMinecartBlocks` emits the block, in the cart's own frame, so the two
// agree about the tilt down to the last degree -- a block that worked out its
// own heading would slide out of the cart on a slope.
//
// **The furnace is always the unlit one.** `ly.aC` is `stoneOvenIdle`; a
// burning furnace cart does not light up, which is a1.1.2's and not an
// omission here.

#include "core/entity/minecart.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::render {

inline constexpr int kMinecartParts = 6;
inline constexpr int kMinecartVerticesEach = kMinecartParts * 6 * 4;
// **What one frame draws**, which is not what exists: the pool has no cap,
// and past this many in range the nearest are drawn -- see draw_budget.hpp.
inline constexpr int kMinecartDrawBudget = 32;
inline constexpr int kMinecartMaxVertices = kMinecartDrawBudget * kMinecartVerticesEach;

// Fills `out` with the quads for every live minecart -- the nearest, when `max`
// cannot hold them all. `world` is needed because
// the tilt is read off the track rather than off the cart -- see the header.
int buildMinecarts(const entity::MinecartSystem& system, const tick::TickWorld& world,
                   double originX, double originY, double originZ, float partial,
                   mesh::DetailVertex* out, int max);

// One cube per chest or furnace cart, off **terrain.png** rather than the
// entity sheet -- see the header. Same arguments, same origin, same partial;
// the caller draws it with the block atlas bound.
inline constexpr int kMinecartBlockVerticesEach = 6 * 4;
inline constexpr int kMinecartBlockDrawBudget = kMinecartDrawBudget;
inline constexpr int kMinecartBlockMaxVertices =
    kMinecartBlockDrawBudget * kMinecartBlockVerticesEach;

int buildMinecartBlocks(const entity::MinecartSystem& system, const tick::TickWorld& world,
                        double originX, double originY, double originZ, float partial,
                        mesh::DetailVertex* out, int max);

}  // namespace mc::render
