#pragma once

// a1.1.2's `ae` (WorldGenFlowers), which plants **four different things**:
// dandelions (37), roses (38), brown mushrooms (39) and red mushrooms (40).
// One generator class, handed a block id by the driver.
//
// The interesting part is that the two mushrooms are not flowers with a
// different texture. `ae` asks the block whether it may stay, and the id it is
// given decides which class answers:
//
//   * `mq` (BlockFlower) wants **light**: at least 8, or a clear view of the
//     sky, and grass, dirt or farmland underneath.
//   * `ky` (BlockMushroom) extends `mq` but overrides both halves. It wants
//     **darkness** -- 13 or less -- and will sit on any opaque cube.
//
// So the same loop plants sun-loving flowers and shade-loving mushrooms
// depending on one argument, and getting the two predicates the right way
// round is the whole job.
//
// **The light they read is not the light the world ends up with.** It is the
// per-column fill that exists during generation, before the original's box
// queue has run at all -- see the long note in population_view.hpp. That is
// what lets a flower grow in the shade under a tree: the canopy drops the
// height map, and the decay beneath it is 14, 13, 12 rather than 0.
//
// Derivation in docs/worldgen-a1.1.2.md.

#include "core/util/types.hpp"

namespace mc {
class JavaRandom;
}

namespace mc::worldgen {

class PopulationView;

// `ae.a(cn, Random, int, int, int)`. Sixty-four tries, each perturbing x and z
// by `nextInt(8) - nextInt(8)` and y by `nextInt(4) - nextInt(4)`.
//
// Always returns true, like the original -- the driver ignores it.
bool generatePlants(PopulationView& view, JavaRandom& random, u8 plantId, i32 x, i32 y, i32 z);

// `mq.g` -- flowers and roses. Exposed so a test can drive the predicate
// directly and localise a failure to it rather than to the loop.
bool flowerCanStay(const PopulationView& view, i32 x, i32 y, i32 z);

// `ky.g` -- both mushrooms.
bool mushroomCanStay(const PopulationView& view, i32 x, i32 y, i32 z);

}  // namespace mc::worldgen
