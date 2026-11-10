// Finding the tiles a texture pack cut holes in. See core/texture/tile_alpha.hpp.

#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/tile_alpha.hpp"

#include <vector>

using namespace mc;
using texture::kAtlasBytes;
using texture::kAtlasEdge;
using texture::kAtlasTilePixels;
using texture::kAtlasTilesPerEdge;

namespace {

std::vector<u8> opaqueAtlas()
{
    return std::vector<u8>(kAtlasBytes, u8(255));
}

void setAlpha(std::vector<u8>& atlas, int tile, int texelX, int texelY, u8 alpha)
{
    const int x = (tile % kAtlasTilesPerEdge) * kAtlasTilePixels + texelX;
    const int y = (tile / kAtlasTilesPerEdge) * kAtlasTilePixels + texelY;
    atlas[(usize(y) * kAtlasEdge + usize(x)) * 4 + 3] = alpha;
}

constexpr int tileOf(mcver::Block block)
{
    return mcver::kBlocks[int(block)].texture;
}

}  // namespace

TEST(an_opaque_atlas_has_no_holes)
{
    const std::vector<u8> atlas = opaqueAtlas();
    CHECK(!texture::tileHasHoles(atlas.data(), 0));
    CHECK(!texture::solidCubeTilesHaveHoles(atlas.data()));
    CHECK(!texture::snowLayerHasHoles(atlas.data()));
}

// `GL_GREATER 0.1` over eight bits: 25 is dropped and 26 is kept.
TEST(a_hole_is_exactly_what_the_alpha_test_drops)
{
    std::vector<u8> atlas = opaqueAtlas();
    setAlpha(atlas, 37, 15, 15, 26);
    CHECK(!texture::tileHasHoles(atlas.data(), 37));
    setAlpha(atlas, 37, 15, 15, 25);
    CHECK(texture::tileHasHoles(atlas.data(), 37));
    // Only that tile: its neighbours on both axes share rows and columns with it.
    CHECK(!texture::tileHasHoles(atlas.data(), 36));
    CHECK(!texture::tileHasHoles(atlas.data(), 38));
    CHECK(!texture::tileHasHoles(atlas.data(), 21));
    CHECK(!texture::tileHasHoles(atlas.data(), 53));
}

TEST(glass_is_allowed_holes_and_stone_is_not)
{
    std::vector<u8> atlas = opaqueAtlas();
    setAlpha(atlas, tileOf(mcver::Block::Glass), 3, 3, 0);
    setAlpha(atlas, tileOf(mcver::Block::Leaves), 3, 3, 0);
    setAlpha(atlas, tileOf(mcver::Block::MobSpawner), 3, 3, 0);
    CHECK(!texture::solidCubeTilesHaveHoles(atlas.data()));

    setAlpha(atlas, tileOf(mcver::Block::Stone), 3, 3, 0);
    CHECK(texture::solidCubeTilesHaveHoles(atlas.data()));
}

TEST(a_hole_in_the_snow_tile_is_noticed)
{
    std::vector<u8> atlas = opaqueAtlas();
    setAlpha(atlas, tileOf(mcver::Block::SnowLayer), 8, 8, 0);
    CHECK(texture::snowLayerHasHoles(atlas.data()));
}

TEST(the_cube_atlas_marks_exactly_the_cutout_blocks_tiles)
{
    CHECK(mesh::cubeSlotIsCutout(mesh::cubeSlotOf(tileOf(mcver::Block::Glass))));
    CHECK(mesh::cubeSlotIsCutout(mesh::cubeSlotOf(tileOf(mcver::Block::Leaves))));
    CHECK(mesh::cubeSlotIsCutout(mesh::cubeSlotOf(tileOf(mcver::Block::MobSpawner))));
    CHECK(!mesh::cubeSlotIsCutout(mesh::cubeSlotOf(tileOf(mcver::Block::Stone))));
    CHECK(!mesh::cubeSlotIsCutout(mesh::cubeSlotOf(tileOf(mcver::Block::Dirt))));
    // Farmland is not opaque and not a full cube, and shares dirt's sides.
    CHECK(!mesh::cubeSlotIsCutout(mesh::cubeSlotOf(2)));
}
