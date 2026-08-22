#include "framework.hpp"

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"
#include "core/world/chunk.hpp"
#include "impl/storage/alpha_chunkfiles/chunk_nbt.hpp"

#include <vector>

using namespace mc;
using world::BlockId;
using world::ChunkColumn;
using world::Section;

namespace {

struct Lcg {
    u32 state;
    u32 next()
    {
        state = state * 1103515245u + 12345u;
        return state >> 8;
    }
};

// The Alpha column arrays, addressed exactly as the format documents them, so
// the tests pin our indexing against the spec rather than against our own code.
struct RawColumn {
    std::vector<u8> blocks{std::vector<u8>(alpha::kBlocksBytes, 0)};
    std::vector<u8> data{std::vector<u8>(alpha::kNibbleBytes, 0)};
    std::vector<u8> blockLight{std::vector<u8>(alpha::kNibbleBytes, 0)};
    std::vector<u8> skyLight{std::vector<u8>(alpha::kNibbleBytes, 0)};
    std::vector<u8> heightMap{std::vector<u8>(alpha::kHeightMapBytes, 0)};

    static usize index(int x, int y, int z)
    {
        return usize(y) + usize(z) * ChunkColumn::kHeight +
               usize(x) * ChunkColumn::kHeight * ChunkColumn::kWidth;
    }

    void setBlock(int x, int y, int z, u8 id) { blocks[index(x, y, z)] = id; }

    static void setNibble(std::vector<u8>& plane, int x, int y, int z, u8 value)
    {
        const usize i = index(x, y, z);
        u8& byte = plane[i >> 1];
        if (i & 1) {
            byte = u8((byte & 0x0F) | u8(value << 4));
        } else {
            byte = u8((byte & 0xF0) | (value & 0x0F));
        }
    }
};

struct BuildOptions {
    bool omitBlocks = false;
    bool omitSkyLight = false;
    bool shortBlocks = false;
    bool extraTag = false;
    bool entitiesList = false;
    bool rootSibling = false;
    bool xPosAsShort = false;
};

// Writes the tags in the same order encodeChunk does, so a faithful round trip
// is byte-identical and the test can say so rather than compare field by field.
std::vector<u8> buildChunkNbt(const RawColumn& raw, i32 x, i32 z, const BuildOptions& opt = {})
{
    std::vector<u8> out;
    nbt::Writer w(out);
    w.beginRoot();
    w.beginCompound("Level");
    if (opt.xPosAsShort) {
        w.writeShort("xPos", i16(x));
    } else {
        w.writeInt("xPos", x);
    }
    w.writeInt("zPos", z);
    w.writeByte("TerrainPopulated", 1);
    w.writeLong("LastUpdate", 1234567890123LL);
    if (!opt.omitBlocks) {
        if (opt.shortBlocks) {
            w.writeByteArray("Blocks", ConstByteSpan(raw.blocks.data(), 100));
        } else {
            w.writeByteArray("Blocks", raw.blocks);
        }
    }
    w.writeByteArray("Data", raw.data);
    w.writeByteArray("BlockLight", raw.blockLight);
    if (!opt.omitSkyLight) {
        w.writeByteArray("SkyLight", raw.skyLight);
    }
    w.writeByteArray("HeightMap", raw.heightMap);
    if (opt.entitiesList) {
        w.beginList("Entities", nbt::TagType::Compound);
        w.beginListElementCompound();
        w.writeString("id", "Pig");
        w.writeShort("Health", 10);
        w.endCompound();
        w.endList();
    }
    if (opt.extraTag) {
        w.writeString("SomeModTag", "written by a tool we know nothing about");
    }
    w.endCompound();
    if (opt.rootSibling) {
        w.writeInt("StrayRootTag", 99);
    }
    w.endRoot();
    return out;
}

RawColumn randomColumn(u32 seed)
{
    Lcg rng{seed};
    RawColumn raw;
    for (int x = 0; x < ChunkColumn::kWidth; ++x) {
        for (int z = 0; z < ChunkColumn::kWidth; ++z) {
            const int height = 40 + int(rng.next() % 30);
            for (int y = 0; y < height; ++y) {
                raw.setBlock(x, y, z, u8(1 + (rng.next() % 60)));
                RawColumn::setNibble(raw.data, x, y, z, u8(rng.next() % 16));
                RawColumn::setNibble(raw.blockLight, x, y, z, u8(rng.next() % 16));
            }
            for (int y = height; y < ChunkColumn::kHeight; ++y) {
                RawColumn::setNibble(raw.skyLight, x, y, z, 15);
            }
            raw.heightMap[usize(z) * ChunkColumn::kWidth + usize(x)] = u8(height);
        }
    }
    return raw;
}

}  // namespace

TEST(decode_reads_the_column_arrays_in_yzx_order)
{
    // Getting this wrong scrambles the world in a way that still looks like
    // terrain, so it is pinned against the documented index formula directly.
    RawColumn raw;
    raw.setBlock(0, 0, 0, 1);
    raw.setBlock(3, 100, 9, 7);
    raw.setBlock(15, 127, 15, 49);
    RawColumn::setNibble(raw.data, 3, 100, 9, 11);
    RawColumn::setNibble(raw.blockLight, 3, 100, 9, 5);
    RawColumn::setNibble(raw.skyLight, 3, 100, 9, 14);

    const std::vector<u8> nbt = buildChunkNbt(raw, -13, 44);

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(nbt, &chunk));
    CHECK_EQ(chunk.x, -13);
    CHECK_EQ(chunk.z, 44);
    CHECK_EQ(chunk.terrainPopulated, true);
    CHECK_EQ(chunk.lastUpdate, 1234567890123LL);

    CHECK_EQ(chunk.block(0, 0, 0), BlockId(1));
    CHECK_EQ(chunk.block(3, 100, 9), BlockId(7));
    CHECK_EQ(chunk.block(15, 127, 15), BlockId(49));
    CHECK_EQ(chunk.block(9, 100, 3), BlockId(0));  // not a transposed read

    CHECK_EQ(chunk.blockData(3, 100, 9), u8(11));
    CHECK_EQ(chunk.blockLight(3, 100, 9), u8(5));
    CHECK_EQ(chunk.skyLight(3, 100, 9), u8(14));
}

TEST(a_realistic_column_round_trips_byte_for_byte)
{
    const RawColumn raw = randomColumn(42);
    const std::vector<u8> original = buildChunkNbt(raw, 5, -9);

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(original, &chunk));

    std::vector<u8> encoded;
    CHECK(alpha::encodeChunk(chunk, &encoded));
    CHECK_EQ(encoded.size(), original.size());
    CHECK(encoded == original);
}

TEST(every_block_and_nibble_survives_the_round_trip)
{
    const RawColumn raw = randomColumn(7);
    const std::vector<u8> original = buildChunkNbt(raw, 0, 0);

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(original, &chunk));

    for (int x = 0; x < ChunkColumn::kWidth; ++x) {
        for (int z = 0; z < ChunkColumn::kWidth; ++z) {
            for (int y = 0; y < ChunkColumn::kHeight; ++y) {
                const usize i = RawColumn::index(x, y, z);
                if (chunk.block(x, y, z) != BlockId(raw.blocks[i])) {
                    CHECK_EQ(chunk.block(x, y, z), BlockId(raw.blocks[i]));
                    return;
                }
                const u8 meta = u8((raw.data[i >> 1] >> ((i & 1) * 4)) & 0x0F);
                if (chunk.blockData(x, y, z) != meta) {
                    CHECK_EQ(chunk.blockData(x, y, z), meta);
                    return;
                }
                const u8 sky = u8((raw.skyLight[i >> 1] >> ((i & 1) * 4)) & 0x0F);
                if (chunk.skyLight(x, y, z) != sky) {
                    CHECK_EQ(chunk.skyLight(x, y, z), sky);
                    return;
                }
            }
            const usize h = usize(z) * ChunkColumn::kWidth + usize(x);
            CHECK_EQ(chunk.heightMap[h], raw.heightMap[h]);
        }
    }
}

TEST(compacting_does_not_change_what_gets_saved)
{
    // compact() rebuilds every palette; if it ever reordered or lost a block
    // the saved file would differ, which is the cheapest way to catch it.
    const RawColumn raw = randomColumn(99);
    const std::vector<u8> original = buildChunkNbt(raw, 1, 2);

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(original, &chunk));
    chunk.compact();

    std::vector<u8> encoded;
    CHECK(alpha::encodeChunk(chunk, &encoded));
    CHECK(encoded == original);
}

TEST(unmodelled_tags_survive_a_load_and_save)
{
    // Entities, TileEntities and anything a server or tool added must come back
    // untouched. Dropping one is silent world corruption.
    BuildOptions opt;
    opt.extraTag = true;
    opt.entitiesList = true;
    opt.rootSibling = true;

    const RawColumn raw = randomColumn(3);
    const std::vector<u8> original = buildChunkNbt(raw, -1, -1, opt);

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(original, &chunk));
    CHECK_EQ(chunk.preserved.size(), usize(2));
    CHECK(chunk.preserved.find("Entities") != nullptr);
    CHECK(chunk.preserved.find("SomeModTag") != nullptr);
    CHECK_EQ(chunk.preservedRoot.size(), usize(1));
    CHECK(chunk.preservedRoot.find("StrayRootTag") != nullptr);

    std::vector<u8> encoded;
    CHECK(alpha::encodeChunk(chunk, &encoded));
    CHECK(encoded == original);
}

TEST(a_chunk_missing_a_required_array_is_rejected)
{
    const RawColumn raw = randomColumn(1);

    BuildOptions noBlocks;
    noBlocks.omitBlocks = true;
    ChunkColumn chunk;
    CHECK(!alpha::decodeChunk(buildChunkNbt(raw, 0, 0, noBlocks), &chunk));

    BuildOptions noSky;
    noSky.omitSkyLight = true;
    CHECK(!alpha::decodeChunk(buildChunkNbt(raw, 0, 0, noSky), &chunk));

    BuildOptions shortBlocks;
    shortBlocks.shortBlocks = true;
    CHECK(!alpha::decodeChunk(buildChunkNbt(raw, 0, 0, shortBlocks), &chunk));
}

TEST(a_modelled_chunk_tag_with_the_wrong_type_is_rejected)
{
    // Carrying it through as an unknown tag instead would save two tags called
    // xPos: the preserved Short and the Int the encoder always writes.
    BuildOptions opt;
    opt.xPosAsShort = true;

    ChunkColumn chunk;
    CHECK(!alpha::decodeChunk(buildChunkNbt(randomColumn(2), 3, 3, opt), &chunk));
}

TEST(a_failed_decode_leaves_the_target_alone)
{
    const RawColumn good = randomColumn(11);
    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(buildChunkNbt(good, 4, 4), &chunk));
    const BlockId before = chunk.block(0, 0, 0);

    BuildOptions noBlocks;
    noBlocks.omitBlocks = true;
    CHECK(!alpha::decodeChunk(buildChunkNbt(good, 9, 9, noBlocks), &chunk));

    CHECK_EQ(chunk.x, 4);
    CHECK_EQ(chunk.block(0, 0, 0), before);
}

TEST(truncated_chunk_files_fail_cleanly_at_every_length)
{
    // Chunk files come off an SD card that can be pulled mid-write. Every
    // prefix must be a clean rejection, never a crash or a partial load.
    const RawColumn raw = randomColumn(5);
    const std::vector<u8> full = buildChunkNbt(raw, 2, 2);

    for (usize len = 0; len < full.size(); len += 97) {
        ChunkColumn chunk;
        alpha::decodeChunk(ConstByteSpan(full.data(), len), &chunk);
    }
    for (usize len = full.size() > 64 ? full.size() - 64 : 0; len < full.size(); ++len) {
        ChunkColumn chunk;
        CHECK(!alpha::decodeChunk(ConstByteSpan(full.data(), len), &chunk));
    }
}

TEST(corrupted_chunk_files_fail_cleanly)
{
    const RawColumn raw = randomColumn(17);
    std::vector<u8> full = buildChunkNbt(raw, 0, 0);

    // The header is where a flipped byte turns into a bogus length or type.
    // Bodies are opaque bytes, so corrupting them is not interesting.
    for (usize i = 0; i < 64 && i < full.size(); ++i) {
        const u8 original = full[i];
        for (u8 flip : {u8(0x00), u8(0xFF), u8(0x7F)}) {
            full[i] = flip;
            ChunkColumn chunk;
            alpha::decodeChunk(full, &chunk);
        }
        full[i] = original;
    }
}

TEST(an_id_the_format_cannot_hold_refuses_to_save)
{
    // Alpha's Blocks array is one byte per block. Truncating a 16-bit id would
    // write a plausible-looking wrong world; refusing is the only safe answer.
    const RawColumn raw = randomColumn(23);
    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(buildChunkNbt(raw, 0, 0), &chunk));

    std::vector<u8> encoded;
    CHECK(alpha::encodeChunk(chunk, &encoded));

    chunk.setBlock(1, 20, 1, 300);
    encoded.clear();
    CHECK(!alpha::encodeChunk(chunk, &encoded));
}
