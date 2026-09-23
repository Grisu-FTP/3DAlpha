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

// `def(id).tickRandomly`, one byte per id. The random tick asks it of a block at
// a random offset eighty times a column a tick, and a whole BlockDef row is a
// cache line of its own for one bool; this is the same column, derived from
// the same table at compile time, packed so the whole of it is 256 bytes.
struct RandomTickTable {
    bool ticks[mcver::kBlockTableSize];
};

constexpr RandomTickTable buildRandomTickTable()
{
    RandomTickTable table{};
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        table.ticks[id] = mcver::kBlocks[id].tickRandomly;
    }
    return table;
}

inline constexpr RandomTickTable kRandomTicks = buildRandomTickTable();

inline bool ticksRandomly(BlockId id)
{
    return id < mcver::kBlockTableSize ? kRandomTicks.ticks[id] : mcver::kUnknownBlock.tickRandomly;
}

// `Block.stepSound`. Air and any id this build does not know come back as the
// silent row, so a caller never has to ask whether the block exists first.
inline const StepSound& stepSoundOf(BlockId id)
{
    const u8 row = def(id).stepSound;
    return mcver::kStepSounds[row < mcver::kStepSoundCount ? row : 0];
}

// The six atlas tiles a block shows **in the world** at this metadata. That is
// `faces` for everything but a furnace, whose mouth is on the side its
// metadata names -- `ku.a(Lnm;IIII)I` -- where `faces` is the inventory answer
// and always has it on +Z. Only the mesher's out-of-line path asks: a block
// with a metadata table is never `unitCube`.
inline const u16* worldFaces(BlockId id, u8 metadata)
{
    const u8 row = id < mcver::kBlockTableSize ? mcver::kMetadataFaceRow[id] : 0;
    if (row == 0) {
        return def(id).faces;
    }
    return mcver::kMetadataFaces[row - 1][metadata & 15];
}

// The block a staircase is built from, which it turns into when something
// solid is put on top of it. Air for every block that is not built from one.
inline BlockId modelOf(BlockId id)
{
    return id < mcver::kBlockTableSize ? mcver::kModelBlock[id] : kAir;
}

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

// The same, for a ray that has been told to notice liquids -- which is one
// item in a1.1.2 and it is the bucket. `hitLiquids` false is the ordinary
// crosshair and answers exactly as the call above; true additionally sees a
// *source* block of water or lava and still not a flowing one. See
// BlockDef::targetableLiquids.
inline bool isTargetable(BlockId id, u8 metadata, bool hitLiquids)
{
    if (!hitLiquids) {
        return def(id).targetable;
    }
    return (def(id).targetableLiquids & (1u << (metadata & 15))) != 0;
}

}  // namespace mc::block
