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

// Collision's equivalent of renderOf. See core/block/collision.hpp for the
// boxes themselves -- this only says which family the block belongs to.
inline Shape shapeOf(BlockId id) { return def(id).shape; }

// Ground friction of the block being stood on. Air's value is never read -- the
// original only consults the table when the id below the feet is non-zero.
inline float slipperinessOf(BlockId id) { return def(id).slipperiness; }

// Whether a ray notices the block at all. False for water, lava and fire, and
// true for plenty of things you cannot walk into.
inline bool isTargetable(BlockId id) { return def(id).targetable; }

}  // namespace mc::block
