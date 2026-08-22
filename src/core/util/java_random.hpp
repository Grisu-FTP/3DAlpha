#pragma once

// java.util.Random, reimplemented exactly.
//
// Every number a1.1.2's world generator consumes comes out of this class, so
// "seed-exact" means "this file is bit-identical to the JDK's". That is a
// realistic goal rather than a hopeful one, because java.util.Random is one of
// the few things in the Java library whose algorithm is *specified* rather than
// left to the implementation: the class documentation gives the linear
// congruential generator, its constants, and the exact expression for every
// derived method. A JVM that deviated would be non-conforming.
//
// The specification, from the JDK docs:
//
//     seed = (seed * 0x5DEECE66D + 0xB) mod 2^48
//     next(bits) = (int)(seed >>> (48 - bits))
//
// and a constructor that scrambles its argument with `(seed ^ 0x5DEECE66D)`.
//
// Two places need care in C++ and are commented where they occur: the rejection
// loop in nextInt(bound) is written in Java to rely on *signed overflow*, which
// is undefined behaviour here and would trip UBSan; and nextGaussian reaches
// for StrictMath, which is not the same thing as libm. See below.
//
// Nothing in this file allocates, throws, or touches global state. It is a
// header because the generator calls it millions of times per chunk and every
// method is a handful of instructions.

#include "core/util/strict_math.hpp"
#include "core/util/types.hpp"

#include <cmath>

namespace mc {

class JavaRandom {
public:
    // Java's own multiplier, addend and modulus mask. Named rather than inlined
    // because they are quoted in the spec and someone will want to check them.
    static constexpr u64 kMultiplier = 0x5DEECE66DULL;
    static constexpr u64 kAddend = 0xBULL;
    static constexpr u64 kMask = (1ULL << 48) - 1;

    JavaRandom() = default;
    explicit JavaRandom(i64 seed) { setSeed(seed); }

    // `new Random(seed)`. The scramble is part of the constructor in Java, and
    // setSeed does the same thing, so both land here.
    void setSeed(i64 seed)
    {
        seed_ = (u64(seed) ^ kMultiplier) & kMask;
        haveNextGaussian_ = false;
    }

    // The raw generator. Every other method is a thin wrapper over this, which
    // is why getting it right is most of the battle.
    i32 next(int bits)
    {
        seed_ = (seed_ * kMultiplier + kAddend) & kMask;
        return i32(u32(seed_ >> (48 - bits)));
    }

    i32 nextInt() { return next(32); }

    // The one method with real logic in it.
    //
    // Java special-cases a power-of-two bound to take the high bits (the low
    // bits of an LCG are notoriously poor), and otherwise loops until the
    // modulus is unbiased. The loop condition in the JDK reads:
    //
    //     for (int u = r; u - (r = u % bound) + m < 0; u = next(31));
    //
    // which is only ever true when `u - r + m` overflows a signed int -- all
    // three terms are non-negative, so the mathematical value cannot be. Signed
    // overflow is undefined in C++ and UBSan would (correctly) stop the build,
    // so the same test is made in 64 bits, where it is exact and defined.
    // Identical results, no undefined behaviour.
    i32 nextInt(i32 bound)
    {
        // Java throws IllegalArgumentException here. This build has no
        // exceptions, and no caller in the generator passes a non-positive
        // bound, so the contract is "don't" rather than a silent fallback that
        // would desynchronise the stream if it ever fired.
        const i32 m = bound - 1;
        if ((bound & m) == 0) {  // power of two
            return i32((i64(bound) * i64(next(31))) >> 31);
        }

        i32 u = next(31);
        i32 r = u % bound;
        while (i64(u) - i64(r) + i64(m) > i64(0x7FFFFFFF)) {
            u = next(31);
            r = u % bound;
        }
        return r;
    }

    // `((long)next(32) << 32) + next(32)`.
    //
    // The addition is signed and the second term is a *signed* int, so a
    // negative low word borrows from the high word. That is the behaviour, not
    // a bug, and the reason this is not written as an or -- but it is also two
    // pieces of undefined behaviour if transcribed literally, and UBSan caught
    // both. Java shifts a sign-extended negative long left by 32; C++ leaves
    // that undefined. Java's addition wraps on overflow; C++ leaves that
    // undefined too.
    //
    // Both are done in u64 here, where the wrap is defined, and the bit pattern
    // is identical: sign-extending before a 32-bit left shift only feeds the
    // sign bits into a half that the shift discards anyway.
    i64 nextLong()
    {
        const u64 high = u64(u32(next(32))) << 32;
        const u64 low = u64(i64(next(32)));  // sign-extend, then reinterpret
        return static_cast<i64>(high + low);
    }

    bool nextBoolean() { return next(1) != 0; }

    float nextFloat() { return float(next(24)) / float(1 << 24); }

    double nextDouble()
    {
        const i64 high = i64(next(26)) << 27;
        return double(high + i64(next(27))) * kDoubleUnit;
    }

    // Marsaglia polar method, exactly as the JDK writes it, including the
    // cached second value -- a caller that takes one Gaussian and then any
    // other number gets a different stream than one that takes two Gaussians,
    // so the cache is part of the observable behaviour.
    //
    // The JDK calls StrictMath.sqrt and StrictMath.log, and the difference
    // between those two is worth knowing. sqrt is fine from anywhere: IEEE-754
    // specifies it to the last bit, so every libm agrees. log is not specified
    // that tightly -- glibc's is correctly rounded, fdlibm's is not, and Java
    // specifies fdlibm -- so this goes through mc::strictmath::log rather than
    // <cmath>. The first run against real JVM vectors disagreed by exactly one
    // ulp until it did; see strict_math.hpp.
    //
    // For the record, since it bounds how much this matters: **no a1.1.2
    // worldgen class calls nextGaussian.** Checked across the chunk provider,
    // both noise generators, caves, and all nine WorldGenerator subclasses.
    // This is here to be correct for whatever asks next, not because terrain
    // needs it.
    double nextGaussian()
    {
        if (haveNextGaussian_) {
            haveNextGaussian_ = false;
            return nextGaussian_;
        }
        double v1 = 0.0;
        double v2 = 0.0;
        double s = 0.0;
        do {
            v1 = 2.0 * nextDouble() - 1.0;
            v2 = 2.0 * nextDouble() - 1.0;
            s = v1 * v1 + v2 * v2;
        } while (s >= 1.0 || s == 0.0);
        const double multiplier = std::sqrt(-2.0 * strictmath::log(s) / s);
        nextGaussian_ = v2 * multiplier;
        haveNextGaussian_ = true;
        return v1 * multiplier;
    }

    // The scrambled internal state, for tests that want to pin the LCG itself
    // rather than what comes out of it.
    u64 rawSeed() const { return seed_; }

private:
    // 2^-53, spelled the way the JDK spells it.
    static constexpr double kDoubleUnit = 1.0 / double(1LL << 53);

    u64 seed_ = 0;
    double nextGaussian_ = 0.0;
    bool haveNextGaussian_ = false;
};

}  // namespace mc
