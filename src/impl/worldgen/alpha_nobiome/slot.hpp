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

// The driver above the generator: `ft`, which decides what is read from the
// save, what is generated, and when a chunk is populated. This is what the
// streamer holds; `WorldGen` is one stage inside it.
using ChunkGenerator = mc::worldgen::ChunkGenerator;

}  // namespace mcver
