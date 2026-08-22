#include "impl/worldgen/alpha_nobiome/trees.hpp"

#include "blocks.hpp"
#include "core/block/registry.hpp"
#include "core/util/java_cast.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/noise.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kGrass = u8(mcver::Block::Grass);
constexpr u8 kDirt = u8(mcver::Block::Dirt);
constexpr u8 kWood = u8(mcver::Block::Log);
constexpr u8 kLeaves = u8(mcver::Block::Leaves);

i32 absOf(i32 value)
{
    return value < 0 ? -value : value;
}

}  // namespace

TreeBatch treeBatchFor(const OctaveNoise& density, JavaRandom& random, i32 blockX, i32 blockZ)
{
    // Half a block per noise unit. The scale is a local in the original,
    // written out as `0.5` twice rather than once, which is why it appears
    // here as a named constant rather than folded into the call.
    constexpr double kScale = 0.5;

    // **Note the axis.** `lp.a(DD)D` forwards to `a(first, second, 0.0)`, so
    // the chunk's z ends up on the noise's *y* axis with z pinned at zero.
    // Passing it as z gives a perfectly plausible forest that is not this
    // seed's -- see OctaveNoise::sample2D.
    const double noise = density.sample2D(double(blockX) * kScale, double(blockZ) * kScale);

    // The division by 3 is integer only at the end: everything inside the
    // parentheses is double, and the truncation is Java's cast. It cannot
    // overflow an int here -- eight octaves of Perlin are bounded well inside
    // a few hundred -- but it goes through the same helper as everything else
    // so that the rule is one rule.
    TreeBatch batch;
    batch.count = javaToInt((noise / 8.0 + random.nextDouble() * 4.0 + 4.0) / 3.0);
    if (batch.count < 0) {
        batch.count = 0;
    }
    if (random.nextInt(10) == 0) {
        ++batch.count;
    }

    // **Per chunk, not per tree.** One roll decides the kind for every tree in
    // the batch, so a chunk is all ordinary trees or all big ones.
    batch.big = random.nextInt(10) == 0;
    return batch;
}

bool generateTree(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z)
{
    // Four to six. Drawn before any of the checks below, so even a tree that
    // cannot be planted still costs this one number.
    const i32 height = random.nextInt(3) + 4;

    if (y < 1 || y + height + 1 > 128) {
        return false;
    }

    // Room to grow: a column of air (or existing leaves) shaped like the tree
    // that is about to be built. Note it tolerates **leaves** as well as air,
    // which is what lets a forest interlock rather than thin itself out.
    bool clear = true;
    for (i32 yy = y; yy <= y + 1 + height; ++yy) {
        i32 radius = 1;
        if (yy == y) {
            radius = 0;  // the trunk's own base needs one column, not nine
        }
        if (yy >= y + 1 + height - 2) {
            radius = 2;  // the canopy is wider than the trunk
        }

        for (i32 px = x - radius; px <= x + radius && clear; ++px) {
            for (i32 pz = z - radius; pz <= z + radius && clear; ++pz) {
                if (yy >= 0 && yy < 128) {
                    const u8 id = view.blockAt(px, yy, pz);
                    if (id != kAir && id != kLeaves) {
                        clear = false;
                    }
                } else {
                    clear = false;
                }
            }
        }
    }
    if (!clear) {
        return false;
    }

    const u8 below = view.blockAt(x, y - 1, z);
    if ((below != kGrass && below != kDirt) || y >= 128 - height - 1) {
        return false;
    }

    // Grass under a tree becomes dirt. The original writes this before placing
    // anything else, and it is a real block change rather than cosmetic --
    // the height map does not move, but a flower planted here later will find
    // dirt rather than grass, and both are legal ground so it makes no
    // difference. It would if the ground rules were narrower.
    view.setBlock(x, y - 1, z, kDirt);

    // The canopy: four layers, the lowest two wide and the top two narrow.
    for (i32 yy = y - 3 + height; yy <= y + height; ++yy) {
        const i32 depth = yy - (y + height);  // 0 at the top, -3 at the bottom
        // Truncating division, and the sign matters: -3 / 2 is -1 in Java and
        // in C++, giving radius 2, where a floored division would give -2 and
        // a radius of 3. Both languages truncate, so this is one of the few
        // places the obvious spelling is also the right one.
        const i32 radius = 1 - depth / 2;

        for (i32 px = x - radius; px <= x + radius; ++px) {
            const i32 dx = px - x;
            for (i32 pz = z - radius; pz <= z + radius; ++pz) {
                const i32 dz = pz - z;

                // **The corners of each layer are the only place a random
                // number is drawn**, and only when both offsets are at the
                // radius -- Java's `||` short-circuits, so an edge that is not
                // a corner never reaches the draw. Getting that wrong shifts
                // the stream for every tree in the chunk.
                //
                // A corner survives on a coin flip, except on the top layer
                // (depth 0), where it is always cut. That is what rounds the
                // canopy off.
                const bool corner = absOf(dx) == radius && absOf(dz) == radius;
                const bool place =
                    !corner || (random.nextInt(2) != 0 && depth != 0);
                if (!place) {
                    continue;
                }

                // `Block.opaqueCubeLookup`, not the live isOpaqueCube -- see
                // core/block/block_def.hpp. Leaves do not overwrite anything
                // solid, so a tree growing into a hillside is clipped by it.
                if (!block::def(view.blockAt(px, yy, pz)).opaqueCube) {
                    view.setBlock(px, yy, pz, kLeaves);
                }
            }
        }
    }

    // The trunk goes in last, over the leaves it just placed. It replaces air
    // and leaves and nothing else, so a trunk never punches through terrain.
    for (i32 yy = 0; yy < height; ++yy) {
        const u8 id = view.blockAt(x, y + yy, z);
        if (id == kAir || id == kLeaves) {
            view.setBlock(x, y + yy, z, kWood);
        }
    }

    return true;
}

}  // namespace mc::worldgen
