#include "framework.hpp"

#include "core/util/strict_math.hpp"
#include "strict_math_vectors.hpp"

#include <cmath>
#include <cstring>
#include <limits>

using namespace mc;
using mc::test::kLogVectorCount;
using mc::test::kLogVectors;

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

// The constants are the part of a transcription most likely to be wrong and
// least likely to look wrong: a mistyped digit in the middle of a 17-digit
// literal reads as fine and computes as almost-fine. fdlibm's source carries
// each one's bit pattern in a comment, so both forms are checked against each
// other here and neither is trusted alone.
TEST(fdlibm_constants_match_their_published_bit_patterns)
{
    using namespace mc::strictmath::detail;

    CHECK_EQ(bits(kLn2Hi), 0x3FE62E42FEE00000ULL);
    CHECK_EQ(bits(kLn2Lo), 0x3DEA39EF35793C76ULL);
    CHECK_EQ(bits(kTwo54), 0x4350000000000000ULL);

    CHECK_EQ(bits(kLg1), 0x3FE5555555555593ULL);
    CHECK_EQ(bits(kLg2), 0x3FD999999997FA04ULL);
    CHECK_EQ(bits(kLg3), 0x3FD2492494229359ULL);
    CHECK_EQ(bits(kLg4), 0x3FCC71C51D8E78AFULL);
    CHECK_EQ(bits(kLg5), 0x3FC7466496CB03DEULL);
    CHECK_EQ(bits(kLg6), 0x3FC39A09D078C69FULL);
    CHECK_EQ(bits(kLg7), 0x3FC2F112DF3E5244ULL);
}

// 380 values from a real JVM's StrictMath.log: powers of two and their
// immediate neighbours, the near-1 polynomial's range, denormals, the extremes,
// and 200 values drawn exactly the way nextGaussian draws them. Bit-for-bit --
// see the note in java_random_test.cpp about why a tolerance would defeat the
// purpose.
TEST(strict_math_log_matches_the_jvm_bit_for_bit)
{
    for (int i = 0; i < kLogVectorCount; ++i) {
        const double x = fromBits(kLogVectors[i].xBits);
        CHECK_EQ(bits(strictmath::log(x)), kLogVectors[i].logBits);
    }
}

// The edge cases fdlibm handles with explicit early returns rather than with
// the polynomial. Java's StrictMath is specified to produce these, and they are
// reached through division by zero, which is well-defined for doubles but looks
// alarming enough in the source to be worth pinning.
TEST(strict_math_log_handles_zero_negatives_and_infinities)
{
    CHECK(std::isinf(strictmath::log(0.0)) && strictmath::log(0.0) < 0.0);
    CHECK(std::isinf(strictmath::log(-0.0)) && strictmath::log(-0.0) < 0.0);
    CHECK(std::isnan(strictmath::log(-1.0)));

    const double inf = std::numeric_limits<double>::infinity();
    CHECK(std::isinf(strictmath::log(inf)) && strictmath::log(inf) > 0.0);
    CHECK(std::isnan(strictmath::log(std::numeric_limits<double>::quiet_NaN())));

    // log(1) is exactly zero, not merely very small -- fdlibm has a branch for
    // it and getting that branch wrong would be invisible in a tolerance test.
    CHECK_EQ(bits(strictmath::log(1.0)), bits(0.0));
}
