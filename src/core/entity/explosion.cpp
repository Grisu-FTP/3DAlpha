// `je.a(Lcn;Lkh;DDDF)V`, the three phases and the two world queries under them.
// See explosion.hpp.

#include "core/entity/explosion.hpp"

#include "core/entity/particle.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::entity {
namespace {

// `f2 * 0.75F` in the class file, which is the flat cost of a step on top of
// whatever the block in it charges.
constexpr float kStepCost = kExplosionStep * 0.75f;

// The `+ 0.3F` inside `(getExplosionResistance(entity) + 0.3F) * 0.3F`. The
// same 0.3 as the step by coincidence of the class file, not by rule, so it is
// its own name.
constexpr float kBlockToll = 0.3f;

}  // namespace

float explosionResistance(i32 x, int y, i32 z, const TickWorld& world)
{
    const block::BlockId id = world.blockAt(x, y, z);
    if (id == block::kAir) {
        return 0.0f;
    }
    const block::BlockDef& def = block::def(id);
    // `ly.b(F)` stores `setResistance * 3` and `ly.c(F)` raises it to
    // `hardness * 5`; every block in this version sets hardness first, so the
    // stored `blockResistance` is the larger of the two. Then `/ 5`.
    const float stored = std::max(def.resistance * 3.0f, def.hardness * 5.0f);
    return stored / 5.0f;
}

float blockDensity(const TickWorld& world, double x, double y, double z, const AABB& box)
{
    const double stepX = 1.0 / ((box.maxX - box.minX) * 2.0 + 1.0);
    const double stepY = 1.0 / ((box.maxY - box.minY) * 2.0 + 1.0);
    const double stepZ = 1.0 / ((box.maxZ - box.minZ) * 2.0 + 1.0);

    int seen = 0;
    int total = 0;
    // The loop variables are floats in the jar and the accumulation is a float
    // add of a double step, which is why they are written the same way here: a
    // double counter visits a different number of points.
    for (float fx = 0.0f; fx <= 1.0f; fx = float(double(fx) + stepX)) {
        for (float fy = 0.0f; fy <= 1.0f; fy = float(double(fy) + stepY)) {
            for (float fz = 0.0f; fz <= 1.0f; fz = float(double(fz) + stepZ)) {
                const double px = box.minX + (box.maxX - box.minX) * double(fx);
                const double py = box.minY + (box.maxY - box.minY) * double(fy);
                const double pz = box.minZ + (box.maxZ - box.minZ) * double(fz);

                const double dx = x - px;
                const double dy = y - py;
                const double dz = z - pz;
                const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (length < 1e-12) {
                    ++seen;
                } else {
                    const RayHit hit = rayTrace(world, px, py, pz, dx / length,
                                                                dy / length, dz / length, length);
                    if (!hit.hit) {
                        ++seen;
                    }
                }
                ++total;
            }
        }
    }
    // **`(float) i2 / (float) i3`**, and it is worth saying so: the decompiler
    // renders it as `return i2 / i3;` with both sides `int`, which would make
    // this a truth value rather than a fraction. The class file has `i2f` on
    // both operands before the `fdiv`.
    return total == 0 ? 0.0f : float(seen) / float(total);
}

bool Explosion::marked(int ix, int iy, int iz) const
{
    const int bit = (ix * kExplosionSpan + iy) * kExplosionSpan + iz;
    return (record_[bit >> 5] & (1u << (bit & 31))) != 0;
}

void Explosion::mark(int ix, int iy, int iz)
{
    const int bit = (ix * kExplosionSpan + iy) * kExplosionSpan + iz;
    u32& word = record_[bit >> 5];
    const u32 mask = 1u << (bit & 31);
    if ((word & mask) == 0) {
        word |= mask;
        ++cells_;
    }
}

void Explosion::cast(TickWorld& world)
{
    JavaRandom& rand = world.random();

    world.playSoundAt("random.explode", x_, y_, z_, 4.0f,
                      (1.0f + (rand.nextFloat() - rand.nextFloat()) * 0.2f) * 0.7f);

    originX_ = MathHelper::floorDouble(x_);
    originY_ = int(MathHelper::floorDouble(y_));
    originZ_ = MathHelper::floorDouble(z_);

    for (int i = 0; i < kExplosionRays; ++i) {
        for (int j = 0; j < kExplosionRays; ++j) {
            for (int k = 0; k < kExplosionRays; ++k) {
                // **Only the shell of the cube**, which is 1,352 of its 4,096
                // cells. Every interior cell would give a direction some shell
                // cell already gives.
                if (i != 0 && i != kExplosionRays - 1 && j != 0 && j != kExplosionRays - 1
                    && k != 0 && k != kExplosionRays - 1) {
                    continue;
                }
                double dx = double(float(i) / (kExplosionRays - 1.0f) * 2.0f - 1.0f);
                double dy = double(float(j) / (kExplosionRays - 1.0f) * 2.0f - 1.0f);
                double dz = double(float(k) / (kExplosionRays - 1.0f) * 2.0f - 1.0f);
                const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= length;
                dy /= length;
                dz /= length;

                float power = strength_ * (0.7f + rand.nextFloat() * 0.6f);
                double px = x_;
                double py = y_;
                double pz = z_;
                while (power > 0.0f) {
                    const i32 bx = MathHelper::floorDouble(px);
                    const int by = int(MathHelper::floorDouble(py));
                    const i32 bz = MathHelper::floorDouble(pz);
                    if (world.blockAt(bx, by, bz) != block::kAir) {
                        power -= (explosionResistance(bx, by, bz, world) + kBlockToll)
                                 * kExplosionStep;
                    }
                    if (power > 0.0f) {
                        const int ix = bx - originX_ + kExplosionMaxRadius;
                        const int iy = by - originY_ + kExplosionMaxRadius;
                        const int iz = bz - originZ_ + kExplosionMaxRadius;
                        // A blast strong enough to leave the record would need
                        // a strength above 8.4; nothing in this version has
                        // one. The cell is skipped rather than wrapped, which
                        // can only ever leave a block standing.
                        if (ix >= 0 && ix < kExplosionSpan && iy >= 0 && iy < kExplosionSpan
                            && iz >= 0 && iz < kExplosionSpan) {
                            mark(ix, iy, iz);
                        }
                    }
                    px += dx * double(kExplosionStep);
                    py += dy * double(kExplosionStep);
                    pz += dz * double(kExplosionStep);
                    power -= kStepCost;
                }
            }
        }
    }
}

bool Explosion::effectOn(const TickWorld& world, double ex, double ey, double ez,
                         const AABB& box, int* damage, double* vx, double* vy,
                         double* vz) const
{
    const double span = double(reach());
    // `khVar2.e(d, d2, d3)` -- getDistance, from the entity's *position* and
    // not from the middle of its box.
    const double dx = ex - x_;
    const double dy = ey - y_;
    const double dz = ez - z_;
    const double falloff = std::sqrt(dx * dx + dy * dy + dz * dz) / span;
    if (falloff > 1.0) {
        return false;
    }

    const double length = MathHelper::sqrtDouble(dx * dx + dy * dy + dz * dz);
    const double nx = dx / length;
    const double ny = dy / length;
    const double nz = dz / length;

    const double push = (1.0 - falloff) * double(blockDensity(world, x_, y_, z_, box));
    *damage = int((push * push + push) / 2.0 * 8.0 * span + 1.0);
    *vx = nx * push;
    *vy = ny * push;
    *vz = nz * push;
    return true;
}

void Explosion::destroy(TickWorld& world)
{
    // Back to front, which is `for (i = list.size() - 1; i >= 0; i--)`. The
    // record is a bitset rather than the jar's `HashSet` copied into an
    // `ArrayList`, so "back" is the last index of the cube and not the order
    // the rays found them -- the *set* is identical, and a set has no order to
    // match. What the reversal buys is the jar's property that a removal's
    // neighbour notification lands on cells that are still standing.
    for (int index = kExplosionSpan * kExplosionSpan * kExplosionSpan - 1; index >= 0;
         --index) {
        const int iz = index % kExplosionSpan;
        const int iy = (index / kExplosionSpan) % kExplosionSpan;
        const int ix = index / (kExplosionSpan * kExplosionSpan);
        if (!marked(ix, iy, iz)) {
            continue;
        }
        const i32 bx = originX_ + ix - kExplosionMaxRadius;
        const int by = originY_ + iy - kExplosionMaxRadius;
        const i32 bz = originZ_ + iz - kExplosionMaxRadius;

        // **The puff goes out before the cell is read**, because in the jar it
        // goes out before the cell is *cleared* -- and on an air cell it goes
        // out anyway. `je` runs its particle block on every recorded cell and
        // only then asks whether there is a block there to drop, so a blast
        // through open air still smokes.
        //
        // Two particles per cell, from a random point inside it: an `explode`
        // **halfway back towards the centre of the blast** and a `smoke` at the
        // point itself, both thrown outward at a speed that falls off with
        // distance. The halving is what makes the fireball read as a ball and
        // the smoke as a shell around it.
        {
            JavaRandom& rand = world.random();
            const double px = double(bx) + double(rand.nextFloat());
            const double py = double(by) + double(rand.nextFloat());
            const double pz = double(bz) + double(rand.nextFloat());
            double ox = px - x_;
            double oy = py - y_;
            double oz = pz - z_;
            const double length = MathHelper::sqrtDouble(ox * ox + oy * oy + oz * oz);
            ox /= length;
            oy /= length;
            oz /= length;
            // `0.5 / (distance / strength + 0.1)` times a product of two draws
            // plus 0.3 -- so a cell near the middle is thrown hard and one at
            // the rim barely moves.
            const double speed = 0.5 / (length / double(strength_) + 0.1)
                                 * double(rand.nextFloat() * rand.nextFloat() + 0.3f);
            const double mx = ox * speed;
            const double my = oy * speed;
            const double mz = oz * speed;
            world.spawnParticle(int(ParticleKind::Explode), (px + x_) / 2.0,
                                (py + y_) / 2.0, (pz + z_) / 2.0, mx, my, mz);
            world.spawnParticle(int(ParticleKind::Smoke), px, py, pz, mx, my, mz);
        }

        const block::BlockId id = world.blockAt(bx, by, bz);
        if (id == block::kAir) {
            continue;
        }
        const u8 metadata = world.dataAt(bx, by, bz);
        tick::dropBlockAsItem(world, bx, by, bz, id, metadata, kExplosionDropChance);
        world.setBlockWithNotify(bx, by, bz, block::kAir);
        // `ly.c(Lcn;III)V` -- onBlockDestroyedByExplosion -- is empty on
        // `Block` and the one override in this version is TNT's. **The cell is
        // already air by the time it runs**, which is the jar's order at
        // offsets 1126-1151 of `je.a()` and is why a blast can light TNT
        // without the entity landing inside the block it came from.
        //
        // It re-primes rather than detonating, on a 10..29-tick fuse, which is
        // what makes a chain ripple. See core/tick/drop.hpp.
        if (block::def(id).tick == block::TickBehaviour::Tnt) {
            tick::tntDestroyedByExplosion(world, bx, by, bz);
        }
    }
}

void createExplosion(TickWorld& world, double x, double y, double z, float strength)
{
    Explosion blast(x, y, z, strength);
    blast.cast(world);
    blast.destroy(world);
}

}  // namespace mc::entity
