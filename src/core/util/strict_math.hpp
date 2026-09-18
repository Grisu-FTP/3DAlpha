#pragma once

// java.lang.StrictMath, for the parts that world generation depends on.
//
// **Why this file exists.** Java has two maths libraries. `Math` is allowed to
// use whatever the platform offers, within an error bound; `StrictMath` is
// contractually the *fdlibm* algorithms, bit for bit, "to ensure the
// portability of Java programs". Anything whose result feeds a seeded random
// stream has to match StrictMath exactly, because a one-ulp difference does not
// stay a one-ulp difference -- it changes a branch, which changes how many
// numbers are drawn, which desynchronises everything after it.
//
// That is not hypothetical here. `JavaRandom::nextGaussian` calls log, and the
// first run against real JVM vectors disagreed in the last bit on the host:
//
//     bits(random.nextGaussian()) == 4607821503525903751, expected ...750
//
// glibc's log is correctly rounded. fdlibm's is not. Java specifies the one
// that is not, so that is the one we need. (newlib on the 3DS is fdlibm-derived
// and may well already agree -- which would be worse, not better, because then
// host and console would disagree with each other and only one of them would be
// right.)
//
// The transcription below is fdlibm 5.3's `__ieee754_log`, the same source
// StrictMath is specified against. Every constant is checked against its
// published bit pattern by tests/strict_math_test.cpp, and the whole function
// is checked against 380 vectors dumped from a real JVM, so a mistranscribed
// digit fails the build rather than quietly bending the terrain.
//
// fdlibm is Copyright (C) 1993 by Sun Microsystems, Inc., developed at SunSoft,
// and is freely distributable provided the notice is preserved. **This is the
// one third-party work whose code is in this tree**, so that condition binds a
// source release as much as a binary one. The notice is carried in
// core/util/about.cpp, shown on the Options -> Info row, and written out in
// docs/licences.md.

#include "core/util/types.hpp"

#include <cstring>

namespace mc::strictmath {

namespace detail {

// The high and low words of a double, fdlibm's __HI/__LO, done through memcpy
// rather than a union so it is defined behaviour rather than merely something
// every compiler happens to allow.
inline u64 toBits(double value)
{
    u64 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

inline double fromBits(u64 bits)
{
    double out = 0.0;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

inline i32 highWord(double value)
{
    return i32(u32(toBits(value) >> 32));
}

inline u32 lowWord(double value)
{
    return u32(toBits(value) & 0xFFFFFFFFULL);
}

inline double withHighWord(double value, u32 high)
{
    const u64 bits = (toBits(value) & 0xFFFFFFFFULL) | (u64(high) << 32);
    return fromBits(bits);
}

// fdlibm's constants, with the bit patterns its source carries in comments.
// tests/strict_math_test.cpp asserts each decimal literal really is the
// commented pattern, which is the check that catches a fat-fingered digit.
inline constexpr double kLn2Hi = 6.93147180369123816490e-01;  // 3fe62e42 fee00000
inline constexpr double kLn2Lo = 1.90821492927058770002e-10;  // 3dea39ef 35793c76
inline constexpr double kTwo54 = 1.80143985094819840000e+16;  // 43500000 00000000

inline constexpr double kLg1 = 6.666666666666735130e-01;  // 3FE55555 55555593
inline constexpr double kLg2 = 3.999999999940941908e-01;  // 3FD99999 9997FA04
inline constexpr double kLg3 = 2.857142874366239149e-01;  // 3FD24924 94229359
inline constexpr double kLg4 = 2.222219843214978396e-01;  // 3FCC71C5 1D8E78AF
inline constexpr double kLg5 = 1.818357216161805012e-01;  // 3FC74664 96CB03DE
inline constexpr double kLg6 = 1.531383769920937332e-01;  // 3FC39A09 D078C69F
inline constexpr double kLg7 = 1.479819860511658591e-01;  // 3FC2F112 DF3E5244

}  // namespace detail

// fdlibm __ieee754_log, transcribed whole.
//
// The shape, for anyone reading it rather than trusting it: reduce x to
// 2^k * (1+f) with sqrt(2)/2 < 1+f < sqrt(2), then compute log(1+f) from
// s = f/(2+f) using an odd polynomial in s*s, and reassemble as
// k*ln2 + log(1+f). ln2 is carried in two pieces so the k*ln2 term keeps its
// low bits. The `i|j` sign test picks which of two algebraically equivalent
// reassociations to use -- they differ in rounding, and which one is chosen is
// part of the specified result.
inline double log(double x)
{
    using namespace detail;

    static const double zero = 0.0;

    i32 hx = highWord(x);
    const u32 lx = lowWord(x);

    i32 k = 0;
    if (hx < 0x00100000) {  // x < 2**-1022, or zero, or negative
        if (((hx & 0x7FFFFFFF) | i32(lx)) == 0) {
            return -kTwo54 / zero;  // log(+-0) = -inf
        }
        if (hx < 0) {
            return (x - x) / zero;  // log(negative) = NaN
        }
        k -= 54;
        x *= kTwo54;  // subnormal: scale up and account for it in k
        hx = highWord(x);
    }
    if (hx >= 0x7FF00000) {
        return x + x;  // inf or NaN through unchanged
    }

    k += (hx >> 20) - 1023;
    hx &= 0x000FFFFF;
    const i32 i0 = (hx + 0x95F64) & 0x100000;
    x = withHighWord(x, u32(hx | (i0 ^ 0x3FF00000)));  // normalise to x or x/2
    k += (i0 >> 20);

    const double f = x - 1.0;
    double dk = 0.0;

    if ((0x000FFFFF & (2 + hx)) < 3) {  // |f| < 2**-20
        if (f == zero) {
            if (k == 0) {
                return zero;
            }
            dk = double(k);
            return dk * kLn2Hi + dk * kLn2Lo;
        }
        const double r = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) {
            return f - r;
        }
        dk = double(k);
        return dk * kLn2Hi - ((r - dk * kLn2Lo) - f);
    }

    const double s = f / (2.0 + f);
    dk = double(k);
    const double z = s * s;
    i32 i = hx - 0x6147A;
    const double w = z * z;
    const i32 j = 0x6B851 - hx;
    const double t1 = w * (kLg2 + w * (kLg4 + w * kLg6));
    const double t2 = z * (kLg1 + w * (kLg3 + w * (kLg5 + w * kLg7)));
    i |= j;
    const double r = t2 + t1;

    if (i > 0) {
        const double hfsq = 0.5 * f * f;
        if (k == 0) {
            return f - (hfsq - s * (hfsq + r));
        }
        return dk * kLn2Hi - ((hfsq - (s * (hfsq + r) + dk * kLn2Lo)) - f);
    }
    if (k == 0) {
        return f - s * (f - r);
    }
    return dk * kLn2Hi - ((s * (f - r) - dk * kLn2Lo) - f);
}

}  // namespace mc::strictmath
