#pragma once

// **A dropped item as geometry** -- `ab.a(Ldx;DDDFF)V`, which is
// `RenderItem.doRenderItem`, and the third thing in this project that turns an
// entity into quads.
//
// It sits beside core/render/particle_mesh.hpp and follows the same rules: the
// caller owns the storage, nothing here allocates, the geometry comes out
// relative to an origin the caller picks, and the vertex written is
// `mesh::DetailVertex` -- so an item on the ground rides the shader that
// already lights and fogs the world.
//
// **The original draws two different things and so does this**:
//
//   * An item that puts down a block whose render type `RenderBlocks.renderItemIn3d`
//     answers true for -- 0, 10, 11 and 13, the standard block, stairs, the
//     fence and the cactus -- is drawn as **that block, a quarter size, spinning
//     about its own vertical axis**. It is the same list `gui::itemDrawsAsCube`
//     uses for a slot on the bottom screen, and asking it here rather than
//     writing a second one is what keeps a fence in the hand, a fence in a slot
//     and a fence on the ground the same shape.
//   * Everything else is a **flat sprite, half a block across, turned to face
//     the camera about the vertical axis only**. Not a full billboard: the
//     original rotates by `180 - playerViewY` and leaves pitch alone, so a
//     sword on the ground stays upright when you look down at it.
//
// **Two sheets, and therefore two passes.** a1.1.2 loads `/terrain.png` for the
// first kind and `/gui/items.png` for the second, and the PICA samples one
// texture per draw -- so `buildItemEntities` is asked for one sheet at a time
// and the caller binds and draws twice. An item whose sheet is not the one
// being asked for is skipped, which is why the two calls together cover the
// pool exactly once.
//
// **The bob and the spin are the original's arithmetic**, both driven off the
// entity's age and its random `hoverStart` so a heap of items does not pulse in
// unison:
//
//     bob   = sin((age + partial) / 10 + hoverStart) * 0.1 + 0.1
//     spin  = ((age + partial) / 20 + hoverStart) * 57.295776   degrees
//
// And a stack is drawn more than once: one copy up to a stack of 1, two past 1,
// three past 5, four past 20, each after the first offset by a fixed jitter.
// The original seeds that jitter with `random.setSeed(187L)` **per entity**,
// which is what makes a given pile look the same from frame to frame rather
// than shimmering; that seed is copied, and it is the one place in this project
// where a literal seed is the point.

#include "core/entity/item_entity.hpp"
#include "core/item/item_def.hpp"
#include "core/mesh/vertex.hpp"
#include "core/render/draw_budget.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `0.25F` for a block and `0.5F` for a sprite, straight out of the two
// branches. Named because the caller's culling wants to know how big one is.
inline constexpr float kItemBlockScale = 0.25f;
inline constexpr float kItemSpriteScale = 0.5f;

// `random.setSeed(187L)`. See the header note.
inline constexpr i64 kItemJitterSeed = 187;

// How many copies a stack of this size is drawn as: 1, 2, 3 or 4.
int itemCopies(int stackSize);

// Fills `out` with the quads for every entity that draws from `sheet`, and
// returns how many vertices it wrote. `viewYawDegrees` is the camera's yaw as
// the original's `playerViewY`; `partial` is the fraction of a tick elapsed.
//
// An entity further from the origin than a 16-bit detail position can express
// is **skipped rather than clamped**, exactly as a particle is: a clamped one
// would be an item stuck to the edge of the world.
//
// When the buffer cannot take every item in range, the **nearest** are drawn
// (core/render/draw_budget.hpp). The two sheets' passes share one buffer, so
// they share one `DrawCutoff` too: the first pass, handed the whole buffer as
// `max`, settles it, and the second charges its items to what is left.
int buildItemEntities(const entity::ItemEntitySystem& system, float viewYawDegrees,
                      double originX, double originY, double originZ, float partial,
                      item::IconSheet sheet, mesh::DetailVertex* out, int max,
                      DrawCutoff* shared = nullptr);

}  // namespace mc::render
