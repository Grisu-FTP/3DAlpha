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

// The body shared by WorldGenMinable and WorldGenClay. In the original these
// are two classes with the same code; here the only differences are which
// block is replaced and which is placed.
bool growVein(PopulationView& view, JavaRandom& random, u8 replaceId, u8 placeId, i32 veinSize,
              i32 x, i32 y, i32 z)
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

        const i32 minX = i32(px - radiusXZ / 2.0);
        const i32 maxX = i32(px + radiusXZ / 2.0);
        const i32 minY = i32(py - radiusY / 2.0);
        const i32 maxY = i32(py + radiusY / 2.0);
        const i32 minZ = i32(pz - radiusXZ / 2.0);
        const i32 maxZ = i32(pz + radiusXZ / 2.0);

        for (i32 bx = minX; bx <= maxX; ++bx) {
            const double nx = (double(bx) + 0.5 - px) / (radiusXZ / 2.0);
            for (i32 by = minY; by <= maxY; ++by) {
                const double ny = (double(by) + 0.5 - py) / (radiusY / 2.0);
                for (i32 bz = minZ; bz <= maxZ; ++bz) {
                    const double nz = (double(bz) + 0.5 - pz) / (radiusXZ / 2.0);
                    if (nx * nx + ny * ny + nz * nz < 1.0 &&
                        view.blockAt(bx, by, bz) == replaceId) {
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
                     i32 y, i32 z)
{
    return growVein(view, random, kStone, blockId, veinSize, x, y, z);
}

bool generateClayPatch(PopulationView& view, JavaRandom& random, i32 patchSize, i32 x, i32 y,
                       i32 z)
{
    // **The guard runs before any random draw.** A rejected patch consumes
    // nothing from the stream, so ten tries over dry land leave the sequence
    // exactly where they found it -- which is why this cannot be reordered with
    // the draws below.
    if (!isWaterMaterial(view.blockAt(x, y, z))) {
        return false;
    }
    return growVein(view, random, kSand, kClay, patchSize, x, y, z);
}

}  // namespace mc::worldgen
