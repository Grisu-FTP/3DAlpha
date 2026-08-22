#pragma once

// The Alpha chunk NBT layout <-> ChunkColumn.
//
//   Compound ""
//     Compound "Level"
//       Int       xPos, zPos
//       Byte      TerrainPopulated
//       Long      LastUpdate
//       ByteArray Blocks       32768   one id per block
//       ByteArray Data         16384   nibbles
//       ByteArray BlockLight   16384   nibbles
//       ByteArray SkyLight     16384   nibbles
//       ByteArray HeightMap      256   one byte per XZ column, ZX order
//       List<Compound> Entities, TileEntities
//
// The column arrays are Y-fastest: index = y + z*128 + x*128*16. Our sections
// use the same order, so a section's slice of a column is 256 contiguous runs
// -- 16 bytes each for Blocks, 8 for a nibble plane -- and conversion is memcpy
// in both directions rather than 4096 shifts per section per plane.
//
// These take and produce *decompressed* NBT. Chunk files are gzip and Map Chunk
// payloads are raw zlib, and keeping the wrapper out of here is what lets both
// share one decoder. See core/util/compress.hpp.
//
// Tags not listed above -- and Entities/TileEntities until there is an entity
// system -- are carried through verbatim, so a load/save cycle never destroys
// data written by a server, a tool, or a later game version.

#include "core/util/span.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <vector>

namespace mc::alpha {

// Column array sizes, derived rather than written down, so a version with a
// different world height does not need this file edited.
inline constexpr usize kColumnBlockCount =
    usize(world::ChunkColumn::kArea) * usize(world::ChunkColumn::kHeight);
inline constexpr usize kBlocksBytes = kColumnBlockCount;
inline constexpr usize kNibbleBytes = kColumnBlockCount / 2;
inline constexpr usize kHeightMapBytes = usize(world::ChunkColumn::kArea);

// Fills *out from decompressed chunk NBT. *out is only written on success, so a
// malformed file cannot leave a half-loaded chunk behind. The chunk's own
// xPos/zPos are taken from the file; the caller should check them against the
// filename, which is authoritative.
bool decodeChunk(ConstByteSpan nbt, world::ChunkColumn* out);

// Appends decompressed chunk NBT for *chunk*. Fails if the column holds a block
// id above 255, which this format has no room for -- better a refused save than
// a world silently written with the wrong blocks.
bool encodeChunk(const world::ChunkColumn& chunk, std::vector<u8>* out);

}  // namespace mc::alpha
