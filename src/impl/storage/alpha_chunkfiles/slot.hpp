#pragma once

// Binds the `storage` slot for versions that use the Alpha level format.
// Included by the generated version_slots.hpp; see docs/build-versions.md.

#include "impl/storage/alpha_chunkfiles/codec.hpp"
#include "impl/storage/alpha_chunkfiles/storage.hpp"

namespace mcver {

using Storage = mc::alpha::AlphaChunkFileStorage;

// The payload codecs, bound beside the storage for the same reason: a backend
// that is not this slot -- the packed container in core/world/format/ -- still
// has to speak this version's chunk and level bytes, and core must reach them
// through an alias rather than by naming an implementation.
using ChunkCodec = mc::alpha::AlphaChunkCodec;
using LevelCodec = mc::alpha::AlphaLevelCodec;
using ChunkLayout = mc::alpha::AlphaChunkLayout;

}  // namespace mcver
