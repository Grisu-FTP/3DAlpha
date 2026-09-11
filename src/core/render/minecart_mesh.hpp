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
// **Not transcribed, and named so it is not looked for**: a chest or furnace
// cart draws its block inside the cart, which needs a block model rendered
// inside an entity and is a thing this build has no path for. Both types are
// placeable and drawable as plain carts. See core/entity/minecart.hpp.

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

}  // namespace mc::render
