#include "framework.hpp"

#include "core/util/java_cast.hpp"

#include <cmath>
#include <limits>

using namespace mc;

namespace {

constexpr i32 kIntMax = 2147483647;
constexpr i32 kIntMin = -2147483647 - 1;

}  // namespace

// Every expectation below was **taken from a JVM**, not from reading JLS 5.1.3
// and believing it. The program was:
//
//     for (double d : ds) { int i = (int)d; System.out.println(i + " " + (d < i ? i - 1 : i)); }
//
// run on the same machine that runs genref, and the second column is
// MathHelper.floor_double's body. The spec and the JVM agree here, but the
// whole project's rule is that the oracle is the thing that runs.
TEST(java_narrowing_matches_the_jvm)
{
    // NaN is zero, which is the one case that has no arithmetic justification
    // at all and has to be looked up.
    CHECK_EQ(javaToInt(std::numeric_limits<double>::quiet_NaN()), 0);

    // Out of range in either direction saturates. C++ leaves this undefined and
    // x86-64's cvttsd2si answers INT_MIN for *both* signs, so the positive case
    // is the one that silently disagrees with Java on the host.
    CHECK_EQ(javaToInt(3e9), kIntMax);
    CHECK_EQ(javaToInt(-3e9), kIntMin);
    CHECK_EQ(javaToInt(std::numeric_limits<double>::infinity()), kIntMax);
    CHECK_EQ(javaToInt(-std::numeric_limits<double>::infinity()), kIntMin);

    // The boundary, where an off-by-one in the guard would clamp a value that
    // Java converts exactly. 2147483647.5 truncates to a representable int and
    // must *not* be clamped -- writing the guard as `value >= double(INT_MAX)`
    // gets this wrong, because 2147483647 is not representable as a double and
    // the comparand rounds up.
    CHECK_EQ(javaToInt(2147483647.5), kIntMax);
    CHECK_EQ(javaToInt(2147483648.0), kIntMax);
    CHECK_EQ(javaToInt(-2147483648.5), kIntMin);
    CHECK_EQ(javaToInt(-2147483649.0), kIntMin);

    // And ordinary values still truncate toward zero rather than flooring.
    CHECK_EQ(javaToInt(2.5), 2);
    CHECK_EQ(javaToInt(-2.5), -2);
}

// The floor the generator actually calls. Its interesting property is at the
// bottom: `(int)d` has already clamped to Integer.MIN_VALUE, `d < i` is still
// true because d really is smaller, and Java's `i - 1` **wraps around to
// Integer.MAX_VALUE**. That is not a rounding detail, it is a sign flip, and
// the JVM run above confirms every negative overflow floors to +2147483647.
//
// In C++ that decrement is signed overflow and therefore undefined, so it is
// spelled out in unsigned arithmetic. This case is reachable: it is the
// negative Far Lands.
TEST(the_floor_wraps_at_the_bottom_the_way_java_does)
{
    CHECK_EQ(javaFloorToInt(-3e9), kIntMax);
    CHECK_EQ(javaFloorToInt(-2147483649.0), kIntMax);
    CHECK_EQ(javaFloorToInt(-std::numeric_limits<double>::infinity()), kIntMax);

    // Positive overflow does not wrap, because d < i is false there.
    CHECK_EQ(javaFloorToInt(3e9), kIntMax);
    CHECK_EQ(javaFloorToInt(2147483648.0), kIntMax);

    // NaN: (int)NaN is 0, and NaN < 0 is false, so the answer is 0 rather than
    // -1. Worth stating, because a floor written with std::floor would give
    // NaN and then convert to something else entirely.
    CHECK_EQ(javaFloorToInt(std::numeric_limits<double>::quiet_NaN()), 0);

    // In range, it is a genuine floor and not a truncation.
    CHECK_EQ(javaFloorToInt(-2.5), -3);
    CHECK_EQ(javaFloorToInt(2.5), 2);
    CHECK_EQ(javaFloorToInt(-0.0), 0);
}

// The float overload's guards cannot be the double overload's numbers -- 2^31+1
// is not representable in 24 bits of mantissa. This checks the bound that is
// actually written, and the neighbouring float on either side.
TEST(the_float_overload_saturates_on_representable_bounds)
{
    CHECK_EQ(javaToInt(std::numeric_limits<float>::quiet_NaN()), 0);
    CHECK_EQ(javaToInt(3e9f), kIntMax);
    CHECK_EQ(javaToInt(-3e9f), kIntMin);
    CHECK_EQ(javaToInt(2147483648.0f), kIntMax);
    CHECK_EQ(javaToInt(-2147483648.0f), kIntMin);

    // The largest float below 2^31 is 2147483520, which converts exactly and
    // must not be clamped.
    CHECK_EQ(javaToInt(2147483520.0f), 2147483520);
    CHECK_EQ(javaToInt(-2.5f), -2);
}
