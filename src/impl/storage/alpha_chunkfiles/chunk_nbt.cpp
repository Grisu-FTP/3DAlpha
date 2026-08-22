#include "impl/storage/alpha_chunkfiles/chunk_nbt.hpp"

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"

#include <cstring>

namespace mc::alpha {
namespace {

using world::ChunkColumn;
using world::Section;

constexpr int kRuns = ChunkColumn::kArea;          // one per (x, z) column
constexpr int kRunBlocks = Section::kSize;         // 16 blocks tall
constexpr int kColumnStride = ChunkColumn::kHeight;  // bytes between runs, Blocks
constexpr int kColumnNibbleStride = ChunkColumn::kHeight / 2;

static_assert(ChunkColumn::kHeight % 2 == 0, "nibble runs must stay byte-aligned");
static_assert(Section::kSize % 2 == 0, "section runs must stay byte-aligned");

// A column array is 256 vertical runs laid end to end; a section takes the same
// slice out of every one of them.
void gatherBlocks(ConstByteSpan column, int sy, u8* dst)
{
    const u8* src = column.data() + sy * kRunBlocks;
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kRunBlocks, src + run * kColumnStride, kRunBlocks);
    }
}

void scatterBlocks(const u8* src, int sy, u8* column)
{
    u8* dst = column + sy * kRunBlocks;
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kColumnStride, src + run * kRunBlocks, kRunBlocks);
    }
}

void gatherNibbles(ConstByteSpan column, int sy, u8* dst)
{
    const u8* src = column.data() + sy * (kRunBlocks / 2);
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * (kRunBlocks / 2), src + run * kColumnNibbleStride,
                    kRunBlocks / 2);
    }
}

void scatterNibbles(const u8* src, int sy, u8* column)
{
    u8* dst = column + sy * (kRunBlocks / 2);
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kColumnNibbleStride, src + run * (kRunBlocks / 2),
                    kRunBlocks / 2);
    }
}

// Which of the tags we model have been seen. Blocks and the three nibble planes
// are required: a chunk missing one is not a chunk, and defaulting it to zero
// would write that guess back over the real world on the next save.
struct Seen {
    bool xPos = false;
    bool zPos = false;
    bool blocks = false;
    bool data = false;
    bool blockLight = false;
    bool skyLight = false;

    bool complete() const
    {
        return xPos && zPos && blocks && data && blockLight && skyLight;
    }
};

bool decodeLevel(nbt::Reader& r, ChunkColumn* chunk, u8* scratch)
{
    Seen seen;
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "xPos") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            chunk->x = r.intValue();
            seen.xPos = true;
        } else if (name == "zPos") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            chunk->z = r.intValue();
            seen.zPos = true;
        } else if (name == "TerrainPopulated") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            chunk->terrainPopulated = r.byteValue() != 0;
        } else if (name == "LastUpdate") {
            if (!nbt::expectType(r, type, nbt::TagType::Long)) return false;
            chunk->lastUpdate = r.longValue();
        } else if (name == "Blocks") {
            if (!nbt::expectType(r, type, nbt::TagType::ByteArray)) return false;
            const ConstByteSpan blocks = r.byteArray();
            if (blocks.size() != kBlocksBytes) {
                r.fail();
                break;
            }
            for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
                gatherBlocks(blocks, sy, scratch);
                chunk->section(sy).assignBlocks(ConstByteSpan(scratch, Section::kVolume));
            }
            seen.blocks = true;
        } else if (name == "Data" || name == "BlockLight" || name == "SkyLight") {
            if (!nbt::expectType(r, type, nbt::TagType::ByteArray)) return false;
            const ConstByteSpan plane = r.byteArray();
            if (plane.size() != kNibbleBytes) {
                r.fail();
                break;
            }
            for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
                gatherNibbles(plane, sy, scratch);
                const ConstByteSpan slice(scratch, world::NibbleArray::kBytes);
                Section& s = chunk->section(sy);
                if (name == "Data") {
                    s.data().assign(slice);
                } else if (name == "BlockLight") {
                    s.blockLight().assign(slice);
                } else {
                    s.skyLight().assign(slice);
                }
            }
            if (name == "Data") {
                seen.data = true;
            } else if (name == "BlockLight") {
                seen.blockLight = true;
            } else {
                seen.skyLight = true;
            }
        } else if (name == "HeightMap") {
            if (!nbt::expectType(r, type, nbt::TagType::ByteArray)) return false;
            const ConstByteSpan heights = r.byteArray();
            if (heights.size() != kHeightMapBytes) {
                r.fail();
                break;
            }
            std::memcpy(chunk->heightMap, heights.data(), kHeightMapBytes);
        } else if (!chunk->preserved.capture(r, name, type)) {
            return false;
        }
    }

    return r.ok() && seen.complete();
}

}  // namespace

bool decodeChunk(ConstByteSpan nbt, ChunkColumn* out)
{
    // Built aside and moved in on success, so a truncated or corrupt file
    // leaves the caller's chunk untouched rather than half-loaded.
    ChunkColumn chunk;

    nbt::Reader r(nbt);
    if (!r.enterRoot()) {
        return false;
    }

    // One buffer for every section and every plane: the gather is 4 KB at its
    // widest and allocating per section would be 32 allocations per chunk load.
    std::vector<u8> scratch(Section::kVolume);

    bool sawLevel = false;
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "Level") {
            if (!nbt::expectType(r, type, nbt::TagType::Compound)) {
                return false;
            }
            if (sawLevel || !decodeLevel(r, &chunk, scratch.data())) {
                return false;
            }
            sawLevel = true;
        } else if (!chunk.preservedRoot.capture(r, name, type)) {
            return false;
        }
    }

    if (!r.ok() || !sawLevel) {
        return false;
    }

    *out = std::move(chunk);
    return true;
}

bool encodeChunk(const ChunkColumn& chunk, std::vector<u8>* out)
{
    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        if (chunk.section(sy).maxBlockId() > 255) {
            return false;
        }
    }

    std::vector<u8> blocks(kBlocksBytes);
    std::vector<u8> data(kNibbleBytes);
    std::vector<u8> blockLight(kNibbleBytes);
    std::vector<u8> skyLight(kNibbleBytes);
    std::vector<u8> scratch(Section::kVolume);

    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        const Section& s = chunk.section(sy);

        s.writeBlocks(scratch.data());
        scatterBlocks(scratch.data(), sy, blocks.data());

        s.data().writeTo(scratch.data());
        scatterNibbles(scratch.data(), sy, data.data());

        s.blockLight().writeTo(scratch.data());
        scatterNibbles(scratch.data(), sy, blockLight.data());

        s.skyLight().writeTo(scratch.data());
        scatterNibbles(scratch.data(), sy, skyLight.data());
    }

    nbt::Writer w(*out);
    w.beginRoot();
    w.beginCompound("Level");
    w.writeInt("xPos", chunk.x);
    w.writeInt("zPos", chunk.z);
    w.writeByte("TerrainPopulated", chunk.terrainPopulated ? 1 : 0);
    w.writeLong("LastUpdate", chunk.lastUpdate);
    w.writeByteArray("Blocks", blocks);
    w.writeByteArray("Data", data);
    w.writeByteArray("BlockLight", blockLight);
    w.writeByteArray("SkyLight", skyLight);
    w.writeByteArray("HeightMap", ConstByteSpan(chunk.heightMap, kHeightMapBytes));
    chunk.preserved.writeTo(w);
    w.endCompound();
    chunk.preservedRoot.writeTo(w);
    w.endRoot();

    return w.ok();
}

}  // namespace mc::alpha
