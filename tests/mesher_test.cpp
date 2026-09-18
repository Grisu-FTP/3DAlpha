#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/mesher.hpp"

#include <cstring>
#include <set>
#include <vector>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using mesh::WorldVertex;
using world::ChunkColumn;
using world::Section;

namespace {

// Meshing needs a scratch and a builder every time; these are big enough that
// the tests share one of each rather than putting 17 KB on the stack per case.
MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

// **One quad per face, unless a test asks otherwise.** Most of this file is
// about which faces exist -- culling, winding, light, tiles -- and those are
// questions about faces, which greedy meshing only regroups. greedy_test.cpp
// checks that the regrouping draws exactly these faces.
MeshBuilder meshOf(const ChunkColumn& column, int sectionY, bool greedy = false)
{
    scratch().fill(ColumnNeighbourhood::isolated(column), sectionY);
    MeshBuilder out;
    out.setGreedy(greedy);
    mesh::meshSection(scratch(), out);
    return out;
}

// The face a quad belongs to, recovered from its vertices, so the tests check
// what was written to the buffer rather than what the mesher was asked to write.
int quadFace(const MeshBuilder& m, usize quad)
{
    return mesh::faceOf(m.vertices()[quad * 4]);
}

// A single face's UV range in the cube atlas, for the tile it shows. The cube
// pass samples the cube atlas and not terrain.png; see core/mesh/cube_atlas.hpp.
struct CubeUv {
    int u0, u1, v0, v1;
};

CubeUv cubeUvOf(int tile)
{
    const int slot = mesh::cubeSlotOf(tile);
    const int sx = slot % mesh::kCubeSlotsPerEdge;
    const int sy = slot / mesh::kCubeSlotsPerEdge;
    return {mesh::cubeUvStart(sx), mesh::cubeUvEnd(sx, 1), mesh::cubeUvStart(sy),
            mesh::cubeUvEnd(sy, 1)};
}

std::set<int> facesEmitted(const MeshBuilder& m)
{
    std::set<int> faces;
    for (usize q = 0; q < m.quadCount(); ++q) {
        faces.insert(quadFace(m, q));
    }
    return faces;
}

constexpr u16 kStone = u16(mcver::Block::Stone);
constexpr u16 kGlass = u16(mcver::Block::Glass);
constexpr u16 kGrass = u16(mcver::Block::Grass);

}  // namespace

TEST(an_empty_section_produces_no_geometry)
{
    ChunkColumn column;
    CHECK(mesh::sectionIsEmpty(column, 0));

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(0));
    CHECK(m.empty());
}

TEST(a_lone_block_shows_all_six_faces_once)
{
    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(6));
    CHECK_EQ(m.vertexCount(), usize(24));

    // Every face exactly once, and no face twice.
    const std::set<int> faces = facesEmitted(m);
    CHECK_EQ(faces.size(), usize(6));

    // 4 vertices per quad and no index data at all is the whole reason the
    // format is worth its awkwardness -- 48 bytes a quad, not 96.
    CHECK_EQ(m.byteSize(), m.quadCount() * 4 * sizeof(WorldVertex));
    CHECK_EQ(m.byteSize(), usize(6 * 48));
}

TEST(touching_faces_between_two_opaque_blocks_are_both_dropped)
{
    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);
    column.setBlock(5, 6, 5, kStone);

    const MeshBuilder m = meshOf(column, 0);

    // Twelve faces minus the shared pair: the lower block's top and the upper
    // block's bottom. Emitting either would be an invisible quad drawn forever.
    CHECK_EQ(m.quadCount(), usize(10));

    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        const int face = mesh::faceOf(v[0]);
        // No quad may sit on the plane y = 6, which is the shared boundary.
        const bool onSeam = (face == mesh::kFacePosY && v[0].y == 6)
                            || (face == mesh::kFaceNegY && v[0].y == 6);
        CHECK(!onSeam);
    }
}

TEST(a_solid_section_is_hollow_from_the_inside)
{
    // Fill the whole section. Only the 6 outer walls can be seen, and each is
    // 16x16 quads. An interior face here would be 6144 wasted quads.
    ChunkColumn column;
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                column.setBlock(x, y, z, kStone);
            }
        }
    }

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(6 * 16 * 16));
}

TEST(glass_hides_the_face_it_shares_with_glass_and_culls_nothing_else)
{
    // **`fc.c(Lnm;IIII)Z` -- BlockBreakable.shouldSideBeRendered**, which this
    // used to assert the opposite of: "alpha has no glass panes and no face
    // merging between glass blocks, so two adjacent glass blocks draw all
    // twelve faces". The class file says otherwise. Glass is constructed with
    // its `a` flag false, and the override is
    // `if (!a && world.getBlockId(i,j,k) == blockID) return false;` before the
    // base call -- so the shared pane is not drawn, and a wall of glass is a
    // window rather than a stack of boxes. Reported from play.
    ChunkColumn column;
    column.setBlock(5, 5, 5, kGlass);
    column.setBlock(5, 6, 5, kGlass);

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(10));

    // Stone under glass is the asymmetric case, and the count is the proof:
    // the stone keeps its top face because you can see it through the glass,
    // while the glass loses its bottom face because the stone hides it. Six
    // and five, not six and six -- culling asks about the neighbour, never
    // about the pair.
    ChunkColumn mixed;
    mixed.setBlock(5, 5, 5, kStone);
    mixed.setBlock(5, 6, 5, kGlass);

    const MeshBuilder m2 = meshOf(mixed, 0);
    CHECK_EQ(m2.quadCount(), usize(11));

    int glassBottom = 0;
    int stoneTop = 0;
    for (usize q = 0; q < m2.quadCount(); ++q) {
        const WorldVertex* v = m2.vertices() + q * 4;
        if (mesh::faceOf(v[0]) == mesh::kFaceNegY && v[0].y == 6) ++glassBottom;
        if (mesh::faceOf(v[0]) == mesh::kFacePosY && v[0].y == 6) ++stoneTop;
    }
    CHECK_EQ(glassBottom, 0);
    CHECK_EQ(stoneTop, 1);
}

TEST(the_checkerboard_is_the_worst_case_the_index_buffer_is_sized_for)
{
    // This is the arrangement that exposes the most faces: half the cells
    // solid, none of them touching. If this ever exceeds kMaxQuadsPerSection
    // the shared index buffer is too small and chunk draws start truncating.
    ChunkColumn column;
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                if (((x + y + z) & 1) == 0) {
                    column.setBlock(x, y, z, kStone);
                }
            }
        }
    }

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(mesh::kMaxQuadsPerSection));
    CHECK_EQ(m.vertexCount(), usize(mesh::kMaxVerticesPerSection));
}

TEST(faces_are_wound_counter_clockwise_seen_from_outside)
{
    // A quad wound the wrong way is culled by the GPU and the block loses that
    // one side. Checking it here means never diagnosing it on a 240-line
    // screen: the cross product of the first two edges must point the way the
    // face does.
    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(6));

    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        const int face = mesh::faceOf(v[0]);

        const int ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, az = v[1].z - v[0].z;
        const int bx = v[2].x - v[0].x, by = v[2].y - v[0].y, bz = v[2].z - v[0].z;

        const int nx = ay * bz - az * by;
        const int ny = az * bx - ax * bz;
        const int nz = ax * by - ay * bx;

        const mesh::FaceOffset& expected = mesh::kFaceOffset[face];
        CHECK_EQ(nx, int(expected.dx));
        CHECK_EQ(ny, int(expected.dy));
        CHECK_EQ(nz, int(expected.dz));
    }
}

TEST(the_four_corners_of_a_quad_are_distinct_and_coplanar)
{
    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);

    const MeshBuilder m = meshOf(column, 0);
    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        const int face = mesh::faceOf(v[0]);

        std::set<int> corners;
        for (int c = 0; c < 4; ++c) {
            corners.insert(v[c].x * 1024 + v[c].y * 32 + v[c].z);
        }
        CHECK_EQ(corners.size(), usize(4));

        // All four share the coordinate the face is perpendicular to.
        const mesh::FaceOffset& offset = mesh::kFaceOffset[face];
        for (int c = 1; c < 4; ++c) {
            if (offset.dx != 0) CHECK_EQ(v[c].x, v[0].x);
            if (offset.dy != 0) CHECK_EQ(v[c].y, v[0].y);
            if (offset.dz != 0) CHECK_EQ(v[c].z, v[0].z);
        }
    }
}

TEST(face_shade_is_what_the_original_applies)
{
    // Read out of the a1.1.2 jar rather than remembered: RenderBlocks loads
    // 0.5, 1.0, 0.8, 0.6 and gives them to bottom, top, the Z faces and the X
    // faces in that order.
    CHECK_EQ(mesh::kFaceShade[mesh::kFaceNegY], u8(128));  // 0.5
    CHECK_EQ(mesh::kFaceShade[mesh::kFacePosY], u8(255));  // 1.0
    CHECK_EQ(mesh::kFaceShade[mesh::kFaceNegZ], u8(204));  // 0.8
    CHECK_EQ(mesh::kFaceShade[mesh::kFacePosZ], u8(204));
    CHECK_EQ(mesh::kFaceShade[mesh::kFaceNegX], u8(153));  // 0.6
    CHECK_EQ(mesh::kFaceShade[mesh::kFacePosX], u8(153));

    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);
    const MeshBuilder m = meshOf(column, 0);

    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        const u8 want = mesh::kFaceShade[mesh::faceOf(v[0])];
        for (int c = 0; c < 4; ++c) {
            CHECK_EQ(v[c].r, want);
            CHECK_EQ(v[c].g, want);
            CHECK_EQ(v[c].b, want);
        }
    }
}

TEST(uvs_land_on_the_blocks_atlas_tile)
{
    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);

    const MeshBuilder m = meshOf(column, 0);
    const int tile = block::def(kStone).texture;
    // The tile's range in the cube atlas, inset off the slot boundary at both
    // ends -- see kCubeUvInset.
    const CubeUv range = cubeUvOf(tile);
    const int u0 = range.u0;
    const int u1 = range.u1;
    const int v0 = range.v0;
    const int v1 = range.v1;

    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        std::set<int> us, vs;
        for (int c = 0; c < 4; ++c) {
            CHECK(v[c].u == u0 || v[c].u == u1);
            CHECK(v[c].v == v0 || v[c].v == v1);
            us.insert(v[c].u);
            vs.insert(v[c].v);
        }
        // Both edges of the tile appear, so the quad covers it rather than
        // collapsing to a line of texels.
        CHECK_EQ(us.size(), usize(2));
        CHECK_EQ(vs.size(), usize(2));
    }

    // The far edge of the last tile must still fit a signed short -- this is
    // why UVs are 1/16384 units and not 1/32768.
    CHECK_EQ(16 * mesh::kUvUnitsPerTile, mesh::kUvUnitsPerAtlas);
    CHECK_EQ(mesh::kCubeSlotsPerEdge * mesh::kCubeUvPerSlot, mesh::kUvUnitsPerAtlas);
    CHECK(mesh::kUvUnitsPerAtlas <= 32767);
}

TEST(each_face_gets_its_own_tile_not_the_blocks_default)
{
    // The whole point of the per-face table: a grass block is dirt underneath,
    // grass on top and the grass-side tile around the middle. Before this,
    // every face took the constructor texture and the world was carpeted in
    // grass-side.
    ChunkColumn column;
    column.setBlock(5, 5, 5, kGrass);

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(6));

    const block::BlockDef& def = block::def(kGrass);
    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        const CubeUv range = cubeUvOf(def.faces[quadFace(m, q)]);
        for (int c = 0; c < 4; ++c) {
            CHECK(v[c].u == range.u0 || v[c].u == range.u1);
            CHECK(v[c].v == range.v0 || v[c].v == range.v1);
        }
    }

    // Stated outright rather than only through the table, so that a
    // regenerated blocks.json cannot quietly move them together.
    CHECK(def.faces[mesh::kFacePosY] != def.faces[mesh::kFaceNegY]);
    CHECK(def.faces[mesh::kFacePosY] != def.faces[mesh::kFaceNegX]);
}

TEST(a_furnace_is_drawn_with_its_mouth_where_its_metadata_says)
{
    // Every furnace in the world used to have its mouth on +Z, because the
    // cube stream draws `faces` -- the inventory answer -- and never looked at
    // the metadata the placement wrote. It is drawn through the out-of-line
    // path now, and still as six ordinary cube quads.
    const u16 furnace = u16(mcver::Block::Furnace);
    const block::BlockDef& def = block::def(furnace);
    const CubeUv mouth = cubeUvOf(def.faces[mesh::kFacePosZ]);
    for (int md = 2; md <= 5; ++md) {
        ChunkColumn column;
        column.setBlock(5, 5, 5, furnace);
        column.setBlockData(5, 5, 5, u8(md));

        const MeshBuilder m = meshOf(column, 0);
        CHECK_EQ(m.quadCount(), usize(6));
        int mouths = 0;
        for (usize q = 0; q < m.quadCount(); ++q) {
            const WorldVertex* v = m.vertices() + q * 4;
            const bool isMouth = v[0].u == mouth.u0 || v[0].u == mouth.u1;
            const bool isMouthRow = v[0].v == mouth.v0 || v[0].v == mouth.v1;
            if (isMouth && isMouthRow) {
                ++mouths;
                CHECK_EQ(quadFace(m, q), md);
            }
        }
        CHECK_EQ(mouths, 1);
    }
}

TEST(light_is_sampled_from_the_cell_the_face_looks_into)
{
    // A block is always dark inside; the level that matters is the one in the
    // air against each face. Sampling the block itself would make every
    // surface in the world pitch black.
    ChunkColumn column;
    column.setBlock(5, 5, 5, kStone);
    column.setSkyLight(5, 6, 5, 15);   // above
    column.setBlockLight(5, 4, 5, 7);  // below

    const MeshBuilder m = meshOf(column, 0);
    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        if (mesh::faceOf(v[0]) == mesh::kFacePosY) {
            CHECK_EQ(v[0].light, u8(15 << 4));
        } else if (mesh::faceOf(v[0]) == mesh::kFaceNegY) {
            CHECK_EQ(v[0].light, u8(7));
        } else {
            CHECK_EQ(v[0].light, u8(0));
        }
    }
}

TEST(sky_light_above_the_world_is_full_not_missing)
{
    // The top section's ceiling faces look at cells outside the column.
    // ChunkColumn reports those as unlit, which is right for physics and would
    // put a black lid on any world whose terrain reaches the build limit.
    ChunkColumn column;
    const int top = ChunkColumn::kHeight - 1;
    column.setBlock(3, top, 3, kStone);

    const MeshBuilder m = meshOf(column, ChunkColumn::kSectionCount - 1);

    bool sawTop = false;
    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        if (mesh::faceOf(v[0]) == mesh::kFacePosY) {
            CHECK_EQ(v[0].light, u8(15 << 4));
            sawTop = true;
        }
    }
    CHECK(sawTop);
}

TEST(section_boundaries_within_a_column_cull_against_each_other)
{
    // The block below sits in section 0 and the block above in section 1. If
    // the scratch did not reach past the section edge, both would keep the face
    // they share and the world would be full of horizontal seams every 16
    // blocks -- which is exactly the bug the 18^3 window exists to prevent.
    ChunkColumn column;
    column.setBlock(5, 15, 5, kStone);
    column.setBlock(5, 16, 5, kStone);

    CHECK_EQ(meshOf(column, 0).quadCount(), usize(5));
    CHECK_EQ(meshOf(column, 1).quadCount(), usize(5));
}

TEST(a_neighbouring_column_culls_across_the_chunk_border)
{
    ChunkColumn centre(0, 0);
    ChunkColumn east(1, 0);
    centre.setBlock(15, 5, 5, kStone);
    east.setBlock(0, 5, 5, kStone);

    // Meshed alone, the block at x=15 keeps its +X face.
    CHECK_EQ(meshOf(centre, 0).quadCount(), usize(6));

    // With the neighbour in place it is culled, and so is the neighbour's.
    ColumnNeighbourhood n = ColumnNeighbourhood::isolated(centre);
    n.at(1, 0) = &east;

    scratch().fill(n, 0);
    MeshBuilder m;
    mesh::meshSection(scratch(), m);
    CHECK_EQ(m.quadCount(), usize(5));

    for (usize q = 0; q < m.quadCount(); ++q) {
        CHECK(quadFace(m, q) != mesh::kFacePosX);
    }
}

TEST(a_missing_neighbour_reads_as_air_rather_than_crashing)
{
    // Null columns are the normal state at the edge of the loaded world. They
    // must be traversable, not a null dereference on a worker thread.
    ChunkColumn centre(0, 0);
    centre.setBlock(0, 5, 0, kStone);
    centre.setBlock(15, 5, 15, kStone);

    ColumnNeighbourhood n;  // every slot null, including the centre
    scratch().fill(n, 0);
    MeshBuilder empty;
    mesh::meshSection(scratch(), empty);
    CHECK_EQ(empty.quadCount(), usize(0));

    const MeshBuilder m = meshOf(centre, 0);
    CHECK_EQ(m.quadCount(), usize(12));
}

TEST(non_cube_render_types_are_left_to_their_own_emitters)
{
    // Nothing that is not a cube may be drawn as one. A missing torch is an
    // obvious gap; a cubic one looks like a deliberate decision.
    ChunkColumn column;
    column.setBlock(1, 1, 1, u16(mcver::Block::Dandelion));
    column.setBlock(3, 1, 1, u16(mcver::Block::Torch));
    column.setBlock(5, 1, 1, u16(mcver::Block::Water));
    column.setBlock(7, 1, 1, u16(mcver::Block::Ladder));

    const MeshBuilder mesh = meshOf(column, 0);
    CHECK_EQ(mesh.quadCount(), usize(0));

    // Three of the four have emitters now, and they land in two different
    // detail streams: four quads for the cross and five for the torch in the
    // opaque one, six faces of an isolated water source in the translucent
    // one. The ladder has no emitter yet and contributes nothing at all --
    // which is the half of this test that has to keep holding as the remaining
    // render types are filled in.
    CHECK_EQ(mesh.detailQuadCount(), usize(4 + 5));
    CHECK_EQ(mesh.translucentQuadCount(), usize(6));
}

TEST(has_emitter_agrees_with_what_the_dispatch_actually_reaches)
{
    // "Implemented" and "reached" are different claims, and the gap between
    // them is invisible: a render type whose emitter exists but whose case is
    // missing from meshSection's switch simply produces nothing, exactly like
    // one that was never written. This meshes one block of every render type
    // the version defines and checks both directions.
    for (int type = 0; type < int(block::RenderType::Count); ++type) {
        const block::RenderType render = block::RenderType(type);

        // The version's first block of this render type, if it has one.
        int id = -1;
        for (int candidate = 1; candidate < mcver::kBlockTableSize; ++candidate) {
            if (mcver::kBlocks[candidate].known && mcver::kBlocks[candidate].render == render) {
                id = candidate;
                break;
            }
        }
        if (id < 0) {
            continue;
        }

        // **Every metadata, not just zero.** A shape can legitimately draw
        // nothing for a metadata the game never writes: a ladder's renderer is
        // an if-chain over 2..5 and falls off the end for anything else, which
        // means the original draws no ladder either. So "has an emitter" is
        // "draws something for at least one state", and "has none" stays
        // "draws nothing, ever".
        usize best = 0;
        for (u8 metadata = 0; metadata < 16; ++metadata) {
            // Alone in air, so nothing is culled and any emitter at all shows
            // up.
            //
            // **Except fire, which needs something to burn on.** `bc.d`
            // branches on what is under and around the cell, and a flame with
            // nothing solid below it and nothing flammable beside it draws
            // zero quads in the original too -- fire in mid-air with nothing
            // to burn goes out on the next tick and is never seen. So this one
            // gets a floor, which is the state a fire is actually in.
            ChunkColumn column;
            column.setBlock(8, 8, 8, u16(id));
            column.setBlockData(8, 8, 8, metadata);
            if (render == block::RenderType::Fire) {
                column.setBlock(8, 7, 8, u16(mcver::Block::Stone));
            }

            const MeshBuilder m = meshOf(column, 0);
            const usize quads = m.quadCount() + m.detailQuadCount() + m.translucentQuadCount();
            if (!mesh::hasEmitter(render)) {
                CHECK_EQ(quads, usize(0));
            }
            best = quads > best ? quads : best;
        }
        if (mesh::hasEmitter(render)) {
            CHECK(best > 0);
        }
    }
}

TEST(a_cross_is_two_planes_drawn_from_both_sides)
{
    ChunkColumn column;
    column.setBlock(2, 3, 4, u16(mcver::Block::Rose));

    const MeshBuilder mesh = meshOf(column, 0);

    // Two diagonals, each emitted in both windings, because the original has no
    // back-face culling to satisfy and a flower is visible from behind.
    CHECK_EQ(mesh.detailQuadCount(), usize(4));
    CHECK_EQ(mesh.quadCount(), usize(0));

    // Every vertex stays inside the block it belongs to, in detail units.
    const mesh::DetailVertex* v = mesh.detailVertices();
    const int lo = 2 * mesh::kDetailUnitsPerBlock;
    const int hi = 3 * mesh::kDetailUnitsPerBlock;
    for (usize i = 0; i < mesh.detailVertexCount(); ++i) {
        CHECK(v[i].x >= lo && v[i].x <= hi);
        CHECK(v[i].z >= 4 * mesh::kDetailUnitsPerBlock
              && v[i].z <= 5 * mesh::kDetailUnitsPerBlock);
        CHECK(v[i].y >= 3 * mesh::kDetailUnitsPerBlock
              && v[i].y <= 4 * mesh::kDetailUnitsPerBlock);
    }
}

// The inset is 0.45 from the centre, which is 0.05 from each edge -- read out of
// a1.1.2's RenderBlocks, where the constant is the double nearest 0.45. At
// 1/1024 of a block that is 51 units, and getting it wrong by a whole block
// would make flowers span the corners instead of the diagonals.
TEST(a_cross_is_inset_the_way_the_original_insets_it)
{
    ChunkColumn column;
    column.setBlock(0, 0, 0, u16(mcver::Block::Rose));

    const MeshBuilder mesh = meshOf(column, 0);
    const mesh::DetailVertex* v = mesh.detailVertices();

    int minX = 1 << 20;
    int maxX = -1;
    for (usize i = 0; i < mesh.detailVertexCount(); ++i) {
        minX = v[i].x < minX ? v[i].x : minX;
        maxX = v[i].x > maxX ? v[i].x : maxX;
    }

    // 0.05 and 0.95 of a block, to the nearest 1/1024.
    CHECK_EQ(minX, 51);
    CHECK_EQ(maxX, 973);
}

// The UVs have to track the corners, and nothing checked that until a real
// terrain.png made it visible: every flower, sapling and mushroom stood on its
// side. The three cases above pass either way -- they check where the corners
// are, how far they are inset and how bright they are, and a mirrored texture
// moves none of those.
//
// The invariant is the one a cross cannot violate: the tile's v runs downward,
// so a vertex at the top of the block must carry the tile's top v and a vertex
// at the bottom must carry its bottom v. Swapping a pair puts the tile's top
// edge on a corner at the block's floor, which is exactly what a transposed UV
// table does.
TEST(a_cross_maps_the_tile_the_same_way_up_as_the_block)
{
    ChunkColumn column;
    column.setBlock(2, 3, 4, u16(mcver::Block::Dandelion));

    const MeshBuilder mesh = meshOf(column, 0);
    const mesh::DetailVertex* v = mesh.detailVertices();
    CHECK_EQ(mesh.detailQuadCount(), usize(4));

    // Dandelion is tile 13: row 0, so the tile's top v is 0 and its bottom v is
    // one tile down. Named rather than derived, so a wrong tile fails here too.
    const int tile = 13;
    const i16 tileU0 = mesh::tileUvMin(tile % mesh::kAtlasTilesPerEdge);
    const i16 tileV0 = mesh::tileUvMin(tile / mesh::kAtlasTilesPerEdge);
    const i16 tileU1 = mesh::tileUvMax(tile % mesh::kAtlasTilesPerEdge);
    const i16 tileV1 = mesh::tileUvMax(tile / mesh::kAtlasTilesPerEdge);

    const int yBottom = 3 * mesh::kDetailUnitsPerBlock;
    const int yTop = 4 * mesh::kDetailUnitsPerBlock;

    for (usize i = 0; i < mesh.detailVertexCount(); ++i) {
        // Only the tile's own four corners are ever emitted.
        CHECK(v[i].u == tileU0 || v[i].u == tileU1);
        CHECK(v[i].v == tileV0 || v[i].v == tileV1);

        // And the one that matters: top of the block, top of the tile.
        if (v[i].y == yTop) {
            CHECK_EQ(v[i].v, tileV0);
        } else {
            CHECK_EQ(int(v[i].y), yBottom);
            CHECK_EQ(v[i].v, tileV1);
        }
    }

    // The two vertices of a vertical edge share a u, or the tile is sheared
    // rather than merely flipped. Each quad is top, bottom, bottom, top.
    for (usize q = 0; q < mesh.detailQuadCount(); ++q) {
        const mesh::DetailVertex* c = v + q * 4;
        CHECK_EQ(c[0].u, c[1].u);
        CHECK_EQ(c[2].u, c[3].u);
        CHECK(c[0].u != c[2].u);
    }
}

TEST(a_cross_is_unshaded_and_lit_from_its_own_cell)
{
    ChunkColumn column;
    column.setBlock(2, 2, 2, u16(mcver::Block::Sapling));
    column.setSkyLight(2, 2, 2, 11);
    column.setBlockLight(2, 2, 2, 3);

    const MeshBuilder mesh = meshOf(column, 0);
    const mesh::DetailVertex* v = mesh.detailVertices();
    CHECK(mesh.detailVertexCount() > 0);

    for (usize i = 0; i < mesh.detailVertexCount(); ++i) {
        // The original calls setColorOpaque_F once with the block's brightness
        // and never consults the per-face shade table, so all four sides of a
        // flower are equally bright.
        CHECK_EQ(v[i].r, u8(255));
        CHECK_EQ(v[i].g, u8(255));
        CHECK_EQ(v[i].b, u8(255));
        CHECK_EQ(v[i].light, u8((11 << 4) | 3));
    }
}

TEST(the_three_vertex_streams_concatenate_in_draw_order)
{
    ChunkColumn column;
    column.setBlock(1, 1, 1, u16(mcver::Block::Stone));
    column.setBlock(1, 2, 1, u16(mcver::Block::Dandelion));
    column.setBlock(4, 1, 4, u16(mcver::Block::Water));

    const MeshBuilder mesh = meshOf(column, 0);
    CHECK(mesh.quadCount() > 0);
    CHECK(mesh.detailQuadCount() > 0);
    CHECK(mesh.translucentQuadCount() > 0);

    const mesh::MeshRanges ranges = mesh.ranges();
    CHECK_EQ(mesh.byteSize(), ranges.total());
    CHECK_EQ(ranges.cubeBytes, mesh.vertexCount() * sizeof(mesh::WorldVertex));
    CHECK_EQ(ranges.detailBytes, mesh.detailVertexCount() * sizeof(mesh::DetailVertex));
    CHECK_EQ(ranges.translucentBytes,
             mesh.translucentVertexCount() * sizeof(mesh::DetailVertex));

    std::vector<u8> buffer(mesh.byteSize(), 0xCD);
    mesh.copyTo(buffer.data());

    // Cubes, then opaque detail, then translucent: the renderer walks each
    // range once, changing the shader, the attribute layout and the blend
    // state a fixed number of times per frame rather than once per section.
    CHECK_EQ(std::memcmp(buffer.data(), mesh.vertices(), ranges.cubeBytes), 0);
    CHECK_EQ(std::memcmp(buffer.data() + ranges.detailOffset(), mesh.detailVertices(),
                         ranges.detailBytes),
             0);
    CHECK_EQ(std::memcmp(buffer.data() + ranges.translucentOffset(), mesh.translucentVertices(),
                         ranges.translucentBytes),
             0);
}

TEST(an_unknown_block_is_meshed_as_a_visible_cube)
{
    // A world can legally hold an id this build has never heard of. It has to
    // become geometry rather than a hole or an out-of-range atlas read.
    ChunkColumn column;
    column.setBlock(5, 5, 5, 200);

    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.quadCount(), usize(6));

    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        for (int c = 0; c < 4; ++c) {
            CHECK(v[c].u >= 0 && v[c].u <= mesh::kUvUnitsPerAtlas);
            CHECK(v[c].v >= 0 && v[c].v <= mesh::kUvUnitsPerAtlas);
        }
    }
}

TEST(positions_stay_inside_the_byte_range_the_format_allows)
{
    // Positions are 0..16 inside a section: 17 values, which is exactly why a
    // byte works and why the section origin lives in the MVP matrix instead.
    ChunkColumn column;
    for (int i = 0; i < 16; ++i) {
        column.setBlock(i, i, 15 - i, kStone);
    }

    const MeshBuilder m = meshOf(column, 0);
    CHECK(m.quadCount() > usize(0));
    for (usize i = 0; i < m.vertexCount(); ++i) {
        const WorldVertex& v = m.vertices()[i];
        CHECK(v.x <= 16);
        CHECK(v.y <= 16);
        CHECK(v.z <= 16);
    }
}

// The other two `own_kind` blocks, and they are the same class: `hi` for
// leaves and `fc` for ice, both constructed with `a` false.
TEST(leaves_and_ice_hide_their_own_shared_faces_too)
{
    ChunkColumn leaves;
    leaves.setBlock(5, 5, 5, u16(mcver::Block::Leaves));
    leaves.setBlock(5, 6, 5, u16(mcver::Block::Leaves));
    CHECK_EQ(meshOf(leaves, 0).quadCount(), usize(10));

    // Ice is translucent, so its quads are on the other stream -- the rule is
    // the same one and the count is where it lands that differs.
    ChunkColumn ice;
    ice.setBlock(5, 5, 5, u16(mcver::Block::Ice));
    ice.setBlock(5, 6, 5, u16(mcver::Block::Ice));
    const MeshBuilder m = meshOf(ice, 0);
    CHECK_EQ(m.quadCount() + m.translucentQuadCount(), usize(10));

    // **And a block of a different kind is not hidden**, which is the half of
    // the rule that is easy to lose: the test is on the id, not on the class.
    ChunkColumn mixed;
    mixed.setBlock(5, 5, 5, u16(mcver::Block::Glass));
    mixed.setBlock(5, 6, 5, u16(mcver::Block::Leaves));
    CHECK_EQ(meshOf(mixed, 0).quadCount(), usize(12));
}

// `oi.c(Lnm;IIII)Z` -- the slab's, and the order of its four lines is the
// behaviour: top always, then the opaque test, then bottom always, then the id.
TEST(a_slab_hides_the_side_it_shares_with_a_slab_and_keeps_its_top)
{
    ChunkColumn column;
    column.setBlock(5, 5, 5, u16(mcver::Block::Slab));
    column.setBlock(6, 5, 5, u16(mcver::Block::Slab));

    // Two boxes, six faces each, less the one side they share -- and each keeps
    // its own top and bottom.
    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.detailQuadCount(), usize(10));

    // **Buried in stone, a slab still draws its top and loses its bottom**, and
    // that asymmetry is the order of the method: `if (l == 1) return true` is
    // *before* the base test and `if (l == 0) return true` is after it. So the
    // top ignores the stone above and the bottom does not ignore the stone
    // below. The `l == 0` line is what keeps the underside of a slab stacked on
    // another slab drawn, since the two boxes do not meet.
    ChunkColumn buried;
    for (int x = 4; x <= 6; ++x) {
        for (int z = 4; z <= 6; ++z) {
            buried.setBlock(x, 4, z, kStone);
            buried.setBlock(x, 6, z, kStone);
        }
    }
    buried.setBlock(4, 5, 5, kStone);
    buried.setBlock(6, 5, 5, kStone);
    buried.setBlock(5, 5, 4, kStone);
    buried.setBlock(5, 5, 6, kStone);
    buried.setBlock(5, 5, 5, u16(mcver::Block::Slab));

    int slabTop = 0;
    int slabBottom = 0;
    const MeshBuilder b = meshOf(buried, 0);
    for (usize q = 0; q < b.detailQuadCount(); ++q) {
        const mesh::DetailVertex* v = b.detailVertices() + q * 4;
        if (v->face == mesh::kFacePosY) ++slabTop;
        if (v->face == mesh::kFaceNegY) ++slabBottom;
    }
    CHECK_EQ(slabTop, 1);
    CHECK_EQ(slabBottom, 0);

    // A slab on a slab keeps its underside: the base test passes (a slab is not
    // opaque) and `l == 0` answers before the id test could hide it.
    ChunkColumn stacked;
    stacked.setBlock(5, 5, 5, u16(mcver::Block::Slab));
    stacked.setBlock(5, 6, 5, u16(mcver::Block::Slab));
    int undersides = 0;
    const MeshBuilder t = meshOf(stacked, 0);
    for (usize q = 0; q < t.detailQuadCount(); ++q) {
        if ((t.detailVertices() + q * 4)->face == mesh::kFaceNegY) ++undersides;
    }
    CHECK_EQ(undersides, 2);
}

// **Every sheet is two-sided, and since the opaque detail pass is culled that
// is now load-bearing rather than merely true.**
//
// The renderer draws non-cube opaque geometry under `GPU_CULL_BACK_CCW`, which
// is what stops a door showing its own inside through the window in its top
// tile. Nothing in the mesher may rely on the cull state to be seen from
// behind: `mesh::addSheet` emits the plane and its mirror, and a shape that
// forgot to go through it would vanish from one side with no other symptom.
// This is the assertion that catches that.
TEST(every_sheet_shape_emits_both_windings)
{
    struct Case {
        u16 block;
        u8 metadata;
        int x, y, z;
    };
    const Case cases[] = {
        {u16(mcver::Block::Ladder), 2, 5, 5, 5},
        {u16(mcver::Block::Rail), 0, 5, 5, 5},
        {u16(mcver::Block::Wheat), 7, 5, 5, 5},
        {u16(mcver::Block::Fire), 0, 5, 5, 5},
        {u16(mcver::Block::RedstoneWire), 0, 5, 5, 5},
    };

    for (const Case& c : cases) {
        ChunkColumn column;
        // Something for each of them to stand on or hang from.
        for (int x = 3; x <= 7; ++x) {
            for (int z = 3; z <= 7; ++z) {
                column.setBlock(x, 4, z, kStone);
            }
        }
        column.setBlock(5, 5, 4, kStone);  // the ladder's wall
        column.setBlock(c.x, c.y, c.z, c.block);
        column.setBlockData(c.x, c.y, c.z, c.metadata);

        const MeshBuilder m = meshOf(column, 0);
        const usize quads = m.detailQuadCount();
        CHECK(quads > 0);

        // Each quad must have a partner with its corners in reverse order.
        for (usize q = 0; q < quads; ++q) {
            const mesh::DetailVertex* a = m.detailVertices() + q * 4;
            bool paired = false;
            for (usize r = 0; r < quads && !paired; ++r) {
                if (r == q) {
                    continue;
                }
                const mesh::DetailVertex* b = m.detailVertices() + r * 4;
                bool same = true;
                for (int i = 0; i < 4 && same; ++i) {
                    const mesh::DetailVertex& av = a[i];
                    const mesh::DetailVertex& bv = b[3 - i];
                    same = av.x == bv.x && av.y == bv.y && av.z == bv.z;
                }
                paired = same;
            }
            CHECK(paired);
        }
    }
}
