#pragma once

// a1.1.2's ordinary tree -- `oa` (WorldGenTrees). The big one, `ej`
// (WorldGenBigTree), is a separate class of a different order of size and is
// not here yet.
//
// The driver picks between them per chunk, not per tree: it builds one
// generator, swaps it for a big one on `nextInt(10) == 0`, and then plants
// however many trees the density noise asked for with whichever it chose. So a
// chunk gets all ordinary trees or all big ones.
//
// **How many trees is a noise question, not a random one.** `populate` samples
// the eight-octave generator at half the block scale and works out
//
//     count = (int)((noise(x/2, z/2) / 8.0 + nextDouble() * 4.0 + 4.0) / 3.0)
//
// clamped at zero, then adds one more on `nextInt(10) == 0`. That is why
// forests are patchy rather than evenly scattered -- see chunk_provider.hpp on
// `treeDensity_`, which is the generator this reads and which was written up
// for a long time as an unused mob-spawner noise.
//
// Derivation in docs/worldgen-a1.1.2.md.

#include "core/util/types.hpp"

namespace mc::worldgen {
class OctaveNoise;
}

namespace mc {
class JavaRandom;
}

namespace mc::worldgen {

class PopulationView;

// `oa.a(cn, Random, int, int, int)`. `y` is the height map value at (x, z),
// which the driver passes in -- so the tree is planted on the surface and this
// never searches for the ground itself.
//
// Returns whether anything was planted. **Both failure paths return before
// drawing anything beyond the initial height**, which matters: an unplantable
// spot costs exactly one `nextInt(3)` and no more.
bool generateTree(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z);

// How many trees `populate` plants in this chunk, and whether they are big
// ones. Split out from the generator because the count is drawn from the
// shared stream before any tree is placed, and a test can pin it on its own.
struct TreeBatch {
    int count = 0;
    bool big = false;
};

// The first thing `populate` does after the ores, in the driver's own order.
// `chunkX`/`chunkZ` are **block** coordinates -- the driver has already
// multiplied by 16 -- and `density` is the eight-octave generator.
//
// Three draws, always, in this order: a `nextDouble` inside the count, then a
// `nextInt(10)` that may add one, then a second `nextInt(10)` that decides
// between the ordinary generator and the big one. Both tens are rolled even
// when the count is zero, so a treeless chunk still costs three numbers.
TreeBatch treeBatchFor(const OctaveNoise& density, JavaRandom& random, i32 blockX,
                       i32 blockZ);

}  // namespace mc::worldgen
