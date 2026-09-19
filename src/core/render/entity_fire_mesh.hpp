#pragma once

// **A burning entity's flames** -- `ak.a(Lkh;DDDF)V`, the private half of
// `doRenderShadowAndFire` that every entity renderer inherits and that this
// port had never drawn. A mob caught fire, took the damage, made the noise and
// looked exactly like a mob that had not.
//
// **It is a stack of camera-facing sheets off the fire tile**, and the whole of
// it is eight numbers out of the class file:
//
//     glDisable(GL_LIGHTING);
//     int i = Block.fire.blockIndexInTexture;    // one tile, not two
//     glTranslatef(x, y, z);
//     float f5 = entity.width * 1.4F;
//     glScalef(f5, f5, f5);
//     float f9 = entity.height / entity.width;
//     glRotatef(-playerViewY, 0, 1, 0);
//     glTranslatef(0, 0, -0.4F + (int)f9 * 0.02F);
//     while (f9 > 0) {                           // ceil(height / width) sheets
//         quad x in [-0.5, f6 - 0.5], y in [-f8, 1.4 - f8]
//         f9 -= 1; f8 -= 1; f6 *= 0.9F;
//         glTranslatef(0, 0, -0.04F);
//     }
//
// Four things in there are worth saying out loud, because each one is a
// "surely that is a bug" that is not:
//
//   * **One tile.** Later versions alternate `fire_0` and `fire_1` down the
//     stack; a1.1.2 reads `blockIndexInTexture` once, above the loop. Block
//     fire uses both tiles (core/mesh/shapes.cpp) and entity fire uses the
//     first. The tile is asked of the block that *renders as fire* rather than
//     of an id, through `texture::flameTile` -- and it is the tile
//     `FlameAnimation` is already rewriting 20 times a second, so these sheets
//     animate for free.
//
//   * **The quad is not symmetric.** `f6` shrinks by a tenth per sheet but only
//     the +x edge moves: x runs from -0.5 to `f6 - 0.5`, so the stack leans
//     left as it rises rather than tapering. That is what the original writes.
//
//   * **The sheets are a whole unit apart and 1.4 units tall**, so consecutive
//     ones overlap by 40 % -- which is what makes three sheets read as one
//     column of flame rather than three stacked cards.
//
//   * **The whole stack is pulled toward the camera**, not away: `-0.4` along
//     the local +z that `glRotatef(-playerViewY)` points *down* the view
//     direction, plus another 0.04 per sheet. Fire drawn inside a mob would be
//     hidden by it; fire drawn a third of a block in front of one is what a
//     burning zombie looks like.
//
// **Full bright and untinted.** The original's `glDisable(GL_LIGHTING)` and
// `glColor4f(1, 1, 1, 1)` become `light = 0xFF` and a white vertex colour, so a
// mob burning at the bottom of a cave lights its own flames. The hurt flash
// that `buildMobs` multiplies into a mob's vertices deliberately does not reach
// these: the original draws the fire in a pass of its own, after the flash.
//
// **Yaw only.** Like a dropped item's sprite and unlike a particle, this
// billboard has no pitch term -- `glRotatef(..., 0, 1, 0)` -- so looking down
// on a burning animal shows the flames edge-on, exactly as it does in the
// original.

#include "core/entity/boat.hpp"
#include "core/entity/falling_block.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/primed_tnt.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `float f5 = entity.width * 1.4F` -- the whole sheet stack is in units of the
// entity's width times this, which is why a spider's fire is wide and flat and
// a skeleton's is a narrow column.
inline constexpr float kEntityFireScale = 1.4f;

// **A cap the original does not have**, because the original's loop is bounded
// by a table it wrote: `height / width` is at most 3 for anything in a1.1.2 (a
// zombie and a skeleton, 1.8 over 0.6). The loop condition is a float countdown
// and a zero width would not terminate at all, so the geometry is bounded here
// rather than trusting a dimension that now comes out of a data file.
inline constexpr int kEntityFireMaxLayers = 8;

// Four vertices a sheet, and **32 burning entities at once**: the sixteen
// animals a frame draws, plus room for the carts, boats and stacks a fire in a
// storeroom can light. 1,024 vertices, 16 KB. Past that the nearest burn and
// the rest are dark -- see draw_budget.hpp, and note that every entity here is
// drawn by its own pass regardless.
inline constexpr int kEntityFireDrawBudget = 32;
inline constexpr int kEntityFireMaxVertices =
    kEntityFireDrawBudget * kEntityFireMaxLayers * 4;

// `glRotatef(-playerViewY, 0, 1, 0)` as the two numbers a vertex needs. Worked
// out once a frame, as `buildItemEntities` works out its own facing.
struct FireFacing {
    float sinYaw, cosYaw;
};

FireFacing fireFacing(float viewYawDegrees);

// How many sheets an entity this shape gets: `ceil(height / width)`, clamped to
// `kEntityFireMaxLayers`. Zero for an entity with no height, because the
// original's loop tests before its first pass.
int entityFireLayers(float width, float height);

// One burning entity's sheets, at a position already made relative to the
// caller's origin. `tile` is the atlas tile the flame lives in. Returns how
// many vertices were written, which is a multiple of four.
//
// **Positions are in `kEntityUnitsPerBlock`** (1/256 of a block), not the
// detail format's, so that a zombie burning at dawn keeps its flames out to the
// 64 blocks it is drawn at. The draw scales the matrix by `kEntityUnitScale`.
// See core/render/entity_range.hpp.
int buildEntityFire(double relX, double relY, double relZ, float width, float height,
                    int tile, FireFacing facing, mesh::DetailVertex* out, int max);

// **Every pool that can be alight.** a1.1.2 runs the ignition in
// `kh.c(DDD)V` -- moveEntity -- so what burns is exactly what moves: the mobs,
// a dropped stack, a boat, a minecart, a falling block and a block of primed
// TNT. An arrow and a painting never call it and never catch fire; see
// core/entity/fire_entry.hpp, which is the tick half of this.
//
// Any of them may be null. They are drawn into **one buffer through one
// cutoff**, because every sheet is the same tile of the same atlas -- so a
// world with four things alight in it costs one draw call, not four.
struct FireScene {
    const entity::MobSystem* mobs = nullptr;
    const entity::ItemEntitySystem* items = nullptr;
    const entity::BoatSystem* boats = nullptr;
    const entity::MinecartSystem* minecarts = nullptr;
    const entity::FallingBlockSystem* fallingBlocks = nullptr;
    const entity::PrimedTntSystem* primedTnt = nullptr;
};

// Every burning entity in `scene`, nearest first when the buffer cannot hold
// them all. Returns 0 for a version whose block table has no fire in it.
//
// **Each entity's flames reach as far as the entity is drawn**, which is
// `kh.a(D)Z` of its own box (`entityInDrawRange`) -- so nothing burns in the
// distance with no entity inside the fire, and a dropped stack's flames stop at
// the 16 blocks the stack does.
//
// **The stack stands on the entity's `posY`**, which is not the same place for
// everything here: a mob's is its feet, and for the five entities whose
// `yOffset` is half their height -- the item, the boat, the cart, the falling
// block, the TNT -- it is the middle of the box. That is where the original
// puts the flames on each, because `doRenderShadowAndFire` is handed the same
// interpolated position the model is drawn at.
int buildEntityFires(const FireScene& scene, float viewYawDegrees, double originX,
                     double originY, double originZ, float partial,
                     mesh::DetailVertex* out, int max);

}  // namespace mc::render
