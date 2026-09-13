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

// **The negative-quadrant ore bug, and the switch that turns it off.**
//
// The six bounds below are `(int)` casts in the original, and a Java `(int)`
// truncates toward zero rather than flooring. At positive coordinates the two
// agree; at negative ones truncation rounds *up*, so the whole box is shifted
// one block towards zero -- the slice at its low end is never visited and is
// never written, while the extra slice at its high end is outside the
// ellipsoid and writes nothing. The vein comes out short, and it comes out
// short in x and in z independently, so the quadrant at negative x and
// negative z loses the most.
//
// It is a bug and not a rule: nothing else in the generator is asymmetric
// about the origin, and the same seed generates different amounts of ore
// depending only on which way the player walked. `FloorBounds` is what the
// Extra Settings screen turns on; `TruncateBounds` is a1.1.2 and the default
// everywhere, fixtures included.
enum class OreBounds {
    TruncateBounds,  // `(int)`, and the bug with it
    FloorBounds,     // floor, and the same vein in every quadrant
};

// `cu.a(cn, Random, int, int, int)`. Always returns true in the original; the
// bool is there for the WorldGenerator interface and every caller discards it.
//
// **`bounds` changes no random draw.** It is read after the stream has been
// drawn from and only decides which blocks the already-chosen vein is written
// into, so a world with the fix on and a world with it off are in step with
// each other through every later pass.
bool generateOreVein(PopulationView& view, JavaRandom& random, u8 blockId, i32 veinSize, i32 x,
                     i32 y, i32 z, OreBounds bounds = OreBounds::TruncateBounds);

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
                       i32 z, OreBounds bounds = OreBounds::TruncateBounds);

}  // namespace mc::worldgen
