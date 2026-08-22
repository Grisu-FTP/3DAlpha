#pragma once

// `nw.a(aw, int, int)` -- the population driver, which runs every generator in
// `src/impl/worldgen/alpha_nobiome/` in one fixed order over one chunk.
//
// **This is the piece that no per-generator test can check.** Each generator is
// already verified block-for-block against the jar in isolation, but they all
// draw from one shared `java.util.Random`, so the *order* they run in and the
// number of draws each makes are as much a part of the seed as their contents.
// Get one pass out of place, or let one generator draw once too often, and
// every generator after it in the chunk moves -- while each of them, tested on
// its own, still passes.
//
// So the whole-chunk comparison is the last check M4 needs: drive all nine
// passes in the driver's own order over a real chunk and compare all 32,768
// bytes against a real a1.1.2 World.
//
// **The population seed is not the chunk seed.** Terrain uses
// `cx * 341873128712 + cz * 132897987541` with no XOR; population uses two odd
// multipliers drawn from the world seed and *then* XORs the world seed back in:
//
//     random.setSeed(worldSeed)
//     a = (nextLong() / 2) * 2 + 1
//     b = (nextLong() / 2) * 2 + 1
//     random.setSeed(cx * a + cz * b ^ worldSeed)
//
// The `/ 2 * 2 + 1` is truncating division, not a shift -- the same trap the
// cave carver has, and wrong in the same way for negative seeds.
//
// **Everything is offset by +8 in x and z except the ores.** The driver passes
// `chunkX * 16` to the ore passes and `chunkX * 16 + 8` to everything else,
// which is what makes trees and plants straddle chunk corners rather than
// clustering at their origins.

#include "core/util/types.hpp"

#include <vector>

namespace mc {
class JavaRandom;
}

namespace mc::worldgen {

class ChunkProvider;
class PopulationView;
struct DungeonChest;
struct DungeonSpawner;

// What population produced that is not a block. Dungeons are the only source.
struct PopulationSideEffects {
    std::vector<DungeonSpawner> spawners;
    std::vector<DungeonChest> chests;

    PopulationSideEffects();
    ~PopulationSideEffects();
    PopulationSideEffects(PopulationSideEffects&&) noexcept;
    PopulationSideEffects& operator=(PopulationSideEffects&&) noexcept;
};

// Runs every population pass over the chunk at the centre of `view`.
//
// `provider` supplies the tree-density noise and the world seed; `view` must be
// a 3x3 (or wider) window whose centre is `(chunkX, chunkZ)`, with every column
// already carrying terrain, surface and caves.
//
// `sideEffects` may be null when only the blocks matter.
void populateChunk(ChunkProvider& provider, PopulationView& view, i32 chunkX, i32 chunkZ,
                   PopulationSideEffects* sideEffects);

}  // namespace mc::worldgen
