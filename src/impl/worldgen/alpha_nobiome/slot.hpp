#pragma once

// Binds the `worldgen` slot for versions whose generator has no biomes --
// a1.1.2 and its neighbours, where terrain shape comes from noise alone and
// there is no temperature or humidity map anywhere in the game.
//
// Included by the generated version_slots.hpp; see docs/build-versions.md.
// The generator itself is in docs/worldgen-a1.1.2.md.

#include "impl/worldgen/alpha_nobiome/chunk_generator.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"

namespace mcver {

using WorldGen = mc::worldgen::ChunkProvider;
using WorldGenOptions = mc::worldgen::GeneratorOptions;

// How big a buffer `WorldGen::generateColumn` fills. Named here so a caller
// outside the slot -- the spawn search in core/world/spawn_point.cpp -- can
// size one without knowing the world is 128 blocks tall.
inline constexpr int kWorldGenChunkBlocks = mc::worldgen::kChunkBlocks;

// The driver above the generator: `ft`, which decides what is read from the
// save, what is generated, and when a chunk is populated. This is what the
// streamer holds; `WorldGen` is one stage inside it.
using ChunkGenerator = mc::worldgen::ChunkGenerator;

}  // namespace mcver
