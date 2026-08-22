#pragma once

// Java's narrowing conversion from floating point to `int`, which is **not**
// C++'s and is load-bearing at the edge of the world.
//
// JLS 5.1.3 specifies `(int)someDouble` completely: NaN becomes 0, anything
// too large becomes Integer.MAX_VALUE, anything too small becomes
// Integer.MIN_VALUE, and everything else truncates toward zero. C++ specifies
// only the last of those -- a conversion whose truncated value will not fit in
// the destination is **undefined behaviour**, and in practice x86-64's
// `cvttsd2si` answers INT_MIN for an overflow in either direction while ARM's
// `vcvt` saturates the way Java does. So the same C++ source produces two
// different worlds on the host and on the console, and neither is reliably the
// one Java produces.
//
// **This is reachable, not theoretical: it is what the Far Lands are.** a1.1.2's
// Perlin lattice evaluates `(latticeX * 684.412 + offset)` and floors it with a
// `d2i`, and at chunk 784,426 -- block x = 12,550,824 -- that product reaches
// 2,147,483,647. Past there Java clamps, the fractional part the sampler
// subtracts stops being a fraction, and the terrain becomes the famous wall.
// Reproducing that faithfully means reproducing the clamp, so the clamp lives
// here rather than being left to whatever the target's FPU happens to do.
// docs/worldgen-a1.1.2.md has the derivation and the arithmetic.
//
// Worth knowing why this was invisible for so long: **GCC's `-fsanitize=undefined`
// does not include `-fsanitize=float-cast-overflow`.** It is one of two checks
// left out of the group (the other is float-divide-by-zero), so an out-of-range
// conversion runs silently under a build that looks fully sanitised. The test
// build now names it explicitly -- see CMakeLists.txt.

#include "core/util/types.hpp"

namespace mc {

// `d2i`. The bounds are written as the exact doubles either side of int's
// range rather than as `double(INT_MAX)`: 2147483647 is not representable as a
// double, so `value >= double(INT_MAX)` would round the comparand up to
// 2147483648 and clamp a value that Java would have converted exactly.
//
// The low end is `-2147483649.0` and the comparison is `<=`, because
// truncation happens *before* the range check -- Java converts -2147483648.5
// to Integer.MIN_VALUE by truncating to -2147483648, which fits.
inline i32 javaToInt(double value)
{
    // NaN first, and written as a self-comparison so it holds without
    // -ffast-math ever being on (it is not, and -ffp-contract=off is).
    if (value != value) {
        return 0;
    }
    if (value >= 2147483648.0) {
        return 2147483647;
    }
    if (value <= -2147483649.0) {
        return -2147483647 - 1;
    }
    return i32(value);
}

// `f2i`. Same rule, but **the bounds are not the same numbers**, and copying
// the double version here would be subtly wrong. 2^31 is exactly representable
// as a float and 2^31 + 1 is not -- with 24 bits of mantissa, `-2147483649.0f`
// silently rounds to `-2147483648.0f` and the guard stops saying what it
// looks like it says.
//
// It happens not to matter, and it is worth knowing why rather than relying on
// it: there is no float strictly between -2^31 and the next one down
// (-2147483904), so the only float the two spellings disagree about is -2^31
// itself, which converts to Integer.MIN_VALUE either way. Written as `<=` on
// -2^31 so the bound is a number a float can actually hold.
inline i32 javaToInt(float value)
{
    if (value != value) {
        return 0;
    }
    if (value >= 2147483648.0f) {
        return 2147483647;
    }
    if (value <= -2147483648.0f) {
        return -2147483647 - 1;
    }
    return i32(value);
}

// The floor a1.1.2 writes by hand, over and over, as a cast plus a conditional
// decrement. Not Math.floor: it differs for negatives in exactly the way that
// matters, and the original never calls Math.floor here.
//
// **The decrement is done in u32 because it can underflow.** At the negative
// Far Lands the cast clamps to Integer.MIN_VALUE, the value really is smaller
// than that, and Java's `i - 1` wraps around to Integer.MAX_VALUE. C++ signed
// overflow is undefined, so the wrap is spelled out. The result is then masked
// with 255 by every caller, so the wrapped value is not merely defined but
// used.
inline i32 javaFloorToInt(double value)
{
    const i32 truncated = javaToInt(value);
    if (value < double(truncated)) {
        return i32(u32(truncated) - 1u);
    }
    return truncated;
}

}  // namespace mc
