// Map Chunk payloads in and out. See chunk_payload.hpp.

#include "core/net/chunk_payload.hpp"

#include "core/util/compress.hpp"
#include "core/world/lighting.hpp"
#include "core/world/section.hpp"

#include <zlib.h>

#include <cstring>

namespace mc::net {

namespace {

using world::ChunkColumn;
using world::Section;

constexpr int kHeight = ChunkColumn::kHeight;
constexpr usize kColumnBlocks = usize(16 * 16 * ChunkColumn::kHeight);

usize payloadBytes(int sizeX, int sizeY, int sizeZ)
{
    return usize(sizeX) * usize(sizeY) * usize(sizeZ) * 5 / 2;
}

// The per-chunk bounds `dy.c` hands `im.a`, clamped to the chunk and the world.
struct ChunkSpan {
    i32 chunkX;
    i32 chunkZ;
    int x0, y0, z0, x1, y1, z1;
};

template <typename Visit>
void forEachChunk(i32 x, int y, i32 z, int sizeX, int sizeY, int sizeZ, Visit visit)
{
    const i32 cx0 = x >> 4;
    const i32 cz0 = z >> 4;
    const i32 cx1 = (x + sizeX - 1) >> 4;
    const i32 cz1 = (z + sizeZ - 1) >> 4;

    int y0 = y;
    int y1 = y + sizeY;
    if (y0 < 0) y0 = 0;
    if (y1 > kHeight) y1 = kHeight;

    for (i32 cx = cx0; cx <= cx1; ++cx) {
        int x0 = int(x - cx * 16);
        int x1 = int(x + sizeX - cx * 16);
        if (x0 < 0) x0 = 0;
        if (x1 > 16) x1 = 16;
        for (i32 cz = cz0; cz <= cz1; ++cz) {
            int z0 = int(z - cz * 16);
            int z1 = int(z + sizeZ - cz * 16);
            if (z0 < 0) z0 = 0;
            if (z1 > 16) z1 = 16;
            visit(ChunkSpan{cx, cz, x0, y0, z0, x1, y1, z1});
        }
    }
}

using NibbleGet = u8 (ChunkColumn::*)(int, int, int) const;
using NibbleSet = void (ChunkColumn::*)(int, int, int, u8);

}  // namespace

void refreshHeightMap(ChunkColumn& column, std::vector<u8>* scratch)
{
    scratch->resize(kColumnBlocks);
    u8* flat = scratch->data();
    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            const int base = (lx << 11) | (lz << 7);
            for (int y = 0; y < kHeight; ++y) {
                flat[base + y] = u8(column.block(lx, y, lz));
            }
        }
    }
    world::computeHeightMap(flat, column.heightMap);
}

bool inflateMapChunk(const Packet& packet, MapChunkRegion* out)
{
    if (packet.id != packet::MapChunk || packet.intCount != 6) {
        return false;
    }
    out->x = i32(packet.ints[0]);
    out->y = int(packet.ints[1]);
    out->z = i32(packet.ints[2]);
    out->sizeX = int(packet.ints[3]) + 1;
    out->sizeY = int(packet.ints[4]) + 1;
    out->sizeZ = int(packet.ints[5]) + 1;

    const usize expected = payloadBytes(out->sizeX, out->sizeY, out->sizeZ);
    out->data.assign(expected, 0);

    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return false;
    }
    stream.next_in = const_cast<Bytef*>(packet.bytes.data());
    stream.avail_in = uInt(packet.bytes.size());
    stream.next_out = out->data.data();
    stream.avail_out = uInt(expected);
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    // Z_BUF_ERROR is both "ran out of input" and "ran out of room" here, and
    // `Inflater.inflate` treats both as a short read rather than a failure.
    return result == Z_STREAM_END || result == Z_OK || result == Z_BUF_ERROR;
}

std::unique_ptr<ChunkColumn> buildColumn(const MapChunkRegion& region)
{
    if (!region.wholeColumn() || region.data.size() != kColumnBlocks * 5 / 2) {
        return nullptr;
    }

    auto column = std::make_unique<ChunkColumn>(region.x >> 4, region.z >> 4);
    column->terrainPopulated = true;

    const u8* blocks = region.data.data();
    const u8* planes[3] = {blocks + kColumnBlocks, blocks + kColumnBlocks * 3 / 2,
                           blocks + kColumnBlocks * 2};

    constexpr int kRunBlocks = Section::kSize;
    constexpr int kRuns = ChunkColumn::kArea;
    u8 scratch[Section::kVolume];
    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        Section& section = column->section(sy);
        for (int run = 0; run < kRuns; ++run) {
            std::memcpy(scratch + run * kRunBlocks, blocks + run * kHeight + sy * kRunBlocks,
                        kRunBlocks);
        }
        section.assignBlocks(ConstByteSpan(scratch, Section::kVolume));

        for (int p = 0; p < 3; ++p) {
            for (int run = 0; run < kRuns; ++run) {
                std::memcpy(scratch + run * (kRunBlocks / 2),
                            planes[p] + run * (kHeight / 2) + sy * (kRunBlocks / 2),
                            kRunBlocks / 2);
            }
            const ConstByteSpan slice(scratch, world::NibbleArray::kBytes);
            if (p == 0) {
                section.data().assign(slice);
            } else if (p == 1) {
                section.blockLight().assign(slice);
            } else {
                section.skyLight().assign(slice);
            }
        }
    }

    world::computeHeightMap(blocks, column->heightMap);
    column->compact();
    return column;
}

int applyMapChunk(const MapChunkRegion& region, ColumnLookup lookup, void* ctx,
                  std::vector<u8>* scratch)
{
    const usize size = region.data.size();
    const u8* data = region.data.data();
    usize offset = 0;
    int written = 0;

    forEachChunk(region.x, region.y, region.z, region.sizeX, region.sizeY, region.sizeZ,
                 [&](const ChunkSpan& span) {
        ChunkColumn* column = lookup(ctx, span.chunkX, span.chunkZ);
        const int runBlocks = span.y1 - span.y0;
        const int runNibbles = runBlocks / 2;

        for (int lx = span.x0; lx < span.x1; ++lx) {
            for (int lz = span.z0; lz < span.z1; ++lz) {
                if (runBlocks <= 0 || offset + usize(runBlocks) > size) {
                    continue;
                }
                if (column != nullptr) {
                    for (int y = span.y0; y < span.y1; ++y) {
                        column->setBlock(lx, y, lz, data[offset + usize(y - span.y0)]);
                    }
                }
                offset += usize(runBlocks);
            }
        }

        const NibbleSet setters[3] = {&ChunkColumn::setBlockData, &ChunkColumn::setBlockLight,
                                      &ChunkColumn::setSkyLight};
        for (const NibbleSet set : setters) {
            for (int lx = span.x0; lx < span.x1; ++lx) {
                for (int lz = span.z0; lz < span.z1; ++lz) {
                    if (runNibbles <= 0 || offset + usize(runNibbles) > size) {
                        continue;
                    }
                    if (column != nullptr) {
                        // Byte k covers the nibble pair its alpha index starts at,
                        // which is (y0 & ~1) + 2k -- one below y0 when y0 is odd.
                        const int firstY = span.y0 & ~1;
                        for (int k = 0; k < runNibbles; ++k) {
                            const u8 b = data[offset + usize(k)];
                            const int y = firstY + 2 * k;
                            (column->*set)(lx, y, lz, u8(b & 15));
                            if (y + 1 < kHeight) {
                                (column->*set)(lx, y + 1, lz, u8(b >> 4));
                            }
                        }
                    }
                    offset += usize(runNibbles);
                }
            }
        }

        if (column != nullptr) {
            refreshHeightMap(*column, scratch);
            ++written;
        }
    });
    return written;
}

void serializeRegion(ConstColumnLookup lookup, void* ctx, i32 x, int y, i32 z, int sizeX,
                     int sizeY, int sizeZ, std::vector<u8>* out)
{
    out->assign(payloadBytes(sizeX, sizeY, sizeZ), 0);
    usize offset = 0;
    const usize size = out->size();

    forEachChunk(x, y, z, sizeX, sizeY, sizeZ, [&](const ChunkSpan& span) {
        const ChunkColumn* column = lookup(ctx, span.chunkX, span.chunkZ);
        const int runBlocks = span.y1 - span.y0;
        const int runNibbles = runBlocks / 2;

        for (int lx = span.x0; lx < span.x1; ++lx) {
            for (int lz = span.z0; lz < span.z1; ++lz) {
                if (runBlocks <= 0 || offset + usize(runBlocks) > size) continue;
                for (int yy = span.y0; yy < span.y1 && column != nullptr; ++yy) {
                    (*out)[offset + usize(yy - span.y0)] = u8(column->block(lx, yy, lz));
                }
                offset += usize(runBlocks);
            }
        }

        const NibbleGet getters[3] = {&ChunkColumn::blockData, &ChunkColumn::blockLight,
                                      &ChunkColumn::skyLight};
        for (const NibbleGet get : getters) {
            for (int lx = span.x0; lx < span.x1; ++lx) {
                for (int lz = span.z0; lz < span.z1; ++lz) {
                    if (runNibbles <= 0 || offset + usize(runNibbles) > size) continue;
                    const int firstY = span.y0 & ~1;
                    for (int k = 0; k < runNibbles && column != nullptr; ++k) {
                        const int yy = firstY + 2 * k;
                        const u8 lo = (column->*get)(lx, yy, lz);
                        const u8 hi = yy + 1 < kHeight ? (column->*get)(lx, yy + 1, lz) : 0;
                        (*out)[offset + usize(k)] = u8((hi << 4) | (lo & 15));
                    }
                    offset += usize(runNibbles);
                }
            }
        }
    });
}

bool makeMapChunk(ConstColumnLookup lookup, void* ctx, i32 x, int y, i32 z, int sizeX, int sizeY,
                  int sizeZ, Packet* out)
{
    std::vector<u8> raw;
    serializeRegion(lookup, ctx, x, y, z, sizeX, sizeY, sizeZ, &raw);

    out->reset(packet::MapChunk);
    out->pushInt(x);
    out->pushInt(y);
    out->pushInt(z);
    out->pushInt(sizeX - 1);
    out->pushInt(sizeY - 1);
    out->pushInt(sizeZ - 1);
    // `new Deflater(1)` -- the fastest level -- is what the server uses.
    return zip::compress(ConstByteSpan(raw.data(), raw.size()), out->bytes, zip::Wrapper::Zlib,
                         1);
}

}  // namespace mc::net
