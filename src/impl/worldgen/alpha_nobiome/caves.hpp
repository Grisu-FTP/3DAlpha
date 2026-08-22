#pragma once

// a1.1.2's cave carver -- `kk` (MapGenCaves) over `cy` (MapGenBase).
//
// Runs after the surface pass and before population, on the same 32,768-byte
// column. Derivation in docs/worldgen-a1.1.2.md.
//
// **Everything in the carver is float**, where the density noise above it is
// entirely double. That split is load-bearing rather than incidental: the
// tunnel's heading, its taper and its wobble are all single precision, and
// promoting any of them to double produces a cave system that looks completely
// plausible and is not the one the seed asks for. The only doubles are the
// position accumulators and the ellipsoid test.
//
// It also never calls a transcendental. Headings go through MathHelper's
// 65,536-entry table, so there is no libm in this path at all -- see
// math_helper.hpp for why that matters and how it is verified.
//
// The driver walks a **17x17 neighbourhood** of chunks around the one being
// generated, reseeding per cell, because a tunnel started up to eight chunks
// away can still reach into this column. That is why generating one chunk costs
// 289 cave rolls and not one.

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::worldgen {

class CaveGenerator {
public:
    CaveGenerator() = default;

    // `cy.a(nw, cn, int, int, byte[])`. `worldSeed` is the World's RandomSeed,
    // which is the only thing the original reads off the World here.
    void generate(i64 worldSeed, i32 chunkX, i32 chunkZ, u8* blocks);

private:
    // `kk.a(cn, int, int, int, int, byte[])`. `cellX`/`cellZ` is the chunk that
    // might *start* a tunnel; `originX`/`originZ` is the chunk being written.
    void recursiveGenerate(i32 cellX, i32 cellZ, i32 originX, i32 originZ, u8* blocks);

    // `kk.a(int, int, byte[], double, double, double)` -- a room, which is just
    // a node with a fat radius, no heading, and a flag that makes it carve once
    // instead of tunnelling.
    void carveRoom(i32 originX, i32 originZ, u8* blocks, double x, double y, double z);

    // `kk.a(int, int, byte[], double, double, double, float, float, float, int, int, double)`.
    //
    // **Recursion depth is at most two, and that is a property worth knowing on
    // a console with a 32 KB stack.** A node branches only while its width is
    // greater than 1.0, and a branch's width is drawn as
    // `nextFloat() * 0.5 + 0.5`, which is always under 1.0. So children never
    // branch again.
    void carveNode(i32 originX, i32 originZ, u8* blocks, double x, double y, double z, float width,
                   float yaw, float pitch, i32 step, i32 maxSteps, double heightScale);

    // How far, in chunks, a tunnel is allowed to start from the column it can
    // affect. `cy`'s own field, and it is 8.
    static constexpr i32 kRange = 8;

    JavaRandom random_;
};

}  // namespace mc::worldgen
