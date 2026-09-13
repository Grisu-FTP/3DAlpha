#pragma once

// **`je` -- Explosion**, and in a1.1.2 it is one class with one method:
// `je.a(Lcn;Lkh;DDDF)V`, reached through `cn.a(Lkh;DDDF)V`. It has both of the
// version's callers now: a creeper at strength 3 (`core/entity/mob.cpp`) and
// primed TNT at strength 4 (`core/entity/primed_tnt.cpp`).
//
// **Three phases, in this order, and the order is observable:**
//
//   1. `random.explode`, then **1,352 rays**: every cell on the surface of a
//      16 x 16 x 16 cube gives a direction, and each ray is walked in steps of
//      0.3 while its strength lasts, losing `(explosionResistance + 0.3) * 0.3`
//      to the block it is in and a flat `0.225` to the step. Every cell a
//      still-live ray passes through is recorded.
//   2. **Entities are hurt while the blocks are still standing**, which is why
//      this is not "destroy, then damage": a creeper on the far side of a wall
//      takes a fraction of its own blast, because `getBlockDensity` traces
//      through the world as it was.
//   3. The recorded cells are destroyed, each dropping its block **three times
//      in ten** (`dropBlockAsItemWithChance(..., 0.3F)`).
//
// **`getExplosionResistance` is `blockResistance / 5`, and `blockResistance` is
// not the number in `blocks.json`.** `ly.b(F)` stores `setResistance(f) * 3`
// and `ly.c(F)` raises it to `hardness * 5` if that is larger; every block in
// this version calls `.c()` before `.b()`, so the stored value is
// `max(resistance * 3, hardness * 5)` -- checked against all 70 rows, with no
// exceptions. The generator keeps the *argument*, so the multiply is done here
// and is named rather than baked into the table.
//
// **The radius is bounded and the bound is measured.** A ray in open air loses
// `0.09 + 0.225 = 0.315` per 0.3-block step and starts at
// `strength * (0.7 .. 1.3)`, so a creeper's 3.0 reaches at most
// `3.9 / 0.315 * 0.3` = **3.7 blocks** and TNT's 4.0 reaches 5.0. The cell
// record below is a bitset over a cube of radius `kMaxRadius` = 8, which is
// 4,913 bits -- 616 bytes, on the caller's stack, no allocation, and enough
// for any strength up to 8.4. A stronger blast is clamped rather than
// overflowing, and nothing in this version can ask for one.
//
// **What is not here**: fire. `isFlaming` arrives with 1.0's beds and ghasts
// and there is nothing in a1.1.2 that sets it.
//
// **`onBlockDestroyedByExplosion` is here**, and it is one block. `ly.c(Lcn;III)V`
// is empty on `Block` and TNT is the only override in this version: a recorded
// cell holding TNT re-primes on a 10..29-tick fuse rather than detonating,
// which is what makes a chain ripple. See `core/tick/drop.hpp`. The hook runs
// **after** the cell has been written to air, which is the jar's order.
//
// **The particles are here now**, and they belong to phase 3 rather than to a
// pass of their own: `je` throws an `explode` and a `smoke` from every recorded
// cell *before* it asks whether there is a block there to drop, so a blast
// through open air smokes exactly as much as one through stone.

// **Why this is in `core/entity/` and not in `core/tick/`.** `je` is world
// code and would sit beside the block behaviours by rights, but it needs two
// things at once: `dropBlockAsItem` from `core/tick/` and `rayTraceBlocks`
// from `core/entity/`. `core/entity` already depends on `core/tick` and the
// reverse has never been true, so the explosion lives on the side that can
// reach both rather than inverting the direction for one class.

#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

using tick::TickWorld;

// `je`'s two literals: the cube it takes its directions from and the step.
inline constexpr int kExplosionRays = 16;
inline constexpr float kExplosionStep = 0.3f;

// `dropBlockAsItemWithChance(world, x, y, z, metadata, 0.3F)`.
inline constexpr float kExplosionDropChance = 0.3f;

// How far the record reaches; see the header for why 8 is enough.
inline constexpr int kExplosionMaxRadius = 8;
inline constexpr int kExplosionSpan = kExplosionMaxRadius * 2 + 1;

// One blast, from the ray cast to the last block falling. Held by the caller
// because the cell record is 616 bytes and this file will not allocate.
class Explosion {
public:
    // `khVar` in the jar is the exploder, and it is used for exactly two
    // things: `Block.getExplosionResistance(Entity)` -- which ignores it in
    // every block of this version -- and excluding itself from the entity list.
    // So there is no entity here; the caller simply does not offer itself to
    // `effectOn`.
    Explosion(double x, double y, double z, float strength)
        : x_(x), y_(y), z_(z), strength_(strength)
    {
    }

    // Phase 1. Plays `random.explode` and records every cell the rays reach.
    // **Consumes `world.random()`**, one `nextFloat` per ray, which is what
    // makes two creepers in the same spot take different bites out of a wall.
    void cast(TickWorld& world);

    // Phase 2, once per entity the caller cares to offer, between `cast` and
    // `destroy`. Answers false for anything further than `strength * 2`.
    //
    // `damage` is `((d*d + d) / 2) * 8 * (strength*2) + 1` where `d` is the
    // distance falloff times the fraction of the entity that can see the
    // centre; the impulse is that same falloff along the line from the centre.
    bool effectOn(const TickWorld& world, double ex, double ey, double ez, const AABB& box,
                  int* damage, double* vx, double* vy, double* vz) const;

    // Phase 3. Destroys what phase 1 recorded, dropping three blocks in ten.
    // **Back to front over the recorded cells**, which is the jar's order
    // (`for (i = list.size() - 1; i >= 0; i--)`) and matters because each
    // removal notifies its neighbours and a torch may fall into a cell that
    // has not been reached yet.
    void destroy(TickWorld& world);

    double x() const { return x_; }
    double y() const { return y_; }
    double z() const { return z_; }

    // `f * 2.0F`, the radius the entity half works in.
    float reach() const { return strength_ * 2.0f; }

    // How many cells phase 1 recorded, for the test suite and the debug page.
    int cellCount() const { return cells_; }

private:
    bool marked(int ix, int iy, int iz) const;
    void mark(int ix, int iy, int iz);

    double x_, y_, z_;
    float strength_;
    int cells_ = 0;

    // The centre cell the record is relative to, fixed on `cast`.
    i32 originX_ = 0;
    int originY_ = 0;
    i32 originZ_ = 0;

    static constexpr int kBits = kExplosionSpan * kExplosionSpan * kExplosionSpan;
    static constexpr int kWords = (kBits + 31) / 32;
    u32 record_[kWords] = {};
};

// `cn.a(Lkh;DDDF)V` for a caller with nothing to damage but blocks -- the two
// phases with no entity between them. The creeper does not use this; it needs
// the middle one.
void createExplosion(TickWorld& world, double x, double y, double z, float strength);

// `ly.a(Lkh;)F` -- getExplosionResistance, which is `blockResistance / 5`. See
// the header for why the multiply is here.
float explosionResistance(i32 x, int y, i32 z, const TickWorld& world);

// `cn.a(Laj;Lcf;)F` -- **getBlockDensity**: the fraction of a lattice of points
// across `box` that can see `(x, y, z)` without a block in the way. The lattice
// spacing is `1 / (2 * extent + 1)` per axis, so a bigger entity is sampled
// with more points rather than with the same points spread further.
float blockDensity(const TickWorld& world, double x, double y, double z, const AABB& box);

}  // namespace mc::entity
