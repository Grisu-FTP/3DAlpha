#include "impl/worldgen/alpha_nobiome/noise.hpp"

namespace mc::worldgen {

PerlinNoise::PerlinNoise(JavaRandom& random)
{
    // Three offsets first, then the permutation. The order is the seed: moving
    // either half would shift every number the rest of the generator draws.
    xOffset_ = random.nextDouble() * 256.0;
    yOffset_ = random.nextDouble() * 256.0;
    zOffset_ = random.nextDouble() * 256.0;

    for (i32 i = 0; i < 256; ++i) {
        permutation_[i] = i;
    }

    // Fisher-Yates, with the original's own bound. Note `nextInt(256 - i) + i`
    // rather than the more usual `nextInt(i + 1)`: it shuffles forward, and the
    // duplicate into the upper half happens inside the same pass rather than in
    // a second loop afterwards -- so entry i+256 is written before entry i is
    // read again by a later swap. Reordering that changes the table.
    for (i32 i = 0; i < 256; ++i) {
        const i32 j = random.nextInt(256 - i) + i;
        const i32 swap = permutation_[i];
        permutation_[i] = permutation_[j];
        permutation_[j] = swap;
        permutation_[i + 256] = permutation_[i];
    }
}

void PerlinNoise::populate(double* out, double x, double y, double z, int xSize, int ySize,
                           int zSize, double xScale, double yScale, double zScale,
                           double amplitude) const
{
    int index = 0;
    const double inverseAmplitude = 1.0 / amplitude;

    // The Y-slab cache. `lastYCell` starts at -1, which no masked cell can be,
    // so the first sample always computes. See the header for why this is
    // observable behaviour rather than an optimisation we could drop.
    i32 lastYCell = -1;
    double n1 = 0.0;
    double n2 = 0.0;
    double n3 = 0.0;
    double n4 = 0.0;

    for (int xi = 0; xi < xSize; ++xi) {
        double fx = (x + double(xi)) * xScale + xOffset_;
        const i32 xFloor = floorToInt(fx);
        const i32 xCell = xFloor & 255;
        fx -= double(xFloor);
        const double fadeX = fade(fx);

        for (int zi = 0; zi < zSize; ++zi) {
            double fz = (z + double(zi)) * zScale + zOffset_;
            const i32 zFloor = floorToInt(fz);
            const i32 zCell = zFloor & 255;
            fz -= double(zFloor);
            const double fadeZ = fade(fz);

            for (int yi = 0; yi < ySize; ++yi) {
                double fy = (y + double(yi)) * yScale + yOffset_;
                const i32 yFloor = floorToInt(fy);
                const i32 yCell = yFloor & 255;
                fy -= double(yFloor);
                const double fadeY = fade(fy);

                if (yi == 0 || yCell != lastYCell) {
                    lastYCell = yCell;

                    // The eight cube corners, addressed the way Perlin's
                    // reference does: fold x into the table, add y, fold again,
                    // add z.
                    const i32 a = permutation_[xCell] + yCell;
                    const i32 aa = permutation_[a] + zCell;
                    const i32 ab = permutation_[a + 1] + zCell;
                    const i32 b = permutation_[xCell + 1] + yCell;
                    const i32 ba = permutation_[b] + zCell;
                    const i32 bb = permutation_[b + 1] + zCell;

                    // Four X-interpolations: the pairs differ in Y and then in
                    // Z, which is what leaves the Y and Z blends to below.
                    n1 = lerp(fadeX, gradient(permutation_[aa], fx, fy, fz),
                              gradient(permutation_[ba], fx - 1.0, fy, fz));
                    n2 = lerp(fadeX, gradient(permutation_[ab], fx, fy - 1.0, fz),
                              gradient(permutation_[bb], fx - 1.0, fy - 1.0, fz));
                    n3 = lerp(fadeX, gradient(permutation_[aa + 1], fx, fy, fz - 1.0),
                              gradient(permutation_[ba + 1], fx - 1.0, fy, fz - 1.0));
                    n4 = lerp(fadeX, gradient(permutation_[ab + 1], fx, fy - 1.0, fz - 1.0),
                              gradient(permutation_[bb + 1], fx - 1.0, fy - 1.0, fz - 1.0));
                }

                const double t1 = lerp(fadeY, n1, n2);
                const double t2 = lerp(fadeY, n3, n4);
                out[index++] += lerp(fadeZ, t1, t2) * inverseAmplitude;
            }
        }
    }
}

double PerlinNoise::sample(double x, double y, double z) const
{
    double fx = x + xOffset_;
    double fy = y + yOffset_;
    double fz = z + zOffset_;

    const i32 xFloor = floorToInt(fx);
    const i32 yFloor = floorToInt(fy);
    const i32 zFloor = floorToInt(fz);

    const i32 xCell = xFloor & 255;
    const i32 yCell = yFloor & 255;
    const i32 zCell = zFloor & 255;

    fx -= double(xFloor);
    fy -= double(yFloor);
    fz -= double(zFloor);

    const double fadeX = fade(fx);
    const double fadeY = fade(fy);
    const double fadeZ = fade(fz);

    const i32 a = permutation_[xCell] + yCell;
    const i32 aa = permutation_[a] + zCell;
    const i32 ab = permutation_[a + 1] + zCell;
    const i32 b = permutation_[xCell + 1] + yCell;
    const i32 ba = permutation_[b] + zCell;
    const i32 bb = permutation_[b + 1] + zCell;

    // X first, then Y, then Z -- the same nesting the lattice fill uses, and
    // the same one Perlin's reference does.
    return lerp(fadeZ,
                lerp(fadeY,
                     lerp(fadeX, gradient(permutation_[aa], fx, fy, fz),
                          gradient(permutation_[ba], fx - 1.0, fy, fz)),
                     lerp(fadeX, gradient(permutation_[ab], fx, fy - 1.0, fz),
                          gradient(permutation_[bb], fx - 1.0, fy - 1.0, fz))),
                lerp(fadeY,
                     lerp(fadeX, gradient(permutation_[aa + 1], fx, fy, fz - 1.0),
                          gradient(permutation_[ba + 1], fx - 1.0, fy, fz - 1.0)),
                     lerp(fadeX, gradient(permutation_[ab + 1], fx, fy - 1.0, fz - 1.0),
                          gradient(permutation_[bb + 1], fx - 1.0, fy - 1.0, fz - 1.0))));
}

double OctaveNoise::sample2D(double x, double y) const
{
    double sum = 0.0;
    double amplitude = 1.0;
    for (int i = 0; i < octaves_; ++i) {
        // z is zero, always: `v.a(DD)D` forwards to `a(first, second, 0.0)`.
        sum += generators_[i].sample(x * amplitude, y * amplitude, 0.0) / amplitude;
        amplitude /= 2.0;
    }
    return sum;
}

OctaveNoise::OctaveNoise(JavaRandom& random, int octaves)
{
    octaves_ = octaves < kMaxOctaves ? octaves : kMaxOctaves;
    // Every generator draws from the same Random, in order, so this loop is
    // itself part of the seed derivation.
    for (int i = 0; i < octaves_; ++i) {
        generators_[i] = PerlinNoise(random);
    }
}

void OctaveNoise::populate(double* out, int count, double x, double y, double z, int xSize,
                           int ySize, int zSize, double xScale, double yScale,
                           double zScale) const
{
    for (int i = 0; i < count; ++i) {
        out[i] = 0.0;
    }

    // Amplitude halves per octave, and the lattice divides its contribution by
    // it -- so the *later* octaves dominate and are also the lowest frequency.
    // Backwards from a conventional fBm, and correct for this game.
    double amplitude = 1.0;
    for (int i = 0; i < octaves_; ++i) {
        generators_[i].populate(out, x, y, z, xSize, ySize, zSize, xScale * amplitude,
                                yScale * amplitude, zScale * amplitude, amplitude);
        amplitude /= 2.0;
    }
}

}  // namespace mc::worldgen
