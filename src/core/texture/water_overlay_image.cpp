// See water_overlay_image.hpp.

#include "core/texture/water_overlay_image.hpp"

#include "core/texture/atlas_image.hpp"
#include "core/texture/png.hpp"

#include <cstring>

namespace mc::texture {
namespace {

// Copies one 16 x 16 RGBA tile onto every cell of the 4 x 4 sheet.
void repeatTile(const u8* tile, std::vector<u8>* out)
{
    out->assign(kWaterOverlayBytes, 0);
    constexpr usize kRowBytes = usize(kWaterOverlayTileEdge) * 4;
    for (int y = 0; y < kWaterOverlayEdge; ++y) {
        const u8* source = tile + usize(y % kWaterOverlayTileEdge) * kRowBytes;
        u8* row = out->data() + usize(y) * usize(kWaterOverlayEdge) * 4;
        for (int cell = 0; cell < kWaterOverlayRepeats; ++cell) {
            std::memcpy(row + usize(cell) * kRowBytes, source, kRowBytes);
        }
    }
}

// A small integer hash, so the mottling is the same on every console and in
// every test without a seeded stream to keep in step.
u32 hash(u32 x, u32 y)
{
    u32 h = x * 0x27D4EB2Du ^ (y + 0x9E3779B9u) * 0x165667B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    return h;
}

}  // namespace

void buildDevArtWaterOverlay(std::vector<u8>* out)
{
    // Blue with the red and green channels stepping between a few shades,
    // wrapped at the tile's edge so the four repeats meet without a seam.
    // About 55 % opaque, which with the quad's own 0.5 leaves the world
    // readable through it.
    u8 tile[kWaterOverlayTileEdge * kWaterOverlayTileEdge * 4];
    for (int y = 0; y < kWaterOverlayTileEdge; ++y) {
        for (int x = 0; x < kWaterOverlayTileEdge; ++x) {
            // Two octaves: one per texel, one per 2 x 2 block, so the pattern
            // has patches rather than salt.
            const u32 fine = hash(u32(x), u32(y)) & 0xFFu;
            const u32 coarse = hash(u32(x / 2) + 101u, u32(y / 2) + 57u) & 0xFFu;
            const int shade = int((fine + coarse * 3u) / 4u);  // 0..255
            u8* texel = tile + (y * kWaterOverlayTileEdge + x) * 4;
            texel[0] = u8(28 + shade * 32 / 255);
            texel[1] = u8(72 + shade * 48 / 255);
            texel[2] = 0xFF;
            texel[3] = u8(132 + shade * 16 / 255);
        }
    }
    repeatTile(tile, out);
}

void buildWaterOverlay(io::FileSystem& fs, std::string_view packPath, std::vector<u8>* out)
{
    buildDevArtWaterOverlay(out);
    if (packPath.empty()) {
        return;
    }

    std::vector<u8> png;
    if (readPackFile(fs, packPath, kWaterOverlayFile, &png) != PackError::Ok) {
        return;
    }
    Image image;
    if (decodePng(png, &image, kMaxWaterOverlayPixels) != PngError::Ok) {
        return;
    }
    // Square, or the stand-in stays: the quad repeats it the same number of
    // times across and down.
    if (image.width != image.height || image.width <= 0) {
        return;
    }

    std::vector<u8> tile;
    scaleSquare(image, kWaterOverlayTileEdge, &tile);
    repeatTile(tile.data(), out);
}

}  // namespace mc::texture
