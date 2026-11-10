// The two parts of the cube range, and the snow layer's hidden floor. See
// mesh::MeshRanges::cutoutCubeBytes and mesh::MeshBuilder::setSnowHidesFloor.

#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/mesher.hpp"
#include "core/world/chunk.hpp"

#include <memory>
#include <vector>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::CubeFormat;
using mesh::MeshBuilder;
using mesh::MeshRanges;
using mesh::MeshScratch;
using world::ChunkColumn;

namespace {

MeshScratch& scratch()
{
    static std::unique_ptr<MeshScratch> s = std::make_unique<MeshScratch>();
    return *s;
}

constexpr u16 kStone = u16(mcver::Block::Stone);
constexpr u16 kGlass = u16(mcver::Block::Glass);
constexpr u16 kLeaves = u16(mcver::Block::Leaves);
constexpr u16 kSpawner = u16(mcver::Block::MobSpawner);
constexpr u16 kSnow = u16(mcver::Block::SnowLayer);
constexpr u16 kSlab = u16(mcver::Block::Slab);

MeshBuilder meshOf(const ChunkColumn& column, int sectionY, bool greedy = true,
                   CubeFormat format = CubeFormat::Vertices, bool snowHidesFloor = true)
{
    scratch().fill(ColumnNeighbourhood::isolated(column), sectionY);
    MeshBuilder out;
    out.setCubeFormat(format);
    out.setGreedy(greedy);
    out.setSnowHidesFloor(snowHidesFloor);
    mesh::meshSection(scratch(), out);
    return out;
}

// Whether a quad samples a cutout slot, read back out of what was written.
bool quadIsCutout(const MeshBuilder& m, usize quad)
{
    int slotX = 0;
    int slotY = 0;
    if (m.cubeFormat() == CubeFormat::Quads) {
        slotX = m.quads()[quad].slotX;
        slotY = m.quads()[quad].slotY;
    } else {
        // The corner nearest the atlas origin is inside the slot whichever way
        // the quad is wound.
        const mesh::WorldVertex* v = m.vertices() + quad * 4;
        i16 u = v[0].u;
        i16 w = v[0].v;
        for (int c = 1; c < 4; ++c) {
            u = v[c].u < u ? v[c].u : u;
            w = v[c].v < w ? v[c].v : w;
        }
        slotX = u / mesh::kCubeUvPerSlot;
        slotY = w / mesh::kCubeUvPerSlot;
    }
    return mesh::cubeSlotIsCutout(slotY * mesh::kCubeSlotsPerEdge + slotX);
}

ChunkColumn mixedColumn()
{
    ChunkColumn column;
    column.setBlock(1, 1, 1, kStone);
    column.setBlock(5, 5, 5, kGlass);
    column.setBlock(9, 9, 9, kStone);
    column.setBlock(12, 3, 12, kLeaves);
    column.setBlock(3, 12, 3, kSpawner);
    return column;
}

}  // namespace

// Solid first, cutout after, in every way a section can be meshed.
TEST(cutout_cube_faces_come_after_every_solid_one)
{
    for (const bool greedy : {true, false}) {
        for (const CubeFormat format : {CubeFormat::Vertices, CubeFormat::Quads}) {
            const MeshBuilder m = meshOf(mixedColumn(), 0, greedy, format);
            const MeshRanges ranges = m.ranges();

            CHECK_EQ(m.quadCount(), usize(30));  // five lone blocks
            const usize bytesPerQuad = mesh::cubeBytesPerQuad(format);
            CHECK_EQ(ranges.cutoutCubeBytes, usize(18) * bytesPerQuad);
            CHECK_EQ(ranges.solidCubeBytes(), usize(12) * bytesPerQuad);
            CHECK_EQ(ranges.solidCubeBytes() + ranges.cutoutCubeBytes, ranges.cubeBytes);

            const usize firstCutout = ranges.solidCubeBytes() / bytesPerQuad;
            for (usize q = 0; q < m.quadCount(); ++q) {
                CHECK_EQ(quadIsCutout(m, q), q >= firstCutout);
            }
        }
    }
}

TEST(a_section_of_solid_blocks_has_no_cutout_part)
{
    ChunkColumn column;
    column.setBlock(2, 2, 2, kStone);
    column.setBlock(10, 10, 10, kStone);
    const MeshRanges ranges = meshOf(column, 0).ranges();
    CHECK_EQ(ranges.cutoutCubeBytes, usize(0));
    CHECK_EQ(ranges.solidCubeBytes(), ranges.cubeBytes);
}

TEST(a_section_of_only_glass_is_all_cutout)
{
    ChunkColumn column;
    column.setBlock(2, 2, 2, kGlass);
    const MeshRanges ranges = meshOf(column, 0).ranges();
    CHECK_EQ(ranges.solidCubeBytes(), usize(0));
    CHECK_EQ(ranges.cutoutCubeBytes, ranges.cubeBytes);
}

// Glass faces merge with glass faces as before; the split must not cost a merge.
TEST(cutout_faces_still_merge)
{
    ChunkColumn column;
    for (int x = 0; x < 3; ++x) {
        for (int z = 0; z < 3; ++z) {
            column.setBlock(x, 4, z, kGlass);
        }
    }
    const MeshBuilder merged = meshOf(column, 0, true);
    const MeshBuilder flat = meshOf(column, 0, false);
    CHECK(merged.quadCount() < flat.quadCount());
    CHECK_EQ(merged.ranges().cutoutCubeBytes, merged.ranges().cubeBytes);
    CHECK_EQ(flat.ranges().cutoutCubeBytes, flat.ranges().cubeBytes);
}

// A builder reused for the next section starts with an empty cutout part.
TEST(clearing_a_builder_forgets_its_cutout_part)
{
    ChunkColumn glass;
    glass.setBlock(2, 2, 2, kGlass);
    ChunkColumn stone;
    stone.setBlock(2, 2, 2, kStone);

    MeshBuilder reused;
    scratch().fill(ColumnNeighbourhood::isolated(glass), 0);
    mesh::meshSection(scratch(), reused);
    CHECK(reused.ranges().cutoutCubeBytes != 0);

    reused.clear();
    scratch().fill(ColumnNeighbourhood::isolated(stone), 0);
    mesh::meshSection(scratch(), reused);
    CHECK_EQ(reused.ranges().cutoutCubeBytes, usize(0));
}

TEST(a_snow_layer_hides_the_top_of_the_block_under_it)
{
    ChunkColumn column;
    column.setBlock(4, 4, 4, kStone);
    column.setBlock(4, 5, 4, kSnow);
    CHECK_EQ(meshOf(column, 0).quadCount(), usize(5));
    CHECK_EQ(meshOf(column, 0, true, CubeFormat::Quads).quadCount(), usize(5));
}

// The face and the snow on it are in different sections here, which is what the
// scratch's padding has to answer for.
TEST(a_snow_layer_hides_a_top_across_a_section_boundary)
{
    ChunkColumn column;
    column.setBlock(4, 15, 4, kStone);
    column.setBlock(4, 16, 4, kSnow);
    CHECK_EQ(meshOf(column, 0).quadCount(), usize(5));
}

// A pack whose snow has holes gets the face back.
TEST(a_snow_tile_with_holes_keeps_the_top)
{
    ChunkColumn column;
    column.setBlock(4, 4, 4, kStone);
    column.setBlock(4, 5, 4, kSnow);
    CHECK_EQ(meshOf(column, 0, true, CubeFormat::Vertices, false).quadCount(), usize(6));
}

// Only snow, and only the face under it.
TEST(a_slab_above_still_leaves_the_top_to_the_block_rule)
{
    ChunkColumn column;
    column.setBlock(4, 4, 4, kStone);
    column.setBlock(4, 5, 4, kSlab);
    CHECK_EQ(meshOf(column, 0).quadCount(), usize(6));

    ChunkColumn beside;
    beside.setBlock(4, 4, 4, kStone);
    beside.setBlock(5, 4, 4, kSnow);
    CHECK_EQ(meshOf(beside, 0).quadCount(), usize(6));
}
