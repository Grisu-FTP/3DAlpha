// Scans terrain atlas tiles for texels the alpha test would drop. See tile_alpha.hpp.

#include "core/texture/tile_alpha.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/texture/atlas_image.hpp"

namespace mc::texture {

bool tileHasHoles(const u8* atlasRgba, int tile)
{
    if (tile < 0 || tile >= mesh::kAtlasTileCount) {
        return false;
    }
    const int x0 = (tile % kAtlasTilesPerEdge) * kAtlasTilePixels;
    const int y0 = (tile / kAtlasTilesPerEdge) * kAtlasTilePixels;
    for (int y = 0; y < kAtlasTilePixels; ++y) {
        const u8* row = atlasRgba + (usize(y0 + y) * kAtlasEdge + usize(x0)) * 4;
        for (int x = 0; x < kAtlasTilePixels; ++x) {
            if (row[usize(x) * 4 + 3] <= kAlphaTestDropsAtOrBelow) {
                return true;
            }
        }
    }
    return false;
}

bool solidCubeTilesHaveHoles(const u8* atlasRgba)
{
    for (int slot = 0; slot < mesh::kCubeAtlas.slotCount; ++slot) {
        if (!mesh::cubeSlotIsCutout(slot)
            && tileHasHoles(atlasRgba, mesh::kCubeAtlas.tileOfSlot[slot])) {
            return true;
        }
    }
    return false;
}

bool snowLayerHasHoles(const u8* atlasRgba)
{
    return tileHasHoles(atlasRgba, mcver::kBlocks[int(mcver::Block::SnowLayer)].texture);
}

}  // namespace mc::texture
