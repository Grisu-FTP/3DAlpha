#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"

#include "core/util/math_helper.hpp"

#include "blocks.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kStone = u8(mcver::Block::Stone);
constexpr u8 kGrass = u8(mcver::Block::Grass);
constexpr u8 kDirt = u8(mcver::Block::Dirt);
constexpr u8 kBedrock = u8(mcver::Block::Bedrock);
constexpr u8 kSand = u8(mcver::Block::Sand);
constexpr u8 kGravel = u8(mcver::Block::Gravel);
constexpr u8 kWater = u8(mcver::Block::Water);
constexpr u8 kIce = u8(mcver::Block::Ice);

// x << 11 | z << 7 | y, Alpha's own column layout.
constexpr int blockIndex(int x, int y, int z)
{
    return (x << 11) | (z << 7) | y;
}

}  // namespace

// Eight generators from one Random, in this order and no other. Moving any one
// of them shifts every number every later generator draws, so this is the most
// order-sensitive code in the project.
//
// Built in the initialiser list rather than assigned in the body, and that is
// not a style choice: an OctaveNoise is 33 KB, so `x_ = OctaveNoise(...)`
// materialises a 33 KB temporary on the stack before the move. The 3DS build
// caught it -- "stack usage is 33216 bytes" against an 8 KB ceiling -- which is
// exactly what that flag is for. Initialiser-list construction builds each one
// in place.
//
// The order here must match the declaration order in the header, because C++
// runs it in declaration order whatever this list says. -Wreorder enforces it.
ChunkProvider::ChunkProvider(i64 seed, GeneratorOptions options)
    : random_(seed),
      options_(options),
      seed_(seed),
      minLimit_(random_, 16),
      maxLimit_(random_, 16),
      main_(random_, 8),
      sandGravel_(random_, 4),
      surfaceDepth_(random_, 4),
      scale_(random_, 10),
      depth_(random_, 16),
      // Constructed and then never read by anything in the game -- it is the
      // mob spawner noise, and a1.1.2 has no worldgen-time animal spawning,
      // checked across the whole jar. It stays because *constructing* it draws
      // eight octaves' worth of numbers out of the Random. Dropping it would
      // change nothing here and everything for any generator added after it.
      treeDensity_(random_, 8)
{
    // **Before anything can be on another thread.** MathHelper's table is one
    // 256 KB allocation built once, and building it lazily from the first sine
    // would put that write on the chunk worker while the main thread might be
    // reading the same flag. Doing it here means every later call finds it
    // already true. See math_helper.hpp.
    MathHelper::ensureBuilt();

}

void ChunkProvider::generateColumn(i32 chunkX, i32 chunkZ, u8* blocks)
{
    // No XOR with the world seed, unlike the population pass. Done in u64
    // because Java's multiply wraps and C++ signed overflow does not.
    const u64 seed = u64(i64(chunkX)) * 341873128712ULL + u64(i64(chunkZ)) * 132897987541ULL;
    random_.setSeed(i64(seed));

    generateTerrain(chunkX, chunkZ, blocks);
    replaceBlocksForBiome(chunkX, chunkZ, blocks);
    carveCaves(chunkX, chunkZ, blocks);
}

void ChunkProvider::carveCaves(i32 chunkX, i32 chunkZ, u8* blocks)
{
    caves_.generate(seed_, chunkX, chunkZ, blocks);
}

void ChunkProvider::initializeNoiseField(i32 x, i32 y, i32 z)
{
    // Both spellings of 684.412 are separate locals in the original, one for
    // horizontal and one for vertical. They are equal, and kept apart because
    // the divisors below are not.
    constexpr double kHorizontal = 684.412;
    constexpr double kVertical = 684.412;

    constexpr int kFlat = kLatticeX * kLatticeZ;

    scale_.populate(scaleField_, kFlat, double(x), double(y), double(z), kLatticeX, 1, kLatticeZ,
                    1.0, 0.0, 1.0);
    depth_.populate(depthField_, kFlat, double(x), double(y), double(z), kLatticeX, 1, kLatticeZ,
                    100.0, 0.0, 100.0);
    main_.populate(mainField_, kLatticeSize, double(x), double(y), double(z), kLatticeX, kLatticeY,
                   kLatticeZ, kHorizontal / 80.0, kVertical / 160.0, kHorizontal / 80.0);
    minLimit_.populate(minLimitField_, kLatticeSize, double(x), double(y), double(z), kLatticeX,
                       kLatticeY, kLatticeZ, kHorizontal, kVertical, kHorizontal);
    maxLimit_.populate(maxLimitField_, kLatticeSize, double(x), double(y), double(z), kLatticeX,
                       kLatticeY, kLatticeZ, kHorizontal, kVertical, kHorizontal);

    int index3d = 0;
    int index2d = 0;
    for (int xi = 0; xi < kLatticeX; ++xi) {
        for (int zi = 0; zi < kLatticeZ; ++zi) {
            // How "peaky" this column is, and how high its centre sits.
            double scale = (scaleField_[index2d] + 256.0) / 512.0;
            if (scale > 1.0) {
                scale = 1.0;
            }

            // Written once and never assigned again in the original. It is the
            // threshold of a bottom taper that therefore never fires, because
            // the loop variable it is compared against starts at 0. Kept as a
            // named constant rather than transcribed as a dead branch -- see
            // the note at the end of this function.
            constexpr double kBottomTaper = 0.0;

            double depth = depthField_[index2d] / 8000.0;
            if (depth < 0.0) {
                depth = -depth;
            }
            depth = depth * 3.0 - 3.0;
            if (depth < 0.0) {
                depth = depth / 2.0;
                if (depth < -1.0) {
                    depth = -1.0;
                }
                depth = depth / 1.4;
                depth = depth / 2.0;
                // An ocean: the peakiness term is thrown away entirely.
                scale = 0.0;
            } else {
                if (depth > 1.0) {
                    depth = 1.0;
                }
                depth = depth / 6.0;
            }

            scale = scale + 0.5;
            depth = depth * double(kLatticeY) / 16.0;
            const double centre = double(kLatticeY) / 2.0 + depth * 4.0;
            ++index2d;

            for (int yi = 0; yi < kLatticeY; ++yi) {
                // Distance from the column's centre height, scaled by how peaky
                // the column is. Below the centre it is multiplied by four,
                // which is what makes overhangs rarer underground than above.
                double offset = (double(yi) - centre) * 12.0 / scale;
                if (offset < 0.0) {
                    offset = offset * 4.0;
                }

                const double lower = minLimitField_[index3d] / 512.0;
                const double upper = maxLimitField_[index3d] / 512.0;
                const double blend = (mainField_[index3d] / 10.0 + 1.0) / 2.0;

                double density = 0.0;
                if (blend < 0.0) {
                    density = lower;
                } else if (blend > 1.0) {
                    density = upper;
                } else {
                    density = lower + (upper - lower) * blend;
                }
                density -= offset;

                // The top four lattice rows fade toward -10, which is what
                // stops terrain touching the world ceiling.
                //
                // **The interpolant is computed in float, not double**, and
                // then widened. That is not an accident of transcription: the
                // original emits i2f / fdiv / f2d, so the division happens at
                // single precision and the result carries float rounding into a
                // double expression. Computing it in double changes the last
                // bits of every block in the top 32 layers.
                if (yi > kLatticeY - 4) {
                    const float t = float(yi - (kLatticeY - 4)) / 3.0f;
                    density = density * (1.0 - double(t)) + (-10.0 * double(t));
                }

                // The dead bottom taper, transcribed as it reads so that the
                // reason it is dead is visible rather than assumed:
                //
                //     if ((double)yi < kBottomTaper) { ... fade toward -10 ... }
                //
                // kBottomTaper is 0.0 and yi starts at 0, so the condition is
                // never true for any lattice. Confirmed against the bytecode --
                // the local is written once, to zero, and read only here.
                static_assert(kBottomTaper == 0.0, "the bottom taper is dead only while this is 0");

                noiseField_[index3d] = density;
                ++index3d;
            }
        }
    }
}

void ChunkProvider::generateTerrain(i32 chunkX, i32 chunkZ, u8* blocks)
{
    initializeNoiseField(chunkX * kCells, 0, chunkZ * kCells);

    for (int xi = 0; xi < kCells; ++xi) {
        for (int zi = 0; zi < kCells; ++zi) {
            for (int yi = 0; yi < kLatticeY - 1; ++yi) {
                constexpr double kYStep = 0.125;  // 8 blocks per lattice cell

                // The cell's four vertical edges: value at the bottom, and the
                // per-block step toward the top.
                double n00 = noiseField_[((xi + 0) * kLatticeZ + (zi + 0)) * kLatticeY + yi];
                double n01 = noiseField_[((xi + 0) * kLatticeZ + (zi + 1)) * kLatticeY + yi];
                double n10 = noiseField_[((xi + 1) * kLatticeZ + (zi + 0)) * kLatticeY + yi];
                double n11 = noiseField_[((xi + 1) * kLatticeZ + (zi + 1)) * kLatticeY + yi];

                const double d00 =
                    (noiseField_[((xi + 0) * kLatticeZ + (zi + 0)) * kLatticeY + yi + 1] - n00) *
                    kYStep;
                const double d01 =
                    (noiseField_[((xi + 0) * kLatticeZ + (zi + 1)) * kLatticeY + yi + 1] - n01) *
                    kYStep;
                const double d10 =
                    (noiseField_[((xi + 1) * kLatticeZ + (zi + 0)) * kLatticeY + yi + 1] - n10) *
                    kYStep;
                const double d11 =
                    (noiseField_[((xi + 1) * kLatticeZ + (zi + 1)) * kLatticeY + yi + 1] - n11) *
                    kYStep;

                for (int ys = 0; ys < 8; ++ys) {
                    constexpr double kXStep = 0.25;  // 4 blocks per lattice cell

                    double a = n00;
                    double b = n01;
                    const double da = (n10 - n00) * kXStep;
                    const double db = (n11 - n01) * kXStep;

                    for (int xs = 0; xs < 4; ++xs) {
                        constexpr double kZStep = 0.25;

                        const int y = yi * 8 + ys;
                        int index = blockIndex(xs + xi * 4, y, zi * 4);
                        constexpr int kZStride = 128;

                        double density = a;
                        const double dDensity = (b - a) * kZStep;

                        for (int zs = 0; zs < 4; ++zs) {
                            u8 id = kAir;
                            if (y < kSeaLevel) {
                                // Ice sits one block below sea level, on top of
                                // the water rather than replacing it.
                                id = (options_.snowCovered && y >= kSeaLevel - 1) ? kIce : kWater;
                            }
                            // Stone wins over both, so the sea is carved out of
                            // the same density field rather than laid on top.
                            if (density > 0.0) {
                                id = kStone;
                            }
                            blocks[index] = id;

                            index += kZStride;
                            density += dDensity;
                        }
                        a += da;
                        b += db;
                    }
                    n00 += d00;
                    n01 += d01;
                    n10 += d10;
                    n11 += d11;
                }
            }
        }
    }
}

void ChunkProvider::replaceBlocksForBiome(i32 chunkX, i32 chunkZ, u8* blocks)
{
    constexpr double kScale = 0.03125;  // 1/32

    // **Sand and gravel come from the same generator**, called twice with the
    // axes shuffled and a magic Y offset on the second. That is the original's
    // own trick for getting two uncorrelated fields out of one octave stack,
    // and it is why `sandGravel_` is one member rather than two.
    sandGravel_.populate(sandField_, 256, double(chunkX * 16), double(chunkZ * 16), 0.0, 16, 16, 1,
                         kScale, kScale, 1.0);
    sandGravel_.populate(gravelField_, 256, double(chunkZ * 16), 109.0134, double(chunkX * 16), 16,
                         1, 16, kScale, 1.0, kScale);
    surfaceDepth_.populate(surfaceDepthField_, 256, double(chunkX * 16), double(chunkZ * 16), 0.0,
                           16, 16, 1, kScale * 2.0, kScale * 2.0, kScale * 2.0);

    // The loop variables are named for what they index in the *block array*:
    // `i` carries the 2048 stride and `j` the 128 stride, so i is X and j is Z.
    // The noise arrays are read as [i + j * 16], which is a transposition
    // relative to how they were generated -- kept exactly, because it is what
    // decides where beaches land.
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            const bool sandy = sandField_[usize(i + j * 16)] + random_.nextDouble() * 0.2 > 0.0;
            const bool gravelly =
                gravelField_[usize(i + j * 16)] + random_.nextDouble() * 0.2 > 3.0;
            // How deep the soil goes. Truncated toward zero, so a negative
            // stone noise can make this zero or less, which is the "bare stone
            // mountain" case handled below.
            const int soilDepth =
                int(surfaceDepthField_[usize(i + j * 16)] / 3.0 + 3.0 + random_.nextDouble() * 0.25);

            int remaining = -1;
            u8 topBlock = kGrass;
            u8 fillBlock = kDirt;

            for (int y = 127; y >= 0; --y) {
                const int index = (i * 16 + j) * 128 + y;

                // **This draw happens on every one of the 128 steps**, whatever
                // the outcome, and it is the reason bedrock has a ragged
                // underside. Hoisting it out of the loop or guarding it with a
                // `y < 5` test would be the obvious optimisation and would
                // desynchronise every column after the first.
                if (y <= random_.nextInt(6) - 1) {
                    blocks[usize(index)] = kBedrock;
                    continue;
                }

                const u8 current = blocks[usize(index)];
                if (current == kAir) {
                    // Air resets the run, so an overhang gets its own soil
                    // layer underneath rather than continuing the one above.
                    remaining = -1;
                    continue;
                }
                if (current != kStone) {
                    continue;
                }

                if (remaining == -1) {
                    if (soilDepth <= 0) {
                        topBlock = kAir;
                        fillBlock = kStone;
                    } else if (y >= kSeaLevel - 4 && y <= kSeaLevel + 1) {
                        // Only this band gets a beach. Above it, the defaults
                        // set before the loop (grass over dirt) stand.
                        topBlock = kGrass;
                        fillBlock = kDirt;
                        if (gravelly) {
                            topBlock = kAir;
                            fillBlock = kGravel;
                        }
                        if (sandy) {
                            topBlock = kSand;
                            fillBlock = kSand;
                        }
                    }

                    // A gravel shore below the waterline gets water on top
                    // rather than nothing -- this is what fills the gap the
                    // `topBlock = air` above just made.
                    if (y < kSeaLevel && topBlock == kAir) {
                        topBlock = kWater;
                    }

                    remaining = soilDepth;
                    blocks[usize(index)] = (y >= kSeaLevel - 1) ? topBlock : fillBlock;
                } else if (remaining > 0) {
                    --remaining;
                    blocks[usize(index)] = fillBlock;
                }
            }
        }
    }
}

}  // namespace mc::worldgen
