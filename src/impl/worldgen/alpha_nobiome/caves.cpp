#include "impl/worldgen/alpha_nobiome/caves.hpp"

#include "blocks.hpp"
#include "core/util/math_helper.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kStone = u8(mcver::Block::Stone);
constexpr u8 kGrass = u8(mcver::Block::Grass);
constexpr u8 kDirt = u8(mcver::Block::Dirt);
constexpr u8 kFlowingWater = u8(mcver::Block::FlowingWater);
constexpr u8 kWater = u8(mcver::Block::Water);

// Flowing lava, id 10 -- not the still lava at 11. Read off the jar rather than
// assumed: the field the carver uses is constructed with `bipush 10`.
constexpr u8 kLava = u8(mcver::Block::FlowingLava);

constexpr float kPi = 3.1415927f;
constexpr float kHalfPi = 1.5707964f;

}  // namespace

void CaveGenerator::generate(i64 worldSeed, i32 chunkX, i32 chunkZ, u8* blocks)
{
    MathHelper::ensureBuilt();

    random_.setSeed(worldSeed);
    // Java's `/` truncates toward zero and nextLong() is negative half the
    // time, so this is **not** a shift: (-5)/2*2+1 is -3, while (-5 >> 1) << 1
    // is -6. Both odd multipliers, different worlds.
    const i64 strideX = (random_.nextLong() / 2) * 2 + 1;
    const i64 strideZ = (random_.nextLong() / 2) * 2 + 1;

    // 17 x 17 chunks: a tunnel begun eight chunks away can still reach here.
    for (i32 cellX = chunkX - kRange; cellX <= chunkX + kRange; ++cellX) {
        for (i32 cellZ = chunkZ - kRange; cellZ <= chunkZ + kRange; ++cellZ) {
            // Wrapping multiply and XOR against the world seed, in u64 because
            // signed overflow is undefined here and defined in Java.
            const u64 seed = u64(i64(cellX)) * u64(strideX) + u64(i64(cellZ)) * u64(strideZ);
            random_.setSeed(i64(seed ^ u64(worldSeed)));
            recursiveGenerate(cellX, cellZ, chunkX, chunkZ, blocks);
        }
    }
}

void CaveGenerator::recursiveGenerate(i32 cellX, i32 cellZ, i32 originX, i32 originZ, u8* blocks)
{
    // Nested draws, innermost first: nextInt(40) bounds a bound that bounds a
    // bound. The result is heavily skewed toward zero, and then thrown away
    // entirely fourteen times in fifteen -- which is what makes caves sparse.
    i32 attempts = random_.nextInt(random_.nextInt(random_.nextInt(40) + 1) + 1);
    if (random_.nextInt(15) != 0) {
        attempts = 0;
    }

    for (i32 i = 0; i < attempts; ++i) {
        const double x = double(cellX * 16 + random_.nextInt(16));
        // Two nested draws, so shallow depths are far more likely than deep
        // ones -- caves cluster near the surface rather than spreading evenly.
        const double y = double(random_.nextInt(random_.nextInt(120) + 8));
        const double z = double(cellZ * 16 + random_.nextInt(16));

        i32 branches = 1;
        if (random_.nextInt(4) == 0) {
            carveRoom(originX, originZ, blocks, x, y, z);
            branches += random_.nextInt(4);
        }

        for (i32 j = 0; j < branches; ++j) {
            const float yaw = random_.nextFloat() * kPi * 2.0f;
            const float pitch = (random_.nextFloat() - 0.5f) * 2.0f / 8.0f;
            float width = random_.nextFloat() * 2.0f + random_.nextFloat();
            carveNode(originX, originZ, blocks, x, y, z, width, yaw, pitch, 0, 0, 1.0);
        }
    }
}

void CaveGenerator::carveRoom(i32 originX, i32 originZ, u8* blocks, double x, double y, double z)
{
    carveNode(originX, originZ, blocks, x, y, z, 1.0f + random_.nextFloat() * 6.0f, 0.0f, 0.0f, -1,
              -1, 0.5);
}

void CaveGenerator::carveNode(i32 originX, i32 originZ, u8* blocks, double x, double y, double z,
                              float width, float yaw, float pitch, i32 step, i32 maxSteps,
                              double heightScale)
{
    const double centreX = double(originX * 16 + 8);
    const double centreZ = double(originZ * 16 + 8);

    float yawChange = 0.0f;
    float pitchChange = 0.0f;

    // A fresh generator per node, seeded from the shared one. Everything below
    // draws from this, not from `random_` -- so a node's shape is independent
    // of how many nodes ran before it.
    JavaRandom random(random_.nextLong());

    if (maxSteps <= 0) {
        const i32 limit = kRange * 16 - 16;
        maxSteps = limit - random.nextInt(limit / 4);
    }

    bool isRoom = false;
    if (step == -1) {
        step = maxSteps / 2;
        isRoom = true;
    }

    const i32 branchPoint = random.nextInt(maxSteps / 2) + maxSteps / 4;
    // A steep tunnel keeps 92 % of its pitch each step instead of 70 %, so it
    // dives instead of levelling out.
    const bool steep = random.nextInt(6) == 0;

    for (; step < maxSteps; ++step) {
        // Radius tapers to nothing at both ends via a half sine. The whole
        // expression up to the widening is float -- including the redundant
        // `* 1.0f`, which is exact and kept for the shape of the line.
        const double horizRadius =
            1.5 + double(MathHelper::sin(float(step) * kPi / float(maxSteps)) * width * 1.0f);
        const double vertRadius = horizRadius * heightScale;

        const float cosPitch = MathHelper::cos(pitch);
        const float sinPitch = MathHelper::sin(pitch);
        x += double(MathHelper::cos(yaw) * cosPitch);
        y += double(sinPitch);
        z += double(MathHelper::sin(yaw) * cosPitch);

        pitch *= steep ? 0.92f : 0.7f;
        pitch += pitchChange * 0.1f;
        yaw += yawChange * 0.1f;
        pitchChange *= 0.9f;
        yawChange *= 0.75f;
        pitchChange += (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 2.0f;
        yawChange += (random.nextFloat() - random.nextFloat()) * random.nextFloat() * 4.0f;

        if (!isRoom && step == branchPoint && width > 1.0f) {
            // Two children at right angles, each too thin to branch again --
            // see the note in the header about depth.
            carveNode(originX, originZ, blocks, x, y, z, random.nextFloat() * 0.5f + 0.5f,
                      yaw - kHalfPi, pitch / 3.0f, step, maxSteps, 1.0);
            carveNode(originX, originZ, blocks, x, y, z, random.nextFloat() * 0.5f + 0.5f,
                      yaw + kHalfPi, pitch / 3.0f, step, maxSteps, 1.0);
            return;
        }

        // Three steps in four are skipped outright. The position still advanced
        // above, so this thins the carving without shortening the tunnel.
        if (!isRoom && random.nextInt(4) == 0) {
            continue;
        }

        // Give up entirely once the node cannot reach this column even if every
        // remaining step went straight at it.
        const double dx = x - centreX;
        const double dz = z - centreZ;
        const double stepsLeft = double(maxSteps - step);
        const double reach = double(width + 2.0f + 16.0f);
        if (dx * dx + dz * dz - stepsLeft * stepsLeft > reach * reach) {
            return;
        }

        // Outside this chunk entirely: skip carving but keep tunnelling.
        if (x < centreX - 16.0 - horizRadius * 2.0 || z < centreZ - 16.0 - horizRadius * 2.0 ||
            x > centreX + 16.0 + horizRadius * 2.0 || z > centreZ + 16.0 + horizRadius * 2.0) {
            continue;
        }

        i32 x0 = MathHelper::floorDouble(x - horizRadius) - originX * 16 - 1;
        i32 x1 = MathHelper::floorDouble(x + horizRadius) - originX * 16 + 1;
        i32 y0 = MathHelper::floorDouble(y - vertRadius) - 1;
        i32 y1 = MathHelper::floorDouble(y + vertRadius) + 1;
        i32 z0 = MathHelper::floorDouble(z - horizRadius) - originZ * 16 - 1;
        i32 z1 = MathHelper::floorDouble(z + horizRadius) - originZ * 16 + 1;

        if (x0 < 0) x0 = 0;
        if (x1 > 16) x1 = 16;
        if (y0 < 1) y0 = 1;    // never carve into bedrock's layer
        if (y1 > 120) y1 = 120;
        if (z0 < 0) z0 = 0;
        if (z1 > 16) z1 = 16;

        // Refuse to carve anywhere near water -- this is what stops caves
        // draining oceans. **Only the shell of the box is tested**: an interior
        // column jumps straight to the bottom, which is what the `by = y0` in
        // the else branch below does.
        bool hitWater = false;
        for (i32 bx = x0; !hitWater && bx < x1; ++bx) {
            for (i32 bz = z0; !hitWater && bz < z1; ++bz) {
                for (i32 by = y1 + 1; !hitWater && by >= y0 - 1; --by) {
                    const i32 index = (bx * 16 + bz) * 128 + by;
                    if (by >= 0 && by < 128) {
                        const u8 id = blocks[usize(index)];
                        if (id == kFlowingWater || id == kWater) {
                            hitWater = true;
                        }
                        if (by != y0 - 1 && bx != x0 && bx != x1 - 1 && bz != z0 && bz != z1 - 1) {
                            by = y0;
                        }
                    }
                }
            }
        }
        if (hitWater) {
            continue;
        }

        for (i32 bx = x0; bx < x1; ++bx) {
            const double nx = (double(bx + originX * 16) + 0.5 - x) / horizRadius;
            for (i32 bz = z0; bz < z1; ++bz) {
                const double nz = (double(bz + originZ * 16) + 0.5 - z) / horizRadius;

                // **The index leads the height by one.** It starts at y1 while
                // the loop starts at y1 - 1, and both step down together, so
                // every block written sits one above the height its ellipsoid
                // test was computed for. That is what the original does -- the
                // decrement is at the tail of the loop, not the head -- and
                // "fixing" it moves every cave surface down a block.
                i32 index = (bx * 16 + bz) * 128 + y1;
                bool hitGrass = false;

                for (i32 by = y1 - 1; by >= y0; --by) {
                    const double ny = (double(by) + 0.5 - y) / vertRadius;

                    // The -0.7 floor makes the ellipsoid flat-bottomed, which
                    // is why cave floors are walkable rather than round.
                    if (ny > -0.7 && nx * nx + ny * ny + nz * nz < 1.0) {
                        const u8 id = blocks[usize(index)];
                        if (id == kGrass) {
                            hitGrass = true;
                        }
                        if (id == kStone || id == kDirt || id == kGrass) {
                            if (by < 10) {
                                blocks[usize(index)] = kLava;
                            } else {
                                blocks[usize(index)] = kAir;
                                // Carving out from under a grass block leaves
                                // the dirt beneath it exposed, so it becomes
                                // grass in turn.
                                if (hitGrass && blocks[usize(index - 1)] == kDirt) {
                                    blocks[usize(index - 1)] = kGrass;
                                }
                            }
                        }
                    }
                    --index;
                }
            }
        }

        if (isRoom) {
            return;
        }
    }
}

}  // namespace mc::worldgen
