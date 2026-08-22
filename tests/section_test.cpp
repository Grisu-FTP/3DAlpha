#include "framework.hpp"

#include "core/world/chunk.hpp"
#include "core/world/nibble_array.hpp"
#include "core/world/section.hpp"

#include <vector>

using namespace mc;
using world::BlockId;
using world::ChunkColumn;
using world::NibbleArray;
using world::Section;
using world::SectionEncoding;

namespace {

// A fixed LCG, so a failure is reproducible and a passing run means something.
struct Lcg {
    u32 state;
    u32 next()
    {
        state = state * 1103515245u + 12345u;
        return state >> 8;
    }
};

}  // namespace

TEST(section_index_is_y_fastest)
{
    // The order is not arbitrary: it matches the Alpha column arrays, which is
    // what makes a section's slice of a column 256 contiguous runs.
    CHECK_EQ(Section::index(0, 0, 0), 0);
    CHECK_EQ(Section::index(0, 1, 0), 1);
    CHECK_EQ(Section::index(0, 0, 1), Section::kSize);
    CHECK_EQ(Section::index(1, 0, 0), Section::kSize * Section::kSize);
    CHECK_EQ(Section::index(15, 15, 15), Section::kVolume - 1);
}

TEST(default_section_is_uniform_air_and_costs_nothing)
{
    Section s;
    CHECK(s.isUniformAir());
    CHECK(s.encoding() == SectionEncoding::Uniform);
    CHECK_EQ(s.memoryUsage(), usize(0));
    CHECK_EQ(s.block(0, 0, 0), BlockId(0));
    CHECK_EQ(s.block(Section::kVolume - 1), BlockId(0));

    // Writing the value it already holds must not allocate: relighting and
    // worldgen both rewrite whole sections with values already in place.
    s.setBlock(5, 0);
    s.data().set(5, 0);
    s.skyLight().set(5, 0);
    CHECK(s.isUniform());
    CHECK_EQ(s.memoryUsage(), usize(0));
}

TEST(set_block_promotes_through_every_encoding)
{
    Section s;

    s.setBlock(0, 1);
    CHECK(s.encoding() == SectionEncoding::Palette4);
    CHECK_EQ(s.block(0), BlockId(1));
    CHECK_EQ(s.block(1), BlockId(0));

    // The palette already holds air, so 15 more ids fill it. 16 entries fit in
    // nibbles; the 17th must widen the index array.
    for (int i = 0; i < 15; ++i) {
        s.setBlock(i, BlockId(i + 1));
    }
    CHECK(s.encoding() == SectionEncoding::Palette4);
    s.setBlock(15, 100);
    CHECK(s.encoding() == SectionEncoding::Palette8);

    for (int i = 0; i < 15; ++i) {
        CHECK_EQ(s.block(i), BlockId(i + 1));
    }
    CHECK_EQ(s.block(15), BlockId(100));
    CHECK_EQ(s.block(16), BlockId(0));
}

TEST(palette_overflow_escapes_to_direct_storage)
{
    // 16-bit ids are the reason this path exists: no palette can hold more than
    // 256 entries, and correctness must not depend on the palette fitting.
    Section s;
    for (int i = 0; i < 300; ++i) {
        s.setBlock(i, BlockId(i + 1));
    }
    CHECK(s.encoding() == SectionEncoding::Direct16);
    for (int i = 0; i < 300; ++i) {
        CHECK_EQ(s.block(i), BlockId(i + 1));
    }
    CHECK_EQ(s.block(300), BlockId(0));
    CHECK_EQ(s.maxBlockId(), BlockId(300));
}

TEST(assign_blocks_picks_the_tightest_encoding)
{
    std::vector<u8> flat(Section::kVolume, 0);

    Section s;
    CHECK(s.assignBlocks(flat));
    CHECK(s.encoding() == SectionEncoding::Uniform);
    CHECK_EQ(s.memoryUsage(), usize(0));

    // A stone section with a few ores: still nibble-indexed.
    for (int i = 0; i < Section::kVolume; ++i) {
        flat[i] = u8(1 + (i % 4));
    }
    CHECK(s.assignBlocks(flat));
    CHECK(s.encoding() == SectionEncoding::Palette4);

    for (int i = 0; i < Section::kVolume; ++i) {
        flat[i] = u8(i % 40);
    }
    CHECK(s.assignBlocks(flat));
    CHECK(s.encoding() == SectionEncoding::Palette8);

    // Wrong length is a caller error the loader must catch, not clamp.
    std::vector<u8> shortFlat(Section::kVolume - 1, 0);
    CHECK(!s.assignBlocks(shortFlat));
}

TEST(assign_and_write_blocks_round_trip)
{
    Lcg rng{12345};
    std::vector<u8> flat(Section::kVolume);
    for (int i = 0; i < Section::kVolume; ++i) {
        flat[i] = u8(rng.next() % 50);
    }

    Section s;
    CHECK(s.assignBlocks(flat));
    for (int i = 0; i < Section::kVolume; ++i) {
        CHECK_EQ(s.block(i), BlockId(flat[i]));
    }

    std::vector<u8> back(Section::kVolume, 0xFF);
    s.writeBlocks(back.data());
    CHECK(back == flat);
}

TEST(compact_drops_an_encoding_that_is_no_longer_needed)
{
    Section s;
    for (int i = 0; i < 300; ++i) {
        s.setBlock(i, BlockId(i + 1));
    }
    CHECK(s.encoding() == SectionEncoding::Direct16);

    // Mining a section out leaves it in the widest encoding it ever reached;
    // compact is what gives the memory back.
    for (int i = 0; i < Section::kVolume; ++i) {
        s.setBlock(i, 0);
    }
    CHECK(s.encoding() == SectionEncoding::Direct16);
    s.compact();
    CHECK(s.encoding() == SectionEncoding::Uniform);
    CHECK(s.isUniformAir());
    CHECK_EQ(s.memoryUsage(), usize(0));

    // And it preserves contents on the way down.
    for (int i = 0; i < Section::kVolume; ++i) {
        s.setBlock(i, BlockId(1 + (i % 3)));
    }
    s.compact();
    CHECK(s.encoding() == SectionEncoding::Palette4);
    for (int i = 0; i < Section::kVolume; ++i) {
        CHECK_EQ(s.block(i), BlockId(1 + (i % 3)));
    }
}

TEST(clone_is_a_deep_copy)
{
    Section s;
    s.setBlock(7, 42);
    s.data().set(7, 9);
    s.skyLight().set(7, 15);

    Section copy = s.clone();
    s.setBlock(7, 1);
    s.data().set(7, 0);

    CHECK_EQ(copy.block(7), BlockId(42));
    CHECK_EQ(copy.data().get(7), u8(9));
    CHECK_EQ(copy.skyLight().get(7), u8(15));
}

TEST(nibble_array_stays_uniform_until_a_value_differs)
{
    NibbleArray plane(15);
    CHECK(plane.isUniform());
    CHECK_EQ(plane.get(0), u8(15));
    CHECK_EQ(plane.get(NibbleArray::kCount - 1), u8(15));
    CHECK_EQ(plane.memoryUsage(), usize(0));

    plane.set(100, 15);
    CHECK(plane.isUniform());

    plane.set(100, 4);
    CHECK(!plane.isUniform());
    CHECK_EQ(plane.get(100), u8(4));
    CHECK_EQ(plane.get(99), u8(15));
    CHECK_EQ(plane.get(101), u8(15));
    CHECK_EQ(plane.memoryUsage(), usize(NibbleArray::kBytes));

    plane.fill(0);
    CHECK(plane.isUniform());
    CHECK_EQ(plane.memoryUsage(), usize(0));
}

TEST(nibble_array_compacts_a_plane_filled_one_value_at_a_time)
{
    // How the lighting engine fills sky light above the terrain: 4096 separate
    // writes of 15 into a plane that started at 0. Nothing notices it has
    // become uniform again until compact() looks.
    NibbleArray plane(0);
    for (int i = 0; i < NibbleArray::kCount; ++i) {
        plane.set(i, 15);
    }
    CHECK(!plane.isUniform());

    plane.compact();
    CHECK(plane.isUniform());
    CHECK_EQ(plane.uniformValue(), u8(15));
    CHECK_EQ(plane.memoryUsage(), usize(0));
    CHECK_EQ(plane.get(4095), u8(15));

    // One dissenting nibble keeps the array.
    plane.set(0, 3);
    plane.compact();
    CHECK(!plane.isUniform());
    CHECK_EQ(plane.get(0), u8(3));
    CHECK_EQ(plane.get(1), u8(15));
}

TEST(nibble_array_packs_even_indices_low)
{
    // Minecraft's packing, and the reason our arrays go to disk with memcpy.
    NibbleArray plane(0);
    plane.set(0, 0x3);
    plane.set(1, 0xA);

    std::vector<u8> out(NibbleArray::kBytes);
    plane.writeTo(out.data());
    CHECK_EQ(out[0], u8(0xA3));
    CHECK_EQ(out[1], u8(0x00));
}

TEST(nibble_array_assign_collapses_a_uniform_plane)
{
    std::vector<u8> packed(NibbleArray::kBytes, 0xFF);

    NibbleArray plane;
    CHECK(plane.assign(packed));
    CHECK(plane.isUniform());
    CHECK_EQ(plane.uniformValue(), u8(15));
    CHECK_EQ(plane.memoryUsage(), usize(0));

    // A single differing nibble is enough to keep the array.
    packed[7] = 0xF0;
    CHECK(plane.assign(packed));
    CHECK(!plane.isUniform());
    CHECK_EQ(plane.get(14), u8(0));
    CHECK_EQ(plane.get(15), u8(15));

    std::vector<u8> tooShort(NibbleArray::kBytes - 1, 0);
    CHECK(!plane.assign(tooShort));
}

TEST(chunk_column_maps_y_to_the_right_section)
{
    ChunkColumn column(3, -7);
    CHECK_EQ(column.x, 3);
    CHECK_EQ(column.z, -7);

    for (int y = 0; y < ChunkColumn::kHeight; ++y) {
        column.setBlock(2, y, 5, BlockId(1 + (y % 200)));
    }
    for (int y = 0; y < ChunkColumn::kHeight; ++y) {
        CHECK_EQ(column.block(2, y, 5), BlockId(1 + (y % 200)));
    }

    // The section the block landed in must agree with the column view.
    const int y = 100;
    CHECK_EQ(column.section(y / Section::kSize).block(2, y % Section::kSize, 5),
             BlockId(1 + (y % 200)));

    // Out-of-range y is air, not a crash: physics and meshing both probe past
    // the world's top and bottom.
    CHECK_EQ(column.block(2, -1, 5), BlockId(0));
    CHECK_EQ(column.block(2, ChunkColumn::kHeight, 5), BlockId(0));
    column.setBlock(2, -1, 5, 1);
    column.setBlock(2, ChunkColumn::kHeight, 5, 1);
}

TEST(chunk_column_nibble_planes_are_addressed_independently)
{
    ChunkColumn column;
    column.setBlockData(1, 70, 2, 12);
    column.setBlockLight(1, 70, 2, 7);
    column.setSkyLight(1, 70, 2, 15);

    CHECK_EQ(column.blockData(1, 70, 2), u8(12));
    CHECK_EQ(column.blockLight(1, 70, 2), u8(7));
    CHECK_EQ(column.skyLight(1, 70, 2), u8(15));
    CHECK_EQ(column.blockData(1, 71, 2), u8(0));
    CHECK_EQ(column.skyLight(1, 69, 2), u8(0));
}

TEST(an_empty_column_costs_almost_nothing)
{
    // The whole point of the section split: at render distance 8 there are 289
    // of these, and most of their sections are air.
    ChunkColumn column;
    const usize empty = column.memoryUsage();
    CHECK(empty < 2048);

    for (int y = 0; y < 64; ++y) {
        column.setBlock(0, y, 0, 1);
    }
    CHECK(column.memoryUsage() > empty);
}
