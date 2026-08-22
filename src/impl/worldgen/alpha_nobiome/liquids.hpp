#pragma once

// a1.1.2's liquid springs -- `nn` (WorldGenLiquids), the source blocks that
// appear in cave walls and hillsides.
//
// Population runs this **seventy times per chunk**: fifty for water and twenty
// for lava, at the very end, after the ores and the plants. Almost all of those
// tries do nothing, which is the point -- the predicate is narrow enough that a
// spring only lands where a wall has exactly one opening.
//
// Two properties make this the easiest generator in the whole of population to
// verify, and both were checked against the jar rather than assumed:
//
//   * **It draws no random numbers.** `nn.a` takes a Random and never touches
//     it. The three `nextInt` calls per try belong to the *driver*, which picks
//     the coordinate. So a bug in here cannot desynchronise the stream, which
//     is the failure mode every other population generator has.
//
//   * **It reads exactly seven blocks** -- above, below, centre, and the four
//     horizontal neighbours -- and nothing else. Its behaviour is therefore a
//     pure function of seven block ids, small enough that
//     tests/liquid_vectors.hpp is the *complete* truth table over
//     {air, stone, other} rather than a set of sampled cases.
//
// Derivation in docs/worldgen-a1.1.2.md.

#include "core/util/types.hpp"

namespace mc::worldgen {

class PopulationView;

// `nn.a(cn, Random, int, int, int)`. Returns what the original returns, which
// is **true regardless of whether anything was placed** -- the driver ignores
// it, and it is kept only so the shape matches.
//
// `liquidId` is the flowing form: 8 for water and 10 for lava, read off
// `ly.B` and `ly.D` in the driver's bytecode. Not the still forms.
bool generateLiquidSpring(PopulationView& view, u8 liquidId, i32 x, i32 y, i32 z);

}  // namespace mc::worldgen
