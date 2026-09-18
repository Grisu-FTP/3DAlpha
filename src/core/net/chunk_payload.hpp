#pragma once

// Map Chunk (0x33) payloads: inflating them, writing them into columns, and
// making them, the last for tests and the host harness.
//
// **The byte layout is `im.a([BIIIIIII)I` on the server**, run through once per
// chunk the region touches. For each chunk, in turn: the block ids of every
// (x, z) run between the region's y bounds, x-major; then the same runs of
// metadata, of block light and of sky light, each as `(y1 - y0) / 2` raw nibble
// bytes starting at byte `(x << 11 | z << 7 | y0) >> 1`. That is the on-disk
// YZX order, which is why a whole column arrives as the four NBT arrays laid
// end to end. The integer division is the original's: a region with an odd
// height copies one nibble byte short, and so does this.
//
// Chunks are visited x-major and then z, with the local bounds clamped to the
// chunk -- `dy.c(IIIIII)` -- so a region that straddles a chunk border is
// several copies back to back rather than one flat box.

#include "core/net/packets.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <memory>
#include <vector>

namespace mc::net {

struct MapChunkRegion {
    i32 x = 0;
    int y = 0;
    i32 z = 0;
    int sizeX = 0;
    int sizeY = 0;
    int sizeZ = 0;
    std::vector<u8> data;  // exactly sizeX * sizeY * sizeZ * 5 / 2 bytes

    // The one shape that stands for a whole column: what the server sends when a
    // chunk comes into view, as against a burst of edits inside one.
    bool wholeColumn() const
    {
        return y == 0 && sizeX == 16 && sizeY == world::ChunkColumn::kHeight && sizeZ == 16
               && (x & 15) == 0 && (z & 15) == 0;
    }
};

// `cz.a(DataInputStream)`'s second half: sizes plus one, then a zlib inflate
// into a buffer of exactly the size the bounds need. Like `Inflater.inflate`,
// a stream that ends early leaves the rest zero and one that runs long is cut
// off; only a stream that is not zlib at all fails.
bool inflateMapChunk(const Packet& packet, MapChunkRegion* out);

// A finished column out of a whole-column region: the four planes, a height
// map, and nothing else -- a Map Chunk carries no tile entities. Null when the
// region is not a whole column.
std::unique_ptr<world::ChunkColumn> buildColumn(const MapChunkRegion& region);

using ColumnLookup = world::ChunkColumn* (*)(void* ctx, i32 chunkX, i32 chunkZ);

// Writes a region into whatever columns `lookup` returns, and returns the
// number of columns written. A chunk it has no column for is skipped, but its
// bytes are still stepped over, so the chunks after it land where they should.
// Height maps of the columns written are recomputed; `scratch` is reused for
// that and must not be shared across threads.
int applyMapChunk(const MapChunkRegion& region, ColumnLookup lookup, void* ctx,
                  std::vector<u8>* scratch);

// Recomputes a column's height map from its blocks, for a column written to
// some other way than `applyMapChunk`. `scratch` as there.
void refreshHeightMap(world::ChunkColumn& column, std::vector<u8>* scratch);

using ConstColumnLookup = const world::ChunkColumn* (*)(void* ctx, i32 chunkX, i32 chunkZ);

// `dy.c(IIIIII)`: the uncompressed payload for a region, as the server makes
// it. A chunk the lookup has no column for contributes zeros.
void serializeRegion(ConstColumnLookup lookup, void* ctx, i32 x, int y, i32 z, int sizeX,
                     int sizeY, int sizeZ, std::vector<u8>* out);

// The whole 0x33, deflated the way `cz`'s constructor does it.
bool makeMapChunk(ConstColumnLookup lookup, void* ctx, i32 x, int y, i32 z, int sizeX, int sizeY,
                  int sizeZ, Packet* out);

}  // namespace mc::net
