#pragma once

// Block lookup.
//
// The table is a constexpr array in .rodata generated from
// data/<version>/blocks.json, so a lookup is one bounds check and an index --
// no map, no runtime parsing, nothing to initialise at startup. This is the
// hottest read in the mesher: every one of the six faces of every block asks
// whether its neighbour is opaque.

#include "blocks.hpp"  // generated; see tools/configure.py
#include "core/block/block_def.hpp"

namespace mc::block {

// Never fails. An id outside the table -- from a modded world or a newer
// server -- comes back as the unknown block rather than reading past the end.
inline const BlockDef& def(BlockId id)
{
    return id < mcver::kBlockTableSize ? mcver::kBlocks[id] : mcver::kUnknownBlock;
}

inline bool isAir(BlockId id) { return id == kAir; }

// The mesher's inner test: does a face touching this block need drawing?
inline bool isOpaque(BlockId id) { return def(id).opaque; }

inline RenderType renderOf(BlockId id) { return def(id).render; }

}  // namespace mc::block
