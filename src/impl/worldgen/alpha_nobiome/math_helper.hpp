#pragma once

// a1.1.2's MathHelper -- class `eo` -- for the parts world generation uses.
//
// **The cave carver never calls a transcendental.** It goes through a
// 65,536-entry float table that the original builds once from
// `(float)Math.sin(i * PI * 2 / 65536)`, and looks up with a multiply, a
// truncation and a mask. So the whole `Math.sin` portability problem collapses
// into one question -- does our table come out bit-identical to Java's? -- and
// once that is answered the runtime path is pure float indexing with no libm in
// it at all.
//
// The table is built at startup rather than compiled in, and **it is on the
// heap rather than in .bss**. 65,536 floats is 256 KB either way, but .bss is
// part of the image the 3DS loader has to allocate and zero before the process
// exists, whereas the heap is asked for after it is running -- and this table
// is built at runtime regardless, so the image was paying for nothing. It was
// the single largest thing in the binary: .bss was 319 KB with it and 63 KB
// without. tests/sin_table_test.cpp hashes our table against a hash taken from
// a real JVM, so "built at startup" does not mean "unverified".
//
// Both lookups are deliberately lossy in the same way the original is: the
// argument is scaled by 10430.378 and truncated, so the table has a resolution
// of about 0.0006 radians and the result is a *quantised* sine. Calling
// std::sin here instead would be more accurate and would carve different caves.

#include "core/util/java_cast.hpp"
#include "core/util/types.hpp"

namespace mc::worldgen {

class MathHelper {
public:
    // 65536 / (2*pi), the constant the original scales by. Not derived at
    // runtime, because it is a float literal in the class file and the
    // rounding of the literal is part of the result.
    static constexpr float kRadiansToIndex = 10430.378f;

    // Builds the table. Idempotent.
    //
    // **Called from sin and cos rather than left to the caller**, and that is
    // deliberate: leaving it to the caller cost an afternoon once already. The
    // ore generator did not call it, the table was all zeros, and instead of
    // failing loudly it quietly returned sin = 0 everywhere -- which produces
    // veins of eight blocks where the original makes a hundred and eight. A
    // zeroed lookup table is a silent wrong answer, not a crash, and the next
    // generator added would have hit it too.
    //
    // The cost is one predictable branch against a `bool` per lookup, against
    // four lookups per cave step. Measured against the risk, that is free.
    //
    // **Not thread-safe, and it does not need to be**: `ChunkProvider`'s
    // constructor calls it, and that runs on whatever thread builds the
    // generator -- before the chunk worker exists. Every later call is a read
    // of a flag that was already true. Leaving it to be built lazily by the
    // worker would be a write racing the main thread's reads the first time
    // anything else wanted a sine.
    static void ensureBuilt();

    // `eo.a(F)F`. Note the truncation is toward zero, not a floor -- for
    // negative arguments those differ, and the mask afterwards is what makes it
    // work out.
    //
    // The cast is Java's saturating one. An angle big enough to overflow int
    // needs about 206,000 radians and no cave ever accumulates that, so this
    // costs two never-taken branches per lookup rather than buying a bug fix
    // -- but a table index is exactly the place where undefined behaviour
    // turns into a read outside the array, so it is not left to chance.
    static float sin(float value)
    {
        ensureBuilt();
        return table_[u32(javaToInt(value * kRadiansToIndex)) & 65535u];
    }

    // `eo.b(F)F`. Cosine is the same table a quarter turn along, and 16384 is
    // exactly 65536/4 -- added *before* the truncation, so it is not the same
    // as shifting the index afterwards.
    static float cos(float value)
    {
        ensureBuilt();
        return table_[u32(javaToInt(value * kRadiansToIndex + 16384.0f)) & 65535u];
    }

    // `eo.b(D)I`, floor_double. A truncation plus a conditional decrement, not
    // std::floor -- the same shape as the one in noise.hpp, and kept separate
    // because they are separate methods in the original. Both go through
    // core/util/java_cast.hpp, which is what makes them agree with Java once
    // the argument leaves int's range; the carver never gets there on its own,
    // but it shares a world with a noise generator that does.
    static i32 floorDouble(double value) { return javaFloorToInt(value); }

    // For the test that hashes the table against the JVM's.
    static const float* table();

private:
    static float* table_;
    static bool built_;
};

}  // namespace mc::worldgen
