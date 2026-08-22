#pragma once

// a1.1.2's ChunkProviderGenerate -- terrain shape and the surface pass.
//
// Class `nw` in the client jar. The derivation, the class map and the seed
// arithmetic are in docs/worldgen-a1.1.2.md; this is the transcription.
//
// Two stages, in the order `provideChunk` runs them and with the same shared
// Random:
//
//   1. **generateTerrain** carves stone, water and (optionally) ice out of a
//      5x17x5 noise lattice interpolated up to 16x128x16. It consumes no random
//      numbers at all.
//   2. **replaceBlocksForBiome** walks each column from the top down and turns
//      the bare stone into grass/dirt, sand or gravel, and lays bedrock. It
//      consumes three doubles per column plus **one nextInt(6) for every one of
//      the 128 Y steps**, which is the part that has to be reproduced exactly or
//      everything downstream of it draws different numbers.
//
// Caves and population are separate passes and are not here yet.
//
// The block array is Alpha's own layout, `x << 11 | z << 7 | y` -- Y-fastest,
// then Z, then X -- which is the order the chunk file stores and the order
// core/world/Section already indexes in, so a generated column slices into
// sections by memcpy.

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"
#include "impl/worldgen/alpha_nobiome/caves.hpp"
#include "impl/worldgen/alpha_nobiome/noise.hpp"

namespace mc::worldgen {

// 16 x 128 x 16.
inline constexpr int kChunkBlocks = 32768;

// The one thing generateTerrain reads from the World: level.dat's
// `SnowCovered`, which swaps the top layer of every sea for ice. a1.1.2 sets it
// on a fresh world with a one-in-four chance, so it is a property of the world
// and not a debug switch.
struct GeneratorOptions {
    bool snowCovered = false;
};

// **Roughly 300 KB. Heap-allocate it; never make one a local.** Eight octave
// stacks plus the scratch fields come to far more than a 3DSX's whole 32 KB
// main-thread stack, and the 3DS build's -Werror=stack-usage will refuse a
// function that tries. One instance per world, owned by the chunk worker.
class ChunkProvider {
public:
    ChunkProvider(i64 seed, GeneratorOptions options);

    // `nw.b(int,int)` up to but not including caves. Fills `blocks`, which must
    // have room for kChunkBlocks.
    //
    // Seeds the shared Random from the chunk coordinates first, exactly as
    // provideChunk does -- and note there is **no XOR with the world seed**
    // here, unlike the population pass.
    void generateColumn(i32 chunkX, i32 chunkZ, u8* blocks);

    // The stages on their own, so tests can pin them separately. None of them
    // seeds the shared Random; generateColumn does that.
    void generateTerrain(i32 chunkX, i32 chunkZ, u8* blocks);
    void replaceBlocksForBiome(i32 chunkX, i32 chunkZ, u8* blocks);
    void carveCaves(i32 chunkX, i32 chunkZ, u8* blocks);

    JavaRandom& random() { return random_; }

    // The world seed and the generator options, both of which population reads:
    // the seed to derive its own per-chunk stream, the options for the snow
    // sweep at the end.
    i64 seed() const { return seed_; }
    const GeneratorOptions& options() const { return options_; }

    // The eighth octave generator, which population samples to decide how many
    // trees a chunk gets. Exposed rather than folded into a `populate` method
    // because the count is drawn before any tree exists and is worth pinning on
    // its own -- see trees.hpp.
    const OctaveNoise& treeDensity() const { return treeDensity_; }

private:
    // `nw.a(double[],int,int,int,int,int,int)`. Fills `noiseField_` with the
    // 5x17x5 density lattice for this chunk.
    void initializeNoiseField(i32 x, i32 y, i32 z);

    static constexpr int kCells = 4;         // 4 x 4 noise cells across a chunk
    static constexpr int kSeaLevel = 64;
    static constexpr int kLatticeX = kCells + 1;  // 5
    static constexpr int kLatticeY = 17;
    static constexpr int kLatticeZ = kCells + 1;  // 5
    static constexpr int kLatticeSize = kLatticeX * kLatticeY * kLatticeZ;

    // **Declaration order below is the seed.**
    //
    // The eight generators are constructed from one Random, and each one drains
    // it further, so the order they are built in decides every number the rest
    // of them ever produce. They are built in the constructor's initialiser
    // list, which runs in *declaration* order regardless of how the list is
    // written -- so reordering these lines silently changes the world while
    // still compiling. `random_` is declared first for the same reason: it has
    // to exist before the generators draw from it.
    //
    // Two things guard this. -Wreorder complains if the initialiser list stops
    // matching, and tests/terrain_test.cpp compares whole chunks against the
    // jar, which no reordering survives.
    JavaRandom random_;
    GeneratorOptions options_;

    // Carries its own Random, seeded from the world seed rather than from the
    // per-chunk one, so it is deliberately not part of the sequence above.
    CaveGenerator caves_;
    i64 seed_ = 0;

    OctaveNoise minLimit_;      // 16 octaves
    OctaveNoise maxLimit_;      // 16
    OctaveNoise main_;          // 8
    OctaveNoise sandGravel_;    // 4  -- used twice, see the .cpp
    OctaveNoise surfaceDepth_;  // 4
    OctaveNoise scale_;         // 10
    OctaveNoise depth_;         // 16
    OctaveNoise treeDensity_;   // 8  -- how many trees a chunk gets; see below

    // Scratch, reused across chunks. Members rather than locals because
    // together they are far past the 3DS build's 8 KB stack ceiling, and
    // because the original keeps them as fields too -- which matters, since a
    // buffer that is *not* cleared between uses is observable.
    double noiseField_[kLatticeSize] = {};
    double minLimitField_[kLatticeSize] = {};
    double maxLimitField_[kLatticeSize] = {};
    double mainField_[kLatticeSize] = {};
    double scaleField_[kLatticeX * kLatticeZ] = {};
    double depthField_[kLatticeX * kLatticeZ] = {};
    double sandField_[256] = {};
    double gravelField_[256] = {};
    double surfaceDepthField_[256] = {};
};

}  // namespace mc::worldgen
