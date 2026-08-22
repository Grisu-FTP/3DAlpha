#pragma once

// Binds the `storage` slot for versions that use the Alpha level format.
// Included by the generated version_slots.hpp; see docs/build-versions.md.

#include "impl/storage/alpha_chunkfiles/storage.hpp"

namespace mcver {

using Storage = mc::alpha::AlphaChunkFileStorage;

}  // namespace mcver
