#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/fluid.hpp"
#include "core/mesh/mesher.hpp"

#include <cmath>
#include <set>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::DetailVertex;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using world::ChunkColumn;

namespace {

constexpr u16 kAir = u16(mcver::Block::Air);
constexpr u16 kStone = u16(mcver::Block::Stone);
constexpr u16 kGlass = u16(mcver::Block::Glass);
constexpr u16 kIce = u16(mcver::Block::Ice);
constexpr u16 kWater = u16(mcver::Block::Water);
constexpr u16 kFlowingWater = u16(mcver::Block::FlowingWater);
constexpr u16 kLava = u16(mcver::Block::Lava);

const u8 kWaterMaterial = block::def(kWater).material;

// 23 KB, so the cases share one rather than putting it on the stack each time.
MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

// A column with the given blocks placed, filled into the scratch around
// section 0. Water is placed at (8, 8, 8) by every case that needs a subject,
// well away from the section's own edges so the shell is never the thing under
// test.
constexpr int kX = 8;
constexpr int kY = 8;
constexpr int kZ = 8;

struct Placement {
    int dx, dy, dz;
    u16 id;
    u8 data;
};

MeshScratch& worldOf(std::initializer_list<Placement> blocks)
{
    static ChunkColumn column;
    column = ChunkColumn();
    for (const Placement& p : blocks) {
        column.setBlock(kX + p.dx, kY + p.dy, kZ + p.dz, p.id);
        column.setBlockData(kX + p.dx, kY + p.dy, kZ + p.dz, p.data);
    }
    scratch().fill(ColumnNeighbourhood::isolated(column), 0);
    return scratch();
}

MeshBuilder meshOne(MeshScratch& s)
{
    MeshBuilder out;
    mesh::addFluid(s, kX, kY, kZ, block::def(s.block(kX, kY, kZ)), out);
    return out;
}

// Water lands in the translucent stream and lava in the opaque one, so a test
// that hardcoded either would pass for the wrong reason on the other. These
// take the whole of whichever stream the subject block went to, and assert that
// the other is empty -- which makes every case below also a check that the
// routing happened.
usize quadsOf(const MeshBuilder& m, bool translucent)
{
    return translucent ? m.translucentQuadCount() : m.detailQuadCount();
}

const DetailVertex* quad(const MeshBuilder& m, usize q, bool translucent)
{
    return (translucent ? m.translucentVertices() : m.detailVertices()) + q * 4;
}

// The stream the block at the test position belongs in.
bool subjectIsTranslucent(const MeshScratch& s)
{
    return block::def(s.block(kX, kY, kZ)).translucent;
}

usize fluidQuadCount(const MeshBuilder& m, const MeshScratch& s)
{
    return quadsOf(m, subjectIsTranslucent(s));
}

// Nothing may leak into the stream the subject does not belong to, and nothing
// at all into the 12-byte cube stream.
bool onlyItsOwnStream(const MeshBuilder& m, const MeshScratch& s)
{
    return quadsOf(m, !subjectIsTranslucent(s)) == 0 && m.quadCount() == 0;
}

std::set<int> facesEmitted(const MeshBuilder& m, const MeshScratch& s)
{
    const bool translucent = subjectIsTranslucent(s);
    std::set<int> faces;
    for (usize q = 0; q < quadsOf(m, translucent); ++q) {
        faces.insert(quad(m, q, translucent)->face);
    }
    return faces;
}

bool near(float a, float b) { return std::fabs(a - b) < 1.0e-5f; }

}  // namespace

// ---------------------------------------------------------------------------
// Surface height, `jp.b(I)F`
// ---------------------------------------------------------------------------

TEST(fluid_height_is_in_ninths_not_eighths)
{
    // The single most misremembered constant in the whole render type: a
    // source block's surface sits at 1 - 1/9, not 1 - 1/8.
    CHECK(near(mesh::fluidHeightPercent(0), 1.0f / 9.0f));
    CHECK(near(mesh::fluidHeightPercent(7), 8.0f / 9.0f));

    // 8..15 are the falling flag, and read back as a source.
    CHECK(near(mesh::fluidHeightPercent(8), 1.0f / 9.0f));
    CHECK(near(mesh::fluidHeightPercent(15), 1.0f / 9.0f));
}

// ---------------------------------------------------------------------------
// Corner heights, `bc.a(int,int,int,gb)`
// ---------------------------------------------------------------------------

TEST(a_lake_surface_is_flat_at_eight_ninths)
{
    // Every cell around the corner is a source, so each contributes its 1/9
    // eleven times over and the average is exactly 1/9.
    MeshScratch& s = worldOf({
        {0, 0, 0, kWater, 0},  {-1, 0, 0, kWater, 0}, {0, 0, -1, kWater, 0},
        {-1, 0, -1, kWater, 0}, {1, 0, 0, kWater, 0}, {0, 0, 1, kWater, 0},
        {1, 0, 1, kWater, 0},  {-1, 0, 1, kWater, 0}, {1, 0, -1, kWater, 0},
    });

    const mesh::FluidCorners corners = mesh::fluidCorners(s, kX, kY, kZ, kWaterMaterial);
    for (int c = 0; c < 4; ++c) {
        CHECK(near(corners.h[c], 8.0f / 9.0f));
    }
}

TEST(an_isolated_source_sags_at_every_corner)
{
    // One source in open air. Each corner samples the block itself (11 counts
    // of 1/9) and three empty cells (1 count of 1.0 each), so
    // h = 1 - (11/9 + 3) / 14.
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}});

    const mesh::FluidCorners corners = mesh::fluidCorners(s, kX, kY, kZ, kWaterMaterial);
    const float expected = 1.0f - (11.0f / 9.0f + 3.0f) / 14.0f;
    for (int c = 0; c < 4; ++c) {
        CHECK(near(corners.h[c], expected));
    }
}

TEST(a_solid_neighbour_leaves_the_corner_alone_and_a_hollow_one_pulls_it_down)
{
    // The corner at (x, z) samples the four cells with that corner in common:
    // the block itself and the three at -X, -Z and both. Filling all three with
    // stone leaves the block's own 11 counts of 1/9 as the whole average.
    MeshScratch& walled = worldOf({
        {0, 0, 0, kWater, 0},
        {-1, 0, 0, kStone, 0},
        {0, 0, -1, kStone, 0},
        {-1, 0, -1, kStone, 0},
    });
    CHECK(near(mesh::fluidCorners(walled, kX, kY, kZ, kWaterMaterial).h[0], 8.0f / 9.0f));

    // Glass is solid too -- it is not opaque and not a fluid, but Material
    // says solid, which is exactly the case a rule guessed from `opaque` gets
    // wrong.
    MeshScratch& glazed = worldOf({
        {0, 0, 0, kWater, 0},
        {-1, 0, 0, kGlass, 0},
        {0, 0, -1, kGlass, 0},
        {-1, 0, -1, kGlass, 0},
    });
    CHECK(near(mesh::fluidCorners(glazed, kX, kY, kZ, kWaterMaterial).h[0], 8.0f / 9.0f));
}

TEST(fluid_above_pins_the_corner_to_the_top_of_the_block)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}, {0, 1, 0, kFlowingWater, 1}});

    // Only the corners whose four cells include the column above are pinned,
    // and the block above sits over all four of this block's corners.
    const mesh::FluidCorners corners = mesh::fluidCorners(s, kX, kY, kZ, kWaterMaterial);
    for (int c = 0; c < 4; ++c) {
        CHECK(near(corners.h[c], 1.0f));
    }
}

TEST(a_flowing_cell_lowers_the_corner_it_touches)
{
    // A source with one level-7 cell diagonally behind the (x,z) corner. That
    // cell counts once, at 8/9; the far corners never see it.
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}, {-1, 0, -1, kFlowingWater, 7}});

    const mesh::FluidCorners corners = mesh::fluidCorners(s, kX, kY, kZ, kWaterMaterial);

    // Corner 0 = (x,z): the source (11 counts of 1/9), the level-7 cell (1 of
    // 8/9) and two empty cells (1 of 1.0 each). The flowing cell is heavier
    // than the empty ones it replaced, so the corner rides higher than the
    // untouched ones -- 0.706 against 0.698.
    const float touched = 1.0f - (11.0f / 9.0f + 8.0f / 9.0f + 2.0f) / 14.0f;
    CHECK(near(corners.h[0], touched));

    // Corner 2 = (x+1,z+1): three empty cells and the source.
    const float untouched = 1.0f - (11.0f / 9.0f + 3.0f) / 14.0f;
    CHECK(near(corners.h[2], untouched));
}

// ---------------------------------------------------------------------------
// Face culling, `jp.c(nm,IIII)`
// ---------------------------------------------------------------------------

TEST(a_fluid_draws_no_face_against_its_own_material)
{
    // Still and flowing water are different blocks and the same material,
    // which is the point of comparing materials rather than ids.
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}, {1, 0, 0, kFlowingWater, 3}});

    CHECK(!mesh::fluidFaceVisible(s, kX + 1, kY, kZ, mesh::kFacePosX, kWaterMaterial));

    // Lava is a fluid and a different material, so the face survives.
    MeshScratch& mixed = worldOf({{0, 0, 0, kWater, 0}, {1, 0, 0, kLava, 0}});
    CHECK(mesh::fluidFaceVisible(mixed, kX + 1, kY, kZ, mesh::kFacePosX, kWaterMaterial));
}

TEST(a_fluid_draws_no_face_against_ice)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}, {1, 0, 0, kIce, 0}});
    CHECK(!mesh::fluidFaceVisible(s, kX + 1, kY, kZ, mesh::kFacePosX, kWaterMaterial));

    // And it is ice specifically, not "any full cube you can see through":
    // glass is the same shape and the same transparency and does not qualify.
    MeshScratch& glazed = worldOf({{0, 0, 0, kWater, 0}, {1, 0, 0, kGlass, 0}});
    CHECK(mesh::fluidFaceVisible(glazed, kX + 1, kY, kZ, mesh::kFacePosX, kWaterMaterial));
}

TEST(a_fluid_side_is_hidden_by_an_opaque_neighbour_only)
{
    MeshScratch& walled = worldOf({{0, 0, 0, kWater, 0}, {1, 0, 0, kStone, 0}});
    CHECK(!mesh::fluidFaceVisible(walled, kX + 1, kY, kZ, mesh::kFacePosX, kWaterMaterial));

    MeshScratch& open = worldOf({{0, 0, 0, kWater, 0}});
    CHECK(mesh::fluidFaceVisible(open, kX + 1, kY, kZ, mesh::kFacePosX, kWaterMaterial));
}

TEST(a_fluid_top_face_is_drawn_even_under_stone)
{
    // The original's quirk, kept: side 1 returns true before the opaque test
    // is ever reached, so water with a stone lid still emits a top face that
    // nothing can see.
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}, {0, 1, 0, kStone, 0}});
    CHECK(mesh::fluidFaceVisible(s, kX, kY + 1, kZ, mesh::kFacePosY, kWaterMaterial));

    const MeshBuilder m = meshOne(s);
    CHECK(facesEmitted(m, s).count(mesh::kFacePosY) == 1);
}

// ---------------------------------------------------------------------------
// Flow direction, `jp.e(nm,III)` and `jp.a(nm,IIILgb;)D`
// ---------------------------------------------------------------------------

TEST(still_water_has_no_flow_direction)
{
    MeshScratch& s = worldOf({
        {0, 0, 0, kWater, 0},  {-1, 0, 0, kWater, 0}, {1, 0, 0, kWater, 0},
        {0, 0, -1, kWater, 0}, {0, 0, 1, kWater, 0},
    });
    CHECK(mesh::fluidFlowAngle(s, kX, kY, kZ, kWaterMaterial) == mesh::kNoFlow);
}

TEST(water_flows_away_from_the_higher_neighbour)
{
    // A source at -X and this cell one level down. The -X neighbour's decay is
    // 0 against our 1, so the weight is -1 and the vector is (+1, 0, 0):
    // pointing away from the source, along +X.
    MeshScratch& s = worldOf({{0, 0, 0, kFlowingWater, 1}, {-1, 0, 0, kWater, 0}});

    const float angle = mesh::fluidFlowAngle(s, kX, kY, kZ, kWaterMaterial);
    CHECK(angle != mesh::kNoFlow);

    // atan2(0, 1) - pi/2.
    CHECK(near(angle, -1.5707963f));
    CHECK(near(std::sin(angle), -1.0f));
}

TEST(water_flows_toward_the_lip_of_a_drop)
{
    // Nothing at +X on this level, but water one step down there. The empty
    // cell is not solid, so the fluid below it pulls: weight is
    // 0 - (1 - 8) = 7, and the vector points at +X.
    MeshScratch& s = worldOf({{0, 0, 0, kFlowingWater, 1}, {1, -1, 0, kFlowingWater, 0}});

    const float angle = mesh::fluidFlowAngle(s, kX, kY, kZ, kWaterMaterial);
    CHECK(angle != mesh::kNoFlow);
    CHECK(near(angle, -1.5707963f));
}

TEST(a_solid_neighbour_blocks_the_pull_from_below)
{
    // The same arrangement with the gap plugged. blocksMovement is true, so
    // the cell below is never consulted and the fluid has no direction at all.
    MeshScratch& s = worldOf({
        {0, 0, 0, kFlowingWater, 1},
        {1, 0, 0, kStone, 0},
        {1, -1, 0, kFlowingWater, 0},
    });
    CHECK(mesh::fluidFlowAngle(s, kX, kY, kZ, kWaterMaterial) == mesh::kNoFlow);
}

TEST(a_falling_column_with_open_sides_points_straight_down)
{
    // Metadata 8 is the falling flag. With air around it the vertical term
    // dominates by six to one, the horizontal components normalise away to
    // zero, and the texture stops spinning.
    MeshScratch& s = worldOf({{0, 0, 0, kFlowingWater, 8}, {0, 1, 0, kWater, 0}});
    CHECK(mesh::fluidFlowAngle(s, kX, kY, kZ, kWaterMaterial) == mesh::kNoFlow);
}

// ---------------------------------------------------------------------------
// The emitter
// ---------------------------------------------------------------------------

TEST(an_isolated_source_emits_all_six_faces_into_one_detail_stream)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}});
    const MeshBuilder m = meshOne(s);

    // Nothing in the 12-byte cube stream and nothing in the opaque detail one:
    // a fluid is never a cube, and water is translucent.
    CHECK(onlyItsOwnStream(m, s));
    CHECK_EQ(m.translucentQuadCount(), usize(6));

    CHECK_EQ(facesEmitted(m, s).size(), usize(6));
}

TEST(water_goes_to_the_translucent_stream_and_lava_to_the_opaque_one)
{
    // a1.1.2's own getRenderBlockPass, which the two fluids answer differently
    // despite sharing a class, a render type and every line of the emitter.
    MeshScratch& water = worldOf({{0, 0, 0, kWater, 0}});
    const MeshBuilder wet = meshOne(water);
    CHECK_EQ(wet.translucentQuadCount(), usize(6));
    CHECK_EQ(wet.detailQuadCount(), usize(0));

    MeshScratch& lava = worldOf({{0, 0, 0, kLava, 0}});
    const MeshBuilder hot = meshOne(lava);
    CHECK_EQ(hot.detailQuadCount(), usize(6));
    CHECK_EQ(hot.translucentQuadCount(), usize(0));
}

TEST(a_buried_fluid_emits_nothing_at_all)
{
    MeshScratch& s = worldOf({
        {0, 0, 0, kWater, 0},  {1, 0, 0, kWater, 0},  {-1, 0, 0, kWater, 0},
        {0, 1, 0, kWater, 0},  {0, -1, 0, kWater, 0}, {0, 0, 1, kWater, 0},
        {0, 0, -1, kWater, 0},
    });
    CHECK(meshOne(s).empty());
}

TEST(the_top_face_sits_at_the_corner_heights_and_the_bottom_at_the_block_base)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}});
    const MeshBuilder m = meshOne(s);

    const float sag = 1.0f - (11.0f / 9.0f + 3.0f) / 14.0f;
    const i16 expected = mesh::detailPos(kY, sag);

    int tops = 0;
    int bottoms = 0;
    for (usize q = 0; q < fluidQuadCount(m, s); ++q) {
        const DetailVertex* v = quad(m, q, subjectIsTranslucent(s));
        if (v->face == mesh::kFacePosY) {
            ++tops;
            for (int c = 0; c < 4; ++c) {
                CHECK_EQ(v[c].y, expected);
            }
        }
        if (v->face == mesh::kFaceNegY) {
            ++bottoms;
            for (int c = 0; c < 4; ++c) {
                CHECK_EQ(v[c].y, i16(kY * mesh::kDetailUnitsPerBlock));
            }
        }
    }
    CHECK_EQ(tops, 1);
    CHECK_EQ(bottoms, 1);
}

TEST(a_side_face_runs_from_the_surface_down_to_the_block_base)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}});
    const MeshBuilder m = meshOne(s);

    const float sag = 1.0f - (11.0f / 9.0f + 3.0f) / 14.0f;
    const i16 top = mesh::detailPos(kY, sag);
    const i16 base = i16(kY * mesh::kDetailUnitsPerBlock);

    for (usize q = 0; q < fluidQuadCount(m, s); ++q) {
        const DetailVertex* v = quad(m, q, subjectIsTranslucent(s));
        if (v->face < mesh::kFaceNegZ) {
            continue;
        }
        // The transcription's order: surface, surface, base, base.
        CHECK_EQ(v[0].y, top);
        CHECK_EQ(v[1].y, top);
        CHECK_EQ(v[2].y, base);
        CHECK_EQ(v[3].y, base);
    }
}

TEST(a_still_top_face_reads_exactly_its_own_tile)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}});
    const MeshBuilder m = meshOne(s);

    const int tile = block::def(kWater).faces[mesh::kFacePosY];
    const i16 u0 = i16((tile % mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile);
    const i16 v0 = i16((tile / mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile);
    const i16 u1 = i16(u0 + mesh::kUvUnitsPerTile);
    const i16 v1 = i16(v0 + mesh::kUvUnitsPerTile);

    for (usize q = 0; q < fluidQuadCount(m, s); ++q) {
        const DetailVertex* v = quad(m, q, subjectIsTranslucent(s));
        if (v->face != mesh::kFacePosY) {
            continue;
        }
        for (int c = 0; c < 4; ++c) {
            CHECK(v[c].u == u0 || v[c].u == u1);
            CHECK(v[c].v == v0 || v[c].v == v1);
        }
    }
}

TEST(a_flowing_top_face_reads_across_the_tile_boundary_by_design)
{
    // The rotated sample square is centred on the flowing tile's far corner,
    // so it straddles the 2x2 that a1.1.2's terrain.png reserves for the same
    // fluid -- 206, 207, 222 and 223 for water. Transcribed, not corrected.
    MeshScratch& s = worldOf({{0, 0, 0, kFlowingWater, 1}, {-1, 0, 0, kWater, 0}});
    const MeshBuilder m = meshOne(s);

    const int tile = block::def(kFlowingWater).faces[mesh::kFaceNegZ];
    const i16 uFar = i16(((tile % mesh::kAtlasTilesPerEdge) + 1) * mesh::kUvUnitsPerTile);

    bool sawBeyond = false;
    for (usize q = 0; q < fluidQuadCount(m, s); ++q) {
        const DetailVertex* v = quad(m, q, subjectIsTranslucent(s));
        if (v->face != mesh::kFacePosY) {
            continue;
        }
        for (int c = 0; c < 4; ++c) {
            if (v[c].u > uFar || v[c].v > uFar) {
                sawBeyond = true;
            }
        }
    }
    CHECK(sawBeyond);
}

TEST(fluid_light_is_the_brighter_of_a_cell_and_the_one_above_it)
{
    // `jp.c(nm,III)F` overrides getBlockBrightness with max(here, above), and
    // every face in the fluid renderer reads through it. Sky and block light
    // are taken separately, which is exactly equivalent: a lightmap texel is a
    // monotone function of both.
    static ChunkColumn column;
    column = ChunkColumn();
    column.setBlock(kX, kY, kZ, kWater);
    column.setSkyLight(kX, kY, kZ, 7);       // the fluid's own cell
    column.setSkyLight(kX, kY - 1, kZ, 3);   // below it: darker than the fluid
    column.setSkyLight(kX + 1, kY, kZ, 11);  // +X
    column.setBlockLight(kX + 1, kY + 1, kZ, 5);  // above +X: a torch out of frame
    scratch().fill(ColumnNeighbourhood::isolated(column), 0);

    const MeshBuilder m = meshOne(scratch());
    int checked = 0;
    for (usize q = 0; q < fluidQuadCount(m, scratch()); ++q) {
        const DetailVertex* v = quad(m, q, subjectIsTranslucent(scratch()));
        if (v->face == mesh::kFacePosY) {
            // Nothing above, so the fluid's own cell wins.
            CHECK_EQ(int(v->light), 7 << 4);
            ++checked;
        }
        if (v->face == mesh::kFaceNegY) {
            // The cell below is at 3 and the fluid above it at 7. Under the
            // plain cube rule this face would be lit at 3.
            CHECK_EQ(int(v->light), 7 << 4);
            ++checked;
        }
        if (v->face == mesh::kFacePosX) {
            // Sky from the neighbour, block light from the cell above it.
            CHECK_EQ(int(v->light), (11 << 4) | 5);
            ++checked;
        }
    }
    CHECK_EQ(checked, 3);
}

TEST(fluid_faces_carry_the_same_directional_shade_as_a_cube)
{
    MeshScratch& s = worldOf({{0, 0, 0, kWater, 0}});
    const MeshBuilder m = meshOne(s);

    for (usize q = 0; q < fluidQuadCount(m, s); ++q) {
        const DetailVertex* v = quad(m, q, subjectIsTranslucent(s));
        CHECK_EQ(int(v->r), int(mesh::kFaceShade[v->face]));
        CHECK_EQ(int(v->r), int(v->g));
        CHECK_EQ(int(v->r), int(v->b));
    }
}

TEST(the_mesher_routes_fluid_through_the_detail_stream)
{
    // The same block through meshSection rather than addFluid directly, so the
    // dispatch is pinned too.
    static ChunkColumn column;
    column = ChunkColumn();
    column.setBlock(kX, kY, kZ, kWater);
    column.setBlock(kX, kY, kZ + 2, kStone);
    scratch().fill(ColumnNeighbourhood::isolated(column), 0);

    MeshBuilder out;
    mesh::meshSection(scratch(), out);

    CHECK_EQ(out.quadCount(), usize(6));             // the stone cube
    CHECK_EQ(out.translucentQuadCount(), usize(6));  // the water
    CHECK_EQ(out.detailQuadCount(), usize(0));
}

TEST(unknown_blocks_and_air_are_never_this_fluids_material)
{
    // Air's material index is 0, which no constructed block takes, and an id
    // outside the table comes back as the unknown block with the same 0. Both
    // have to leave a fluid's faces alone.
    CHECK_EQ(int(block::def(kAir).material), 0);
    CHECK_EQ(int(block::def(60000).material), 0);
    CHECK(kWaterMaterial != 0);
}
