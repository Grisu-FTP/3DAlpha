#pragma once

// a1.1.2's `ej` -- WorldGenBigTree, the tall branching one the driver picks on
// a one-in-ten roll. Structurally unlike everything else in population: it
// plans the whole tree into a list of leaf nodes first, then draws trunk,
// branches and foliage from that plan.
//
// **It draws exactly one number from the shared stream**, a `nextLong`, and
// then seeds its own Random from it. Everything after that is private, so a
// big tree cannot desynchronise the chunk however wrong its internals are --
// which is the opposite of every other generator here and makes the failure
// mode "one misshapen tree" rather than "everything downstream moved".
//
// ---------------------------------------------------------------------------
// **The `Math.pow` risk this class was flagged for does not exist.** That was
// worth checking rather than working around.
//
// All seven `pow` calls have a literal exponent of `2.0`, and `pow(x, 2.0)` is
// bit-identical to `x * x`: measured over 608,011 values across the exact
// argument shapes this class uses -- integers, integers plus a half, widened
// floats -- against both `Math.pow` and `StrictMath.pow`, with zero
// mismatches. IEEE-754 multiplication is correctly rounded and every sane pow
// special-cases an exponent of 2, so this is a property rather than a
// coincidence. They are written as multiplications here.
//
// `sqrt` is IEEE-exact by specification everywhere, so the three calls are
// safe as well.
//
// **The one real transcendental is the `sin`/`cos` pair** that places a branch
// around the trunk, and the situation there is worse than it first looks:
// `Math.sin` and `StrictMath.sin` disagree with *each other* on 3.4% of the
// arguments this class generates, so the original is not self-consistent
// across JVMs either. glibc differs from this machine's `Math.sin` on 0.3% of
// them, by at most 1 ulp.
//
// That is tolerable here, and the margin is measured rather than assumed. The
// result is multiplied by a branch length under ~30 and then floored to a
// block, so a 1-ulp difference moves the coordinate by around 1e-15 blocks.
// Sampling where those coordinates actually land, the closest any came to an
// integer boundary was **6.8e-4** -- eleven orders of magnitude of margin. A
// branch would have to land within a femtoblock of a boundary to tip, and
// nothing in the stream depends on the answer, so a tip would misplace one
// branch by one block and nothing else.
//
// Recorded rather than fixed: transcribing fdlibm's `sin` would buy agreement
// with `StrictMath`, which is not what the original calls. See
// docs/worldgen-a1.1.2.md.

#include "core/util/types.hpp"

namespace mc {
class JavaRandom;
}

namespace mc::worldgen {

class PopulationView;

// **The one field of `ej` that survives from one tree to the next.**
//
// The driver constructs a single `ej` *before* its tree loop and calls it for
// every tree in the chunk, and `ej.a`'s first act is
//
//     if (this.e == 0) this.e = 5 + this.b.nextInt(this.m);
//
// so `e` -- the height limit -- is rolled for the **first** tree only and every
// later tree in the chunk reuses it. `validTreeLocation` can also shrink it
// when the site is short of headroom, and that shrunken value carries forward
// too, so one cramped tree makes the rest of the chunk's big trees short.
//
// It reads like a bug in the original and it is one; it is also the behaviour,
// so it is reproduced rather than tidied. Everything else `ej` holds is either
// rewritten by `setScale` on each call or recomputed from the plan.
//
// **This cost a wrong answer once.** An earlier version made a fresh generator
// per tree on the reasoning that the driver's `new ej()` is per chunk anyway --
// true, and the wrong conclusion, because per chunk is exactly what makes the
// state persist across the trees *inside* one. The second big tree of a chunk
// came out at height 3 against the jar's 8, and no per-tree fixture could see
// it: the state only exists between trees. See docs/worldgen-a1.1.2.md.
struct BigTreeState {
    // `ej.e`. Zero means "not rolled yet", which is the original's own guard.
    i32 heightLimit = 0;
};

// `ej.a(cn, Random, int, int, int)`. `y` is the height map value the driver
// passes in, as for the ordinary tree.
//
// `state` is the per-chunk generator instance; the driver owns one and hands
// the same one to every big tree in the chunk.
//
// `heightScale` is what `ik.a(DDD)` sets -- the driver always passes 1.0, so
// the default is the only value a1.1.2 ever uses, and it is a parameter only
// because leaving it out would hide that the original has one.
bool generateBigTree(PopulationView& view, JavaRandom& random, BigTreeState& state, i32 x, i32 y,
                     i32 z, double heightScale = 1.0);

}  // namespace mc::worldgen
