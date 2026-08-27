#include "core/texture/background.hpp"

#include "core/block/registry.hpp"
#include "core/texture/png.hpp"

#include <cstring>

namespace mc::texture {

namespace {

constexpr char kDirtName[] = "dirt.png";

// The atlas tile the fallback cuts out. Looked up rather than written down: the
// block table is generated from the version's own blocks.json, so a build of a
// version whose dirt sits elsewhere in terrain.png follows it.
int dirtTile()
{
    return int(block::def(block::BlockId(mcver::Block::Dirt)).texture);
}

// The pack's dirt tile out of the finished atlas, as an Image scaleSquare can
// take. The atlas is always kAtlasEdge and always 16 tiles across, so this is a
// straight cut with no scaling in it.
bool cutDirtFromAtlas(const AtlasImage& atlas, Image* out)
{
    if (atlas.empty()) {
        return false;
    }
    const int tile = dirtTile();
    const int x0 = (tile % kAtlasTilesPerEdge) * kAtlasTilePixels;
    const int y0 = (tile / kAtlasTilesPerEdge) * kAtlasTilePixels;

    out->width = kAtlasTilePixels;
    out->height = kAtlasTilePixels;
    out->rgba.assign(usize(kAtlasTilePixels) * kAtlasTilePixels * 4, 0);
    for (int y = 0; y < kAtlasTilePixels; ++y) {
        const u8* src = atlas.rgba.data() + (usize(y0 + y) * kAtlasEdge + usize(x0)) * 4;
        std::memcpy(out->rgba.data() + usize(y) * kAtlasTilePixels * 4, src,
                    usize(kAtlasTilePixels) * 4);
    }
    return true;
}

// 0x404040, multiplied rather than interpolated. Alpha is left alone: the
// original multiplies only the colour, and dirt is opaque anyway.
void darken(std::vector<u8>* rgba)
{
    for (usize i = 0; i + 3 < rgba->size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            (*rgba)[i + usize(c)] = u8((u32((*rgba)[i + usize(c)]) * kBackgroundShade) / 0xFF);
        }
    }
}

}  // namespace

PackError buildBackground(io::FileSystem& fs, std::string_view packPath,
                          const AtlasImage& atlas, std::vector<u8>* out)
{
    out->clear();

    Image source;

    std::vector<u8> png;
    if (readPackFile(fs, packPath, kDirtName, &png) == PackError::Ok) {
        // A dirt.png that will not decode, is not square, or is enormous is not
        // worth refusing the whole menu over -- the atlas below draws the same
        // block. The pack screen has already said whether the pack is sound.
        Image decoded;
        if (decodePng(png, &decoded, kMaxTerrainPixels) == PngError::Ok
            && decoded.width == decoded.height && decoded.width > 0) {
            source = std::move(decoded);
        }
    }

    if (source.empty() && !cutDirtFromAtlas(atlas, &source)) {
        return PackError::NotFound;
    }

    scaleSquare(source, kBackgroundEdge, out);
    darken(out);
    return PackError::Ok;
}

}  // namespace mc::texture
