// The sub-block box emitter, and the blocks that stopped being full cubes.
//
// **This is the fix for "a slab and a double slab both place a double slab".**
// They were placing different blocks all along; the mesher drew both as a unit
// cube because the cube vertex formats store whole-block corners and cannot say
// anything else. Five of a1.1.2's blocks render through the standard renderer
// with bounds that are not the unit cube -- the slab, the snow layer, both
// pressure plates and the button -- and every one of them was a full block.
//
// The load-bearing assertion here is the first one: a full cube put through the
// box path comes out in the same place, with the same UVs, as one put through
// the cube path. That is what says the new geometry agrees with the geometry
// the console has been drawing since M0, rather than being a second opinion.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/mesher.hpp"
#include "core/util/aabb.hpp"
#include "framework.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::DetailVertex;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using world::ChunkColumn;

namespace {

MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

MeshBuilder meshOf(const ChunkColumn& column, int sectionY)
{
    scratch().fill(ColumnNeighbourhood::isolated(column), sectionY);
    MeshBuilder out;
    mesh::meshSection(scratch(), out);
    return out;
}

// A detail vertex's position as a fraction of a block, relative to block
// (bx, by, bz).
double localAxis(i16 stored, int block)
{
    return double(stored - block * mesh::kDetailUnitsPerBlock)
           / double(mesh::kDetailUnitsPerBlock);
}

constexpr u16 kStone = u16(mcver::Block::Stone);
constexpr u16 kSlab = u16(mcver::Block::Slab);
constexpr u16 kDoubleSlab = u16(mcver::Block::DoubleSlab);

}  // namespace

TEST(a_full_cube_through_the_box_path_matches_the_cube_path)
{
    // The same block, the same tiles, the same light, both ways round. The cube
    // path stores block coordinates and the box path stores 1/1024 of a block,
    // so the comparison is on the values they mean rather than on the bytes.
    const block::BlockDef& def = block::def(block::BlockId(kStone));
    const u8 light = (12 << 4) | 3;

    MeshBuilder cubes;
    for (int face = 0; face < mesh::kFaceCount; ++face) {
        cubes.addQuad(2, 3, 4, face, def.faces[face], light);
    }

    MeshBuilder boxes;
    mesh::addBox(2, 3, 4, AABB{0.0, 0.0, 0.0, 1.0, 1.0, 1.0}, def.faces, light, true,
                 mesh::kAllBoxFaces, boxes);

    CHECK_EQ(cubes.quadCount(), usize(6));
    CHECK_EQ(boxes.detailQuadCount(), usize(6));

    for (usize v = 0; v < 24; ++v) {
        const mesh::WorldVertex& a = cubes.vertices()[v];
        const DetailVertex& b = boxes.detailVertices()[v];
        const int face = mesh::faceOf(a);
        CHECK_EQ(int(b.face), face);
        CHECK_EQ(int(b.light), int(a.light));

        // **The same corner of the same tile, in two different atlases.** The
        // cube path samples the cube atlas (core/mesh/cube_atlas.hpp) and the
        // box path terrain.png, so the numbers differ; what has to agree is
        // which end of the tile each corner is at. A cube UV at its run's
        // start is the tile's near edge, anything else its far one.
        const int tile = def.faces[face];
        const bool uNear = a.u == mesh::cubeUvStart(a.u / mesh::kCubeUvPerSlot);
        const bool vNear = a.v == mesh::cubeUvStart(a.v / mesh::kCubeUvPerSlot);
        CHECK_EQ(int(b.u), uNear ? mesh::tileUvMin(tile % mesh::kAtlasTilesPerEdge)
                                 : mesh::tileUvMax(tile % mesh::kAtlasTilesPerEdge));
        CHECK_EQ(int(b.v), vNear ? mesh::tileUvMin(tile / mesh::kAtlasTilesPerEdge)
                                 : mesh::tileUvMax(tile / mesh::kAtlasTilesPerEdge));
        CHECK_EQ(int(b.r), int(a.r));
        // The cube path's position is a block index; the box path's is the same
        // corner in 1/1024ths.
        CHECK_EQ(int(b.x), int(a.x) * mesh::kDetailUnitsPerBlock);
        CHECK_EQ(int(b.y), int(a.y) * mesh::kDetailUnitsPerBlock);
        CHECK_EQ(int(b.z), int(a.z) * mesh::kDetailUnitsPerBlock);
    }
}

TEST(a_slab_is_half_a_block_tall_and_a_double_slab_is_not)
{
    // The bug, as two meshes. A slab has to leave the cube stream entirely and
    // reach only halfway up its cell; a double slab has to stay a cube.
    ChunkColumn slab;
    slab.setBlock(1, 4, 1, kSlab);
    const MeshBuilder slabMesh = meshOf(slab, 0);

    ChunkColumn doubled;
    doubled.setBlock(1, 4, 1, kDoubleSlab);
    const MeshBuilder doubleMesh = meshOf(doubled, 0);

    CHECK_EQ(slabMesh.quadCount(), usize(0));
    CHECK_EQ(slabMesh.detailQuadCount(), usize(6));
    CHECK_EQ(doubleMesh.quadCount(), usize(6));
    CHECK_EQ(doubleMesh.detailQuadCount(), usize(0));

    double highest = 0.0;
    for (usize v = 0; v < slabMesh.detailQuadCount() * 4; ++v) {
        const double y = localAxis(slabMesh.detailVertices()[v].y, 4);
        CHECK(y >= -1e-9);
        CHECK(y <= 0.5 + 1e-9);
        highest = y > highest ? y : highest;
    }
    // ...and it really does reach the half, rather than being a flat sheet.
    CHECK(std::fabs(highest - 0.5) < 1e-9);
}

TEST(a_slabs_side_samples_the_top_half_of_its_tile)
{
    // `bc.c` takes a side face's v from the box's **y** range, low end first,
    // so the top edge of the box lands on tile row minY*16. For a slab that is
    // rows 0..8 -- the top half -- and it is the reason an upside-down slab's
    // texture looks shifted in every version of the game.
    ChunkColumn column;
    column.setBlock(1, 4, 1, kSlab);
    const MeshBuilder m = meshOf(column, 0);

    const block::BlockDef& def = block::def(block::BlockId(kSlab));
    const int tile = def.faces[mesh::kFaceNegZ];
    const int tileTop = (tile / mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile;
    const int halfway = tileTop + mesh::kUvUnitsPerTile / 2;

    bool sawSide = false;
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        const DetailVertex* quad = m.detailVertices() + q * 4;
        if (quad[0].face != mesh::kFaceNegZ) {
            continue;
        }
        sawSide = true;
        for (int c = 0; c < 4; ++c) {
            // Never past the middle of the tile, which is what "the top half"
            // means. The inset makes the two ends a few units short of exact.
            CHECK(quad[c].v >= tileTop);
            CHECK(quad[c].v <= halfway);
        }
    }
    CHECK(sawSide);
}

TEST(a_slab_keeps_its_top_face_and_loses_its_bottom_one)
{
    // The face-culling rule the box path adds: a face only counts as hidden if
    // the box actually reaches that side of the cell. A slab sitting on stone
    // has its underside covered and its top four-tenths of a block clear of
    // anything, so the top must survive whatever is above.
    ChunkColumn column;
    column.setBlock(1, 3, 1, kStone);
    column.setBlock(1, 4, 1, kSlab);
    column.setBlock(1, 5, 1, kStone);
    const MeshBuilder m = meshOf(column, 0);

    bool sawTop = false;
    bool sawBottom = false;
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        const int face = m.detailVertices()[q * 4].face;
        sawTop = sawTop || face == mesh::kFacePosY;
        sawBottom = sawBottom || face == mesh::kFaceNegY;
    }
    CHECK(sawTop);
    CHECK(!sawBottom);
}

TEST(every_standard_block_that_is_not_a_unit_cube_leaves_the_cube_stream)
{
    // Derived rather than listed: whichever blocks the selection table says are
    // rendered by the standard renderer with reduced bounds must all take the
    // box path. If a version adds one, this covers it without an edit.
    int checked = 0;
    for (int id = 1; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (!def.known || def.render != block::RenderType::Cube) {
            continue;
        }
        const AABB bounds = block::selectionBox(block::BlockId(id), 0);
        const bool unit = bounds.minX == 0.0 && bounds.minY == 0.0 && bounds.minZ == 0.0
                          && bounds.maxX == 1.0 && bounds.maxY == 1.0 && bounds.maxZ == 1.0;
        if (unit) {
            continue;
        }
        ChunkColumn column;
        column.setBlock(1, 4, 1, u16(id));
        const MeshBuilder m = meshOf(column, 0);
        CHECK_EQ(m.quadCount(), usize(0));
        CHECK(m.detailQuadCount() > 0);
        ++checked;
    }
    // a1.1.2 has five of them; a version with none would make this test a
    // no-op that still passed, which is the failure mode worth catching.
    CHECK(checked > 0);
}
