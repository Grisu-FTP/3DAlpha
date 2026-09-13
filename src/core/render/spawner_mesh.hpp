#pragma once

// **The mob turning inside the cage** -- `r`, which is a1.1.2's
// TileEntityMobSpawnerRenderer and one of exactly two tile-entity renderers in
// the game (`in`, the sign, is the other).
//
// The block itself needs nothing from here: `mob_spawner` is render type `cube`
// with one texture on all six faces, so the bars are drawn by the ordinary
// mesher like any other block. What the mesher cannot draw is the *contents* --
// a miniature of whatever `EntityId` names, spinning, leaning back, and lit by
// the cell it is in.
//
// **The whole renderer is nine calls**, and they compose to one `Placement`:
//
//     glTranslatef(x + 0.5F, y, z + 0.5F);
//     entity = cache(EntityId) ?: EntityList.createEntityInWorld(EntityId, null);
//     entity.setWorld(tile.worldObj);
//     glTranslatef(0.0F, 0.4F, 0.0F);
//     glRotatef((prevYaw + (yaw - prevYaw) * partial) * 10.0F, 0, 1, 0);
//     glRotatef(-30.0F, 1, 0, 0);
//     glTranslatef(0.0F, -0.4F, 0.0F);
//     glScalef(0.4375F, 0.4375F, 0.4375F);
//     entity.setLocationAndAngles(x, y, z, 0.0F, 0.0F);
//     RenderManager.instance.renderEntityWithPosYaw(entity, 0, 0, 0, 0, partial);
//
// **The factor of ten is the interesting number.** `bd` accumulates
// `1000 / (delay + 200)` degrees a tick and this multiplies it by ten on the
// way out, so the mob turns at `10000 / (delay + 200)` degrees a tick -- about
// 12.5 with a fresh 600-tick delay and **fifty** on the tick before it fires.
// The visible acceleration is the whole tell that a spawner is about to go off,
// and it falls out of the arithmetic rather than being animated.
//
// **The lift is not symmetric with itself.** `+0.4` before the rotations and
// `-0.4` after means the mob leans about a point four tenths of a block above
// the block's floor, which is roughly the middle of the cage -- so the -30
// degree tilt swings it rather than pivoting it on its feet.
//
// **The mob is never ticked.** It comes out of `EntityList` with a null world,
// is put in a map and reused for every spawner with the same name, and nothing
// calls `onUpdate` on it -- so its limbs are at rest, its head is straight and
// its `renderYawOffset` is zero. `RenderLiving`'s own `180 - renderYawOffset`
// still applies, which is why the miniature faces *away* at zero spin.
//
// Two deviations, both stated:
//
//   * **No entity cache.** A `Mob` is 200-odd bytes on the stack here; the jar
//     keeps a HashMap because building one costs a reflective constructor call.
//   * **A slime is always size 1.** `ma`'s constructor draws `1 << nextInt(3)`
//     from the entity's *own* `java.util.Random`, so the jar's cached display
//     slime is whatever the first draw gave it and stays that for the session.
//     There is no seed to reproduce and no saved state to read; the smallest is
//     the one that fits in the cage.

#include "core/entity/mob_spawner.hpp"
#include "core/mesh/vertex.hpp"
#include "core/render/box_model.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// `0.4375F` -- seven sixteenths, which is what makes a 1.8-block zombie 0.7875
// and fit inside a one-block cage with room to lean.
inline constexpr float kSpawnerModelScale = 0.4375f;

// The `+0.4` / `-0.4` pair the rotations sit between.
inline constexpr double kSpawnerPivotLift = 0.4;

// `glRotatef(-30.0F, 1, 0, 0)`, and the ten the spin is multiplied by.
inline constexpr float kSpawnerTilt = -30.0f;
inline constexpr float kSpawnerSpinFactor = 10.0f;

// **What one frame draws.** A dungeon has one spawner and a render distance
// rarely holds two; four is slack, not a limit players meet. Each is one mob's
// worth of parts.
inline constexpr int kSpawnerDrawBudget = 4;
inline constexpr int kSpawnerMaxVertices = kSpawnerDrawBudget * kMobVerticesEach;

// Where one spawner's miniature goes -- the nine calls above, composed. Exposed
// because it is the whole of the renderer that can be wrong and a test can
// check it without a GPU.
Placement placeSpawnerMob(const entity::MobSpawnerBlock& spawner, double originX,
                          double originY, double originZ, float partial);

// `r.a(Lbd;DDDF)V` for every spawner in the store, nearest first when the
// buffer cannot hold them all. Returns how many vertices were written.
int buildSpawnerMobs(const entity::MobSpawnerStore& store, double originX, double originY,
                     double originZ, float partial, mesh::DetailVertex* out, int max);

}  // namespace mc::render
