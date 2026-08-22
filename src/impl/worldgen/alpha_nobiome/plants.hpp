#pragma once

// a1.1.2's reeds (`es`, WorldGenReed) and cactus (`da`, WorldGenCactus).
//
// Both run late in population -- reeds ten times a chunk, cactus once -- and
// both are **stream-sensitive in a way the liquids are not**: how many random
// numbers they consume depends on what they find in the world. A try that lands
// on air draws two more numbers for a height; a try that lands on stone does
// not. So a transcription can place every plant in the right spot and still be
// wrong, by drawing a different number of times and shifting everything that
// runs after it.
//
// That is why tests/plant_vectors.hpp carries a **stream fingerprint** per case
// -- one extra `nextLong` taken after the generator returns -- alongside the
// placements. A wrong draw count fails that comparison even when every block
// landed correctly.
//
// **Neither needs the lighting engine**, which is why they are transcribed now
// and the flowers are not: `ae` (WorldGenFlowers, which also plants both
// mushrooms) tests `canBlockStay`, and for flowers and mushrooms that reads the
// block light value. Reeds and cactus have their own `canBlockStay` overrides,
// and both use only block ids and materials. See docs/worldgen-a1.1.2.md.
//
// Derivation, and the ids -- reed 83, cactus 81, sand 12, grass 2, dirt 3 --
// all read off the jar.

#include "core/util/types.hpp"

namespace mc {
class JavaRandom;
}

namespace mc::worldgen {

class PopulationView;

// `es.a(cn, Random, int, int, int)`. Twenty tries, each perturbing x and z by
// `nextInt(4) - nextInt(4)`; **y is used as given and never perturbed**, unlike
// cactus.
bool generateReeds(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z);

// `da.a(cn, Random, int, int, int)`. Ten tries, perturbing all three axes.
bool generateCactus(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z);

// The two `canBlockStay` overrides, exposed because they are the interesting
// half and a test that can ask them directly localises a failure to the
// predicate rather than to the loop around it.
//
// `jm.a(cn, int, int, int)` -- reed. Grass or dirt below with water beside it,
// or another reed below.
bool reedCanStay(const PopulationView& view, i32 x, i32 y, i32 z);

// `hy.g(cn, int, int, int)` -- cactus. No solid neighbour on any of the four
// sides, and sand or another cactus below.
bool cactusCanStay(const PopulationView& view, i32 x, i32 y, i32 z);

}  // namespace mc::worldgen
