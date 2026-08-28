#include "core/map/map_palette.hpp"

#include "core/block/registry.hpp"
#include "core/map/map_sample.hpp"
#include "core/mesh/vertex.hpp"

namespace mc::map {

namespace {

// The tile a block shows to something looking straight down. `faces` is filled
// for every block -- one texture repeated when a block has only one -- so this
// never has to fall back to `def.texture`.
u16 topTile(const block::BlockDef& def) { return def.faces[mesh::kFacePosY]; }

}  // namespace

u32 averageTileColour(const texture::AtlasImage& atlas, int tile)
{
    if (atlas.empty() || tile < 0 || tile >= mesh::kAtlasTileCount) {
        return 0;
    }

    const int tx = (tile % texture::kAtlasTilesPerEdge) * texture::kAtlasTilePixels;
    const int ty = (tile / texture::kAtlasTilesPerEdge) * texture::kAtlasTilePixels;

    // Two sums: one weighted by alpha, which is the answer, and one that is
    // not, which is the fallback for a tile that is transparent everywhere and
    // would otherwise divide by zero.
    u32 weighted[3] = {0, 0, 0};
    u32 plain[3] = {0, 0, 0};
    u32 alphaSum = 0;

    for (int y = 0; y < texture::kAtlasTilePixels; ++y) {
        const usize row = usize(ty + y) * texture::kAtlasEdge;
        for (int x = 0; x < texture::kAtlasTilePixels; ++x) {
            const u8* px = &atlas.rgba[(row + usize(tx + x)) * 4];
            const u32 a = px[3];
            for (int c = 0; c < 3; ++c) {
                weighted[c] += u32(px[c]) * a;
                plain[c] += px[c];
            }
            alphaSum += a;
        }
    }

    constexpr u32 kPixels = u32(texture::kAtlasTilePixels) * texture::kAtlasTilePixels;
    u32 out = 0;
    for (int c = 0; c < 3; ++c) {
        const u32 value = alphaSum != 0 ? weighted[c] / alphaSum : plain[c] / kPixels;
        out = (out << 8) | (value > 255 ? 255 : value);
    }
    return out;
}

u32 shadeColour(u32 rgb, int shade)
{
    if (shade < 0 || shade >= kShadeCount) {
        shade = kShadeCount - 1;
    }
    const u32 n = u32(kShadeNumerator[shade]);
    const u32 r = (((rgb >> 16) & 0xFF) * n) / 255;
    const u32 g = (((rgb >> 8) & 0xFF) * n) / 255;
    const u32 b = ((rgb & 0xFF) * n) / 255;
    return (r << 16) | (g << 8) | b;
}

void buildMapPalette(const texture::AtlasImage& atlas, MapPalette* out)
{
    *out = MapPalette{};
    if (atlas.empty()) {
        return;
    }

    for (int id = 0; id < kPaletteSize; ++id) {
        const block::BlockId blockId = block::BlockId(id);
        if (!showsOnMap(blockId)) {
            continue;
        }

        const u32 rgb = averageTileColour(atlas, topTile(block::def(blockId)));
        out->base[id] = rgb;
        out->known[id] = true;
        out->water[id] = isMapWater(blockId);
        for (int shade = 0; shade < kShadeCount; ++shade) {
            const u32 shaded = shadeColour(rgb, shade);
            out->shaded[shade][id] =
                rgb565(int((shaded >> 16) & 0xFF), int((shaded >> 8) & 0xFF), int(shaded & 0xFF));
        }
    }
}

}  // namespace mc::map
