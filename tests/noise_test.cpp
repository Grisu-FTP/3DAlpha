#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/noise.hpp"
#include "noise_vectors.hpp"

#include <cstring>
#include <vector>

using namespace mc;
using mc::test::kNoiseCaseCount;
using mc::test::kNoiseCases;
using mc::test::kPerlinVectorCount;
using mc::test::kPerlinVectors;
using worldgen::OctaveNoise;
using worldgen::PerlinNoise;

namespace {

u64 bits(double value)
{
    u64 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

double fromBits(u64 value)
{
    double out = 0.0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

}  // namespace

// Construction, checked apart from evaluation on purpose. If the permutation is
// wrong, every noise value is wrong and the lattice test tells you nothing
// about which half is at fault; this one does.
TEST(perlin_construction_matches_the_jar)
{
    for (int v = 0; v < kPerlinVectorCount; ++v) {
        const auto& want = kPerlinVectors[v];
        JavaRandom random(want.seed);
        const PerlinNoise noise(random);

        CHECK_EQ(bits(noise.xOffset()), want.offsetBits[0]);
        CHECK_EQ(bits(noise.yOffset()), want.offsetBits[1]);
        CHECK_EQ(bits(noise.zOffset()), want.offsetBits[2]);

        for (int i = 0; i < 512; ++i) {
            CHECK_EQ(noise.permutation()[i], want.permutation[i]);
        }
    }
}

// The upper half of the permutation is the lower half repeated -- that is what
// lets the evaluator index `perm[x + 1]` without a bounds check when x is 255.
// Implied by the vectors above, but stated separately because it is the
// invariant the evaluator silently depends on.
TEST(perlin_permutation_upper_half_mirrors_the_lower)
{
    JavaRandom random(12345);
    const PerlinNoise noise(random);
    for (int i = 0; i < 256; ++i) {
        CHECK_EQ(noise.permutation()[i + 256], noise.permutation()[i]);
    }
}

// The whole lattice, against the jar's own output, bit for bit.
//
// The cases are the shapes the chunk provider actually asks for -- the 5x17x5
// terrain lattice at 684.412/80 and /160, the flat 2D fields, the surface pass
// at 1/32 -- plus degenerate shapes and negative coordinates. See
// tools/genref.java for why each one is in the list.
TEST(octave_noise_matches_the_jar_bit_for_bit)
{
    for (int c = 0; c < kNoiseCaseCount; ++c) {
        const auto& want = kNoiseCases[c];

        JavaRandom random(want.seed);
        const OctaveNoise noise(random, want.octaves);

        std::vector<double> out(usize(want.count), 0.0);
        noise.populate(out.data(), want.count, fromBits(want.xBits), fromBits(want.yBits),
                       fromBits(want.zBits), want.xSize, want.ySize, want.zSize,
                       fromBits(want.xScaleBits), fromBits(want.yScaleBits),
                       fromBits(want.zScaleBits));

        for (int i = 0; i < want.count; ++i) {
            CHECK_EQ(bits(out[i]), want.expected[i]);
        }
    }
}

// The output is Y-fastest, because the fill's innermost loop is Y and the
// output index just counts up. The loops are nested X, Z, Y -- not the order
// the argument list reads in -- so this is worth pinning on its own: a
// transposed buffer still passes a "does it look like noise" check and produces
// a world that is wrong in a way nobody would trace back to here.
TEST(octave_noise_output_is_y_fastest)
{
    JavaRandom random(4242);
    const OctaveNoise noise(random, 2);

    constexpr int kXs = 3;
    constexpr int kYs = 5;
    constexpr int kZs = 7;
    constexpr int kCount = kXs * kYs * kZs;

    std::vector<double> whole(kCount, 0.0);
    noise.populate(whole.data(), kCount, 0.0, 0.0, 0.0, kXs, kYs, kZs, 1.3, 0.7, 2.1);

    // Ask for each (x, z) column on its own and check it lands where the whole
    // buffer says it should.
    for (int xi = 0; xi < kXs; ++xi) {
        for (int zi = 0; zi < kZs; ++zi) {
            JavaRandom columnRandom(4242);
            const OctaveNoise columnNoise(columnRandom, 2);

            std::vector<double> column(kYs, 0.0);
            columnNoise.populate(column.data(), kYs, double(xi), 0.0, double(zi), 1, kYs, 1, 1.3,
                                 0.7, 2.1);

            for (int yi = 0; yi < kYs; ++yi) {
                const int index = (xi * kZs + zi) * kYs + yi;
                CHECK_EQ(bits(whole[usize(index)]), bits(column[usize(yi)]));
            }
        }
    }
}

// The Y-slab cache, isolated.
//
// The cache does *not* make consecutive samples equal -- that was the obvious
// guess and it is wrong, because `fadeY` is still recomputed for every sample.
// What it does is interpolate between **stale corner gradients**: when two Y
// samples share an integer cell, the second blends using corners computed from
// the first one's fractional Y.
//
// So the way to see it is to ask for the same points twice, once in a batch and
// once one at a time. A single-sample call has `yi == 0` on its only iteration,
// which forces the recompute -- so per-sample calls are what a cacheless
// implementation would produce. With a Y scale under 1 the two disagree, and
// that disagreement *is* the cache.
//
// This matters because a textbook Perlin -- one that recomputes every sample,
// as any library would -- passes every "does it look like noise" check and
// generates a different world.
TEST(the_y_slab_cache_is_observable)
{
    constexpr int kYs = 8;
    constexpr double kYScale = 0.01;  // well under 1, so samples share a cell

    JavaRandom random(2024);
    const OctaveNoise noise(random, 1);

    std::vector<double> batched(kYs, 0.0);
    noise.populate(batched.data(), kYs, 0.0, 0.0, 0.0, 1, kYs, 1, 1.0, kYScale, 1.0);

    int disagreements = 0;
    for (int yi = 0; yi < kYs; ++yi) {
        JavaRandom singleRandom(2024);
        const OctaveNoise singleNoise(singleRandom, 1);

        double one = 0.0;
        singleNoise.populate(&one, 1, 0.0, double(yi), 0.0, 1, 1, 1, 1.0, kYScale, 1.0);

        if (bits(one) != bits(batched[usize(yi)])) {
            ++disagreements;
        }
    }

    // The first sample always agrees -- yi == 0 recomputes in both paths -- so
    // the most that can disagree is kYs - 1. Requiring several rather than all
    // of them leaves room for the span to cross a cell boundary, which depends
    // on the generator's random Y offset.
    CHECK(disagreements >= kYs / 2);
}

// The other half of the same claim: when the Y scale is large enough that every
// sample lands in its own integer cell, the cache never fires and the two paths
// agree exactly. a1.1.2's terrain lattice is in this regime -- its Y scale is
// 684.412/160, about 4.28 -- which is why the quirk above never affects a real
// world and still has to be reproduced.
TEST(the_y_slab_cache_never_fires_at_terrain_scale)
{
    constexpr int kYs = 17;
    constexpr double kYScale = 684.412 / 160.0;

    JavaRandom random(2024);
    const OctaveNoise noise(random, 1);

    std::vector<double> batched(kYs, 0.0);
    noise.populate(batched.data(), kYs, 0.0, 0.0, 0.0, 1, kYs, 1, 1.0, kYScale, 1.0);

    for (int yi = 0; yi < kYs; ++yi) {
        JavaRandom singleRandom(2024);
        const OctaveNoise singleNoise(singleRandom, 1);

        double one = 0.0;
        singleNoise.populate(&one, 1, 0.0, double(yi), 0.0, 1, 1, 1, 1.0, kYScale, 1.0);

        CHECK_EQ(bits(one), bits(batched[usize(yi)]));
    }
}
