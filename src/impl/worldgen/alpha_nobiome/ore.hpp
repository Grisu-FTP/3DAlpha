#pragma once

// WorldGenMinable -- class `cu`. The generator behind every ore, and also
// behind the dirt and gravel patches, which are the same code with a different
// block and a bigger vein.
//
// A vein is a line segment between two points, with a sphere swept along it
// whose radius rises and falls as a half sine. Only stone is replaced, so a
// vein that crosses a cave or an existing vein comes out pitted rather than
// truncated.
//
// The seven passes a1.1.2 runs, recovered from the populate driver:
//
//   | ore      | tries | y below | vein |
//   |----------|-------|---------|------|
//   | dirt     |    20 |     128 |   32 |
//   | gravel   |    10 |     128 |   32 |
//   | coal     |    20 |     128 |   16 |
//   | iron     |    20 |      64 |    8 |
//   | gold     |     2 |      32 |    8 |
//   | redstone |     8 |      16 |    7 |
//   | diamond  |     1 |      16 |    7 |

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

namespace mc::worldgen {

// `cu.a(cn, Random, int, int, int)`. Always returns true in the original; the
// bool is there for the WorldGenerator interface and every caller discards it.
bool generateOreVein(PopulationView& view, JavaRandom& random, u8 blockId, i32 veinSize, i32 x,
                     i32 y, i32 z);

// WorldGenClay -- class `gv`. **The same vein, with two changes**, which is
// true of the original too: `gv` is a copy of `cu` with a guard bolted on.
//
//   * It refuses unless the block it is handed is *water* by material, so clay
//     only forms under lakes and rivers.
//   * It replaces **sand**, not stone, with clay.
//
// The patch size a1.1.2 uses is 32, ten tries per chunk. Returns false when the
// guard rejects, which is the one place a generator's return value differs from
// "always true".
bool generateClayPatch(PopulationView& view, JavaRandom& random, i32 patchSize, i32 x, i32 y,
                       i32 z);

}  // namespace mc::worldgen
