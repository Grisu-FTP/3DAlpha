#pragma once

// a1.1.2's two noise generators, transcribed from the client jar.
//
// `PerlinNoise` is class `v` and `OctaveNoise` is class `lp`; both derive from
// the abstract `bk`, which carries nothing. The full derivation, including how
// the classes were identified and where every constant came from, is in
// docs/worldgen-a1.1.2.md. This file is the transcription, and the numbers in
// it are load-bearing rather than tunable.
//
// **This is Ken Perlin's improved noise, and it is not quite the textbook.**
// The gradient function and the 6t^5-15t^4+10t^3 fade are the standard ones,
// but two things here are Minecraft's own and are the reason this is
// transcribed rather than reached for from a library:
//
//   * **Octave amplitude goes up, not down.** The octave loop halves a factor
//     each pass, multiplies the *scales* by it -- so each successive octave is
//     lower frequency -- and divides the *contribution* by it, so each
//     successive octave contributes twice as much. A conventional fBm does the
//     opposite. Sixteen octaves of this is what a1.1.2's terrain is made of.
//
//   * **The lattice fill caches its Y slab**, and the cache is observable. It
//     recomputes the four X-interpolated corner values only when the integer Y
//     cell changes (or on the first Y step), while the gradients it feeds them
//     depend on the *fractional* Y as well. So when two Y samples land in the
//     same integer cell -- which happens whenever the Y scale is under 1 -- the
//     second one silently reuses the first one's gradients. That is not a
//     transcription slip: it is what the original does, and it is pinned by a
//     test case built to land two samples in one cell. a1.1.2's own terrain
//     never triggers it (its Y scale is about 4.28), but a faithful generator
//     has to behave the same way for the cases that do.
//
// Everything is double, deliberately and throughout. Caves are float in the
// original and that split matters -- see docs/worldgen-a1.1.2.md -- but nothing
// in this file is.

#include "core/util/java_cast.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::worldgen {

// Which two of a cell's six corner offsets -- fx, fx - 1, fy, fy - 1, fz,
// fz - 1 -- gradient() takes as u (low nibble) and v (high nibble), for each
// hash and each corner. Built from gradient's own conditions; see
// PerlinNoise::cornerGradient.
struct PerlinCornerPicks {
    u8 pick[16][8];
};

constexpr PerlinCornerPicks buildPerlinCornerPicks()
{
    PerlinCornerPicks table{};
    for (int h = 0; h < 16; ++h) {
        for (int corner = 0; corner < 8; ++corner) {
            const int x = corner & 1;
            const int y = 2 + ((corner >> 1) & 1);
            const int z = 4 + ((corner >> 2) & 1);
            const int u = h < 8 ? x : y;
            const int v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
            table.pick[h][corner] = u8(u | (v << 4));
        }
    }
    return table;
}

inline constexpr PerlinCornerPicks kPerlinCornerPick = buildPerlinCornerPicks();

// Class `v`. One Perlin lattice with a 512-entry permutation and three random
// offsets, all drawn from the shared Random in a fixed order that is part of
// the seed.
class PerlinNoise {
public:
    PerlinNoise() = default;
    explicit PerlinNoise(JavaRandom& random);

    // The lattice fill, `v.a([DDDDIIIDDDD)V`. Accumulates into `out` rather
    // than overwriting it -- the octave loop relies on that, and zeroing is the
    // caller's job.
    //
    // The iteration order is X outermost, then Z, then Y innermost, and the
    // output index simply counts up, so `out` is Y-fastest. That is not the
    // same order as the arguments are named in, and getting it wrong produces a
    // world that is plausible and wrong.
    void populate(double* out, double x, double y, double z, int xSize, int ySize, int zSize,
                  double xScale, double yScale, double zScale, double amplitude) const;

    // `v.a(DDD)D`. **A single sample, not a slice of the lattice fill**, and it
    // is a separate method in the original for a reason: it has no Y-slab
    // cache, so it cannot reuse a neighbour's gradients the way populate can.
    // The two agree wherever the cache does not fire, which is everywhere
    // a1.1.2's terrain asks, but they are different code and are kept so.
    double sample(double x, double y, double z) const;

    // The three offsets, exposed because the tests pin them separately from the
    // permutation: a failure that names one and not the other says whether the
    // constructor or the evaluator is wrong.
    double xOffset() const { return xOffset_; }
    double yOffset() const { return yOffset_; }
    double zOffset() const { return zOffset_; }

    const i32* permutation() const { return permutation_; }

    // `v.a(IDDD)D`. Perlin's own gradient selector: the low four bits of the
    // hash pick one of twelve edge vectors, expressed as sign choices over two
    // of the three components. Public so noise_test can hold cornerGradient to
    // it.
    static double gradient(i32 hash, double x, double y, double z)
    {
        const i32 h = hash & 15;
        const double u = h < 8 ? x : y;
        const double v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }

    // **`gradient`, for the lattice fill, with its choices looked up.** The
    // eight corners of a cell only ever pass fx or fx - 1, fy or fy - 1, and fz
    // or fz - 1, so populate keeps those six in `corners`, in that order, and
    // this picks u and v out of them by table instead of by comparison.
    // `corner` is dx | dy << 1 | dz << 2.
    //
    // **Bit-identical to gradient, not an approximation of it**: the same
    // component of the same value comes out, and negation is a sign flip. The
    // table is built from gradient's own conditions, and noise_test checks the
    // two against each other for every hash and corner.
    //
    // Why: the hash is random, so gradient's comparisons are unpredictable
    // branches, and eight a cell made them the largest single cost in terrain
    // generation on the host. It also keeps the six values in one place for
    // the ARM11, which otherwise stores the three it was passed on every call.
    static double cornerGradient(i32 hash, const double* corners, int corner)
    {
        const u8 pick = kPerlinCornerPick.pick[hash & 15][corner];
        const double u = corners[pick & 15];
        const double v = corners[pick >> 4];
        return ((hash & 1) == 0 ? u : -u) + ((hash & 2) == 0 ? v : -v);
    }

private:

    // `v.b(DDD)D`. Note the argument order: the interpolant comes first.
    static double lerp(double t, double a, double b) { return a + t * (b - a); }

    // 6t^5 - 15t^4 + 10t^3, in the factored form the original evaluates. The
    // factoring is kept because floating-point addition is not associative and
    // a rearrangement would change the last bits.
    static double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

    // Java's (int) cast truncates toward zero; this is a floor. The original
    // writes it as a cast followed by a conditional decrement rather than
    // calling Math.floor, and the two differ for negative values, so the shape
    // is preserved.
    //
    // **The cast is Java's, not C++'s, and that is what makes the Far Lands
    // work.** Past block x = 12,550,824 the argument here exceeds 2^31 and the
    // conversion stops being a conversion; Java clamps, C++ calls it undefined
    // and x86 answers INT_MIN where ARM saturates. See core/util/java_cast.hpp
    // -- writing `i32(value)` here generates a different world at the edge of
    // the map on every target, and generated *water* where a1.1.2 generates
    // the wall.
    static i32 floorToInt(double value) { return javaFloorToInt(value); }

    i32 permutation_[512] = {};
    double xOffset_ = 0.0;
    double yOffset_ = 0.0;
    double zOffset_ = 0.0;
};

// Class `lp`. A stack of Perlin lattices, constructed from one Random so the
// draw order across the whole stack is fixed.
class OctaveNoise {
public:
    OctaveNoise() = default;
    OctaveNoise(JavaRandom& random, int octaves);

    int octaves() const { return octaves_; }

    // `lp.a([DDDDIIIDDD)[D`. Zeroes `out` and then sums the octaves into it.
    // The caller owns the buffer; the original would allocate one when handed
    // null, which no caller in the generator does.
    void populate(double* out, int count, double x, double y, double z, int xSize, int ySize,
                  int zSize, double xScale, double yScale, double zScale) const;

    // `lp.a(DD)D`. The two-argument sample, used by population to decide how
    // many trees a chunk gets.
    //
    // **The second argument is Y, not Z.** `v.a(DD)D` forwards to
    // `a(first, second, 0.0)`, so the caller's world z ends up on the noise's
    // y axis with z pinned at zero. Passing it as z instead gives a perfectly
    // plausible tree distribution that is not this seed's.
    //
    // The octave weighting is also the mirror of populate's: here the sum is
    // divided by the amplitude and the *inputs* are multiplied by it, so early
    // octaves are high frequency and low amplitude, exactly as populate has
    // it -- but written out per sample rather than accumulated into a buffer.
    double sample2D(double x, double y) const;

private:
    // Sixteen is the largest a1.1.2 asks for, and the generator builds eight of
    // these at construction. Fixed-size rather than heap so that a generator
    // living on a worker thread does no allocation per chunk; see
    // docs/architecture.md on the chunk worker.
    //
    // **Never put one of these on a stack.** A PerlinNoise is 2,072 bytes and an
    // OctaveNoise is 33,160 -- four times the 3DS build's 8 KB
    // -Werror=stack-usage ceiling, and the whole of a 3DSX's 32 KB main-thread
    // stack. The build will refuse it, which is the point of that flag; this
    // note is here because the project has already lost a hardware launch to a
    // 36 KB local (see docs/status.md on the first launch). The eight generators
    // the chunk provider holds come to roughly 265 KB and belong on the heap,
    // once, for the lifetime of the world.
    //
    // Sizing every instance for sixteen octaves wastes about 95 KB across those
    // eight, since the real counts are 16, 16, 8, 4, 4, 10, 16 and 8. Measured
    // and accepted: it buys a fixed layout with no indirection, against a 40 MB
    // heap.
    static constexpr int kMaxOctaves = 16;

    PerlinNoise generators_[kMaxOctaves];
    int octaves_ = 0;
};

}  // namespace mc::worldgen
