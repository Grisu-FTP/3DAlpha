#include "impl/worldgen/alpha_nobiome/ore.hpp"

#include "blocks.hpp"
#include "core/block/registry.hpp"
#include "core/util/math_helper.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kStone = u8(mcver::Block::Stone);
constexpr u8 kSand = u8(mcver::Block::Sand);
constexpr u8 kClay = u8(mcver::Block::Clay);
constexpr u8 kWater = u8(mcver::Block::Water);
constexpr float kPi = 3.1415927f;

// `cn.f(III)` is getBlockMaterial, and the guard compares it against
// Material.water. Nothing in the block table names water's material directly,
// but flowing and still water share it -- the same identity fluid.cpp already
// relies on -- so asking the table for water's own material is exact and needs
// no new generated constant.
bool isWaterMaterial(u8 id)
{
    return block::def(id).material == block::def(kWater).material;
}

// The original's `(int)` cast, or a floor. See OreBounds in the header: at a
// non-negative value the two are the same function, so a world with the fix on
// differs from a1.1.2 only where a1.1.2 was already asymmetric.
i32 veinBound(double value, OreBounds bounds)
{
    if (bounds == OreBounds::FloorBounds) {
        const i32 truncated = i32(value);
        return (double(truncated) > value) ? truncated - 1 : truncated;
    }
    return i32(value);
}

// The squared, normalised offset of each block along one axis of a vein step:
// `n = (b + 0.5 - centre) / half`, squared. The original writes this inside
// the triple loop; it is the same expression on the same operands here, so the
// same bits, computed once per block along the axis instead of once per block
// in the box. The spread is at most 2 * veinSize / 16 + 1, so a1.1.2's largest
// vein (32) spans 6 blocks; a wider axis computes the rest when asked.
class AxisSquares {
public:
    AxisSquares(i32 min, i32 max, double centre, double half)
        : min_(min), centre_(centre), half_(half)
    {
        const i32 span = max - min + 1;
        count_ = span < kHoisted ? (span > 0 ? span : 0) : kHoisted;
        for (i32 k = 0; k < count_; ++k) {
            squared_[k] = compute(min + k);
        }
    }

    double at(i32 b) const
    {
        const i32 k = b - min_;
        return k < count_ ? squared_[k] : compute(b);
    }

private:
    static constexpr i32 kHoisted = 16;

    double compute(i32 b) const
    {
        const double n = (double(b) + 0.5 - centre_) / half_;
        return n * n;
    }

    double squared_[kHoisted];
    i32 min_;
    i32 count_;
    double centre_;
    double half_;
};

// The body shared by WorldGenMinable and WorldGenClay. In the original these
// are two classes with the same code; here the only differences are which
// block is replaced and which is placed.
bool growVein(PopulationView& view, JavaRandom& random, u8 replaceId, u8 placeId, i32 veinSize,
              i32 x, i32 y, i32 z, OreBounds bounds)
{
    const float angle = random.nextFloat() * kPi;

    // The two ends of the vein, offset from the centre along `angle`. All four
    // are float expressions widened to double afterwards -- the sine comes from
    // MathHelper's quantised table, not from libm.
    const double x1 =
        double(float(x + 8) + MathHelper::sin(angle) * float(veinSize) / 8.0f);
    const double x2 =
        double(float(x + 8) - MathHelper::sin(angle) * float(veinSize) / 8.0f);
    const double z1 =
        double(float(z + 8) + MathHelper::cos(angle) * float(veinSize) / 8.0f);
    const double z2 =
        double(float(z + 8) - MathHelper::cos(angle) * float(veinSize) / 8.0f);

    const double y1 = double(y + random.nextInt(3) + 2);
    const double y2 = double(y + random.nextInt(3) + 2);

    // **`<=`, not `<`.** The loop runs veinSize + 1 times, so the segment is
    // sampled at both endpoints. Off by one against the obvious reading, and
    // it changes both the vein's length and how many doubles it draws.
    for (i32 i = 0; i <= veinSize; ++i) {
        const double px = x1 + (x2 - x1) * double(i) / double(veinSize);
        const double py = y1 + (y2 - y1) * double(i) / double(veinSize);
        const double pz = z1 + (z2 - z1) * double(i) / double(veinSize);

        // One draw per step, so the number of draws depends on the vein size --
        // which is why the passes must run in the documented order.
        const double scale = random.nextDouble() * double(veinSize) / 16.0;
        const double spread =
            double(MathHelper::sin(float(i) * kPi / float(veinSize)) + 1.0f) * scale + 1.0;

        // The original computes this twice into two locals. They are the same
        // expression and the same value; kept as one, since nothing between
        // them draws or mutates.
        const double radiusXZ = spread;
        const double radiusY = spread;

        // **Where the negative-quadrant bug lives.** Six `(int)` casts, all
        // of which truncate toward zero; `veinBound` is the one place that
        // changes, and only when the world asked for it.
        const i32 minX = veinBound(px - radiusXZ / 2.0, bounds);
        const i32 maxX = veinBound(px + radiusXZ / 2.0, bounds);
        const i32 minY = veinBound(py - radiusY / 2.0, bounds);
        const i32 maxY = veinBound(py + radiusY / 2.0, bounds);
        const i32 minZ = veinBound(pz - radiusXZ / 2.0, bounds);
        const i32 maxZ = veinBound(pz + radiusXZ / 2.0, bounds);

        // **The same test on the same values, with less arithmetic.** Each of
        // nx, ny and nz depends on its own axis alone, so their squares are
        // worked out once per step instead of once per block -- a division
        // each, and a division is the dearest thing an ARM11 does in double.
        // See AxisSquares.
        //
        // And a row or column whose partial sum already reaches 1 is skipped:
        // adding a square cannot make a sum smaller (rounding is monotonic and
        // a square is never negative), so no block in it could pass. The
        // skipped blocks are exactly the ones whose `&&` never asked
        // blockAt, so nothing observable goes missing.
        const AxisSquares xs(minX, maxX, px, radiusXZ / 2.0);
        const AxisSquares ys(minY, maxY, py, radiusY / 2.0);
        const AxisSquares zs(minZ, maxZ, pz, radiusXZ / 2.0);

        for (i32 bx = minX; bx <= maxX; ++bx) {
            const double nx2 = xs.at(bx);
            if (nx2 >= 1.0) {
                continue;
            }
            for (i32 by = minY; by <= maxY; ++by) {
                const double nxy = nx2 + ys.at(by);
                if (nxy >= 1.0) {
                    continue;
                }
                for (i32 bz = minZ; bz <= maxZ; ++bz) {
                    if (nxy + zs.at(bz) < 1.0 && view.blockAt(bx, by, bz) == replaceId) {
                        view.setBlock(bx, by, bz, placeId);
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace

bool generateOreVein(PopulationView& view, JavaRandom& random, u8 blockId, i32 veinSize, i32 x,
                     i32 y, i32 z, OreBounds bounds)
{
    return growVein(view, random, kStone, blockId, veinSize, x, y, z, bounds);
}

bool generateClayPatch(PopulationView& view, JavaRandom& random, i32 patchSize, i32 x, i32 y,
                       i32 z, OreBounds bounds)
{
    // **The guard runs before any random draw.** A rejected patch consumes
    // nothing from the stream, so ten tries over dry land leave the sequence
    // exactly where they found it -- which is why this cannot be reordered with
    // the draws below.
    if (!isWaterMaterial(view.blockAt(x, y, z))) {
        return false;
    }
    return growVein(view, random, kSand, kClay, patchSize, x, y, z, bounds);
}

}  // namespace mc::worldgen
