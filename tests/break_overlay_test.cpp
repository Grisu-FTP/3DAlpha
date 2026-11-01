// The crack drawn over a block being broken: its shape, its tile, and the
// growth that stands in for the polygon offset the 3DS does not have.

#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/render/break_overlay.hpp"
#include "framework.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

#include <vector>

using namespace mc;
using mcver::Block;

namespace {

std::vector<mesh::DetailVertex> buffer()
{
    return std::vector<mesh::DetailVertex>(render::kBreakOverlayMaxVertices);
}

double blocks(i16 units) { return double(units) / double(mesh::kDetailUnitsPerBlock); }

}  // namespace

TEST(a_full_cube_cracks_on_all_six_faces)
{
    auto v = buffer();
    CHECK_EQ(render::buildBreakOverlay(block::BlockId(Block::Stone), 0, 3, v.data(),
                                       render::kBreakOverlayMaxVertices),
             24);
}

TEST(every_face_samples_the_stage_tile_in_the_last_row)
{
    auto v = buffer();
    const int stage = 7;
    const int n = render::buildBreakOverlay(block::BlockId(Block::Dirt), 0, stage, v.data(),
                                            render::kBreakOverlayMaxVertices);
    const int tile = render::kBreakOverlayFirstTile + stage;
    const int u0 = (tile % mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile;
    const int v0 = (tile / mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile;
    for (int i = 0; i < n; ++i) {
        CHECK(int(v[i].u) >= u0 && int(v[i].u) <= u0 + mesh::kUvUnitsPerTile);
        CHECK(int(v[i].v) >= v0 && int(v[i].v) <= v0 + mesh::kUvUnitsPerTile);
    }
}

TEST(the_overlay_stands_just_off_the_block_it_covers)
{
    auto v = buffer();
    const int n = render::buildBreakOverlay(block::BlockId(Block::Stone), 0, 0, v.data(),
                                            render::kBreakOverlayMaxVertices);
    for (int q = 0; q < n; q += 4) {
        // Each face is lifted along its normal only, so its edges stay on the
        // block and the crack never overlaps a neighbour.
        int lifted = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const double c0 = blocks(axis == 0 ? v[q].x : axis == 1 ? v[q].y : v[q].z);
            if (c0 < -0.001 || c0 > 1.001) {
                ++lifted;
                CHECK(c0 > -0.01 && c0 < 1.01);
            } else {
                CHECK(c0 > -0.0001 && c0 < 1.0001 && (c0 < 0.0001 || c0 > 0.9999));
            }
        }
        CHECK_EQ(lifted, 1);
    }
}

TEST(a_small_block_cracks_at_the_tiles_pixel_scale)
{
    auto v = buffer();
    const int n = render::buildBreakOverlay(block::BlockId(Block::StoneButton), 0, 0, v.data(),
                                            render::kBreakOverlayMaxVertices);
    CHECK(n > 0);
    for (int i = 0; i < n; ++i) {
        CHECK(int(v[i].u) >= 0);
    }
    // A button face covers a few texels of the tile, never the whole tile.
    int span = 0;
    for (int i = 1; i < 4; ++i) {
        const int d = int(v[i].u) - int(v[0].u);
        span = (d < 0 ? -d : d) > span ? (d < 0 ? -d : d) : span;
    }
    CHECK(span < mesh::kUvUnitsPerTile);
}

TEST(a_slab_cracks_as_a_slab)
{
    auto v = buffer();
    const int n = render::buildBreakOverlay(block::BlockId(Block::Slab), 0, 0, v.data(),
                                            render::kBreakOverlayMaxVertices);
    CHECK(n > 0);
    double top = 0.0;
    for (int i = 0; i < n; ++i) {
        top = blocks(v[i].y) > top ? blocks(v[i].y) : top;
    }
    CHECK(top < 0.6);
}

TEST(a_cell_already_gone_borrows_stones_cube)
{
    auto v = buffer();
    CHECK_EQ(render::buildBreakOverlay(block::kAir, 0, 5, v.data(),
                                       render::kBreakOverlayMaxVertices),
             24);
}

TEST(a_stage_out_of_range_draws_nothing)
{
    auto v = buffer();
    CHECK_EQ(render::buildBreakOverlay(block::BlockId(Block::Stone), 0, -1, v.data(),
                                       render::kBreakOverlayMaxVertices),
             0);
    CHECK_EQ(render::buildBreakOverlay(block::BlockId(Block::Stone), 0, 10, v.data(),
                                       render::kBreakOverlayMaxVertices),
             0);
}
