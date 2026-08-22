#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "java_random_vectors.hpp"

#include <cstring>

using namespace mc;
using mc::test::kRandomBoundCount;
using mc::test::kRandomBounds;
using mc::test::kRandomDraws;
using mc::test::kRandomVectorCount;
using mc::test::kRandomVectors;

namespace {

// Floats and doubles are compared as bit patterns, never with ==. Two reasons,
// and the second is the one that matters: == would call 0.0 and -0.0 equal and
// every NaN unequal to itself, and -- much more to the point -- a comparison
// that tolerates the last bit is exactly the comparison that lets a noise
// lattice drift. Seed-exact means every bit.
u32 bits(float value)
{
    u32 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

u64 bits(double value)
{
    u64 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

}  // namespace

// The scramble in the constructor. Cheap to check and it localises a whole
// class of failure: if this is wrong, everything below is wrong too, and this
// is the one line that says so.
TEST(java_random_seed_scramble)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        CHECK_EQ(random.rawSeed(), want.rawSeed);
    }
}

TEST(java_random_next_int)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        for (int i = 0; i < kRandomDraws; ++i) {
            CHECK_EQ(random.nextInt(), want.nextInt[i]);
        }
    }
}

// Covers all three branches: the power-of-two shortcut (16), the ordinary
// modulus path (17, 3), and the retry loop (2^30+1, which rejects about half
// its draws -- see tools/genref.java for why that bound was chosen).
TEST(java_random_next_int_bounded)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        for (int b = 0; b < kRandomBoundCount; ++b) {
            JavaRandom random(want.seed);
            for (int i = 0; i < kRandomDraws; ++i) {
                CHECK_EQ(random.nextInt(kRandomBounds[b]), want.nextIntBound[b][i]);
            }
        }
    }
}

// The signed addition of the low word is the whole point: a negative low word
// borrows from the high word, so this is not an or.
TEST(java_random_next_long)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        for (int i = 0; i < kRandomDraws; ++i) {
            CHECK_EQ(random.nextLong(), want.nextLong[i]);
        }
    }
}

TEST(java_random_next_float)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        for (int i = 0; i < kRandomDraws; ++i) {
            CHECK_EQ(bits(random.nextFloat()), want.nextFloatBits[i]);
        }
    }
}

// The generator's workhorse -- the noise lattice is built out of nextDouble --
// so this is the single most load-bearing case in the file.
TEST(java_random_next_double)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        for (int i = 0; i < kRandomDraws; ++i) {
            CHECK_EQ(bits(random.nextDouble()), want.nextDoubleBits[i]);
        }
    }
}

TEST(java_random_next_boolean)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        for (int i = 0; i < kRandomDraws; ++i) {
            CHECK_EQ(u8(random.nextBoolean() ? 1 : 0), want.nextBoolean[i]);
        }
    }
}

// **This one is allowed to be the first to break, and that is why it is here.**
// nextGaussian is the only method whose result is not fully specified: the JDK
// calls StrictMath.log, contractually fdlibm, while we call whatever libm the
// host or newlib provides. If this fails and nothing else does, the finding is
// "our log differs from fdlibm in the last bit", not "the generator is broken"
// -- and the fix is to transcribe fdlibm's log rather than to loosen the
// comparison. See the note in java_random.hpp.
TEST(java_random_next_gaussian)
{
    for (int v = 0; v < kRandomVectorCount; ++v) {
        const auto& want = kRandomVectors[v];
        JavaRandom random(want.seed);
        for (int i = 0; i < kRandomDraws; ++i) {
            CHECK_EQ(bits(random.nextGaussian()), want.nextGaussianBits[i]);
        }
    }
}

// setSeed has to reset the Gaussian cache, or a reseeded generator hands back a
// value derived from the *old* seed. Java documents this; it is easy to miss
// because it only shows up when a caller takes an odd number of Gaussians.
TEST(java_random_set_seed_clears_gaussian_cache)
{
    const auto& want = kRandomVectors[0];

    JavaRandom fresh(want.seed);
    const double first = fresh.nextGaussian();

    JavaRandom reused(12345);
    reused.nextGaussian();  // leaves the cached second value behind
    reused.setSeed(want.seed);

    CHECK_EQ(bits(reused.nextGaussian()), bits(first));
}

// The stream is one sequence, not one per method: interleaving draws must give
// the same numbers as taking them separately would from the same position.
// Pins that nothing caches or looks ahead.
TEST(java_random_stream_is_shared_across_methods)
{
    JavaRandom mixed(42);
    const i32 a = mixed.nextInt();
    const i64 b = mixed.nextLong();
    const double c = mixed.nextDouble();

    JavaRandom replay(42);
    CHECK_EQ(replay.nextInt(), a);
    CHECK_EQ(replay.nextLong(), b);
    CHECK_EQ(bits(replay.nextDouble()), bits(c));
}
