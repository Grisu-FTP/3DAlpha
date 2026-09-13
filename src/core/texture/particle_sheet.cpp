// See particle_sheet.hpp.

#include "core/texture/particle_sheet.hpp"

#include "core/entity/particle.hpp"
#include "core/texture/png.hpp"

#include <cmath>

namespace mc::texture {
namespace {

u8* tileTexel(std::vector<u8>* out, int tile, int x, int y)
{
    const int originX = (tile % kParticleTilesPerEdge) * kParticleTilePixels;
    const int originY = (tile / kParticleTilesPerEdge) * kParticleTilePixels;
    const usize index =
        (usize(originY + y) * usize(kParticleSheetEdge) + usize(originX + x)) * 4;
    return out->data() + index;
}

// A white disc centred in the tile, `radius` texels across and `alpha` opaque
// at the middle, falling to nothing at the rim. One shape, because one shape is
// all a placeholder should claim to be.
//
// `hollow` leaves the middle out, which is the one distinction worth drawing:
// a bubble is a ring in the original and a ring is what tells it apart from a
// puff of smoke at a glance.
void disc(std::vector<u8>* out, int tile, float radius, float alpha, bool hollow = false)
{
    constexpr float kCentre = float(kParticleTilePixels) / 2.0f;
    for (int y = 0; y < kParticleTilePixels; ++y) {
        for (int x = 0; x < kParticleTilePixels; ++x) {
            const float dx = (float(x) + 0.5f) - kCentre;
            const float dy = (float(y) + 0.5f) - kCentre;
            const float distance = std::sqrt(dx * dx + dy * dy);
            float coverage = radius - distance;
            if (hollow) {
                // A ring two texels thick, so the hole survives at any radius
                // this file asks for.
                coverage = 1.0f - std::fabs(distance - radius + 1.0f);
            }
            if (coverage <= 0.0f) {
                continue;
            }
            const float edge = coverage > 1.0f ? 1.0f : coverage;
            u8* texel = tileTexel(out, tile, x, y);
            texel[0] = 0xFF;
            texel[1] = 0xFF;
            texel[2] = 0xFF;
            texel[3] = u8(alpha * edge * 255.0f + 0.5f);
        }
    }
}

}  // namespace

void buildDevArtParticles(std::vector<u8>* out)
{
    out->assign(kParticleSheetBytes, 0);

    // **The smoke row, tiles 0 to 7, and it is walked backwards.** `nl`, `dp`
    // and `en` all set `tile = 7 - age * 8 / maxAge`, so 7 is the frame a puff
    // is born in and 0 is the frame it dies in -- which is why the disc shrinks
    // and thins towards 0 and not away from it.
    for (int frame = 0; frame <= int(entity::kParticleTileSmokeLast); ++frame) {
        const float through = float(frame) / float(entity::kParticleTileSmokeLast);
        disc(out, frame, 1.2f + through * 2.0f, 0.25f + through * 0.75f);
    }

    // The bubble: a ring, which is the one sprite on this sheet that is not a
    // blob in the original either.
    disc(out, int(entity::kParticleTileBubble), 3.0f, 1.0f, /*hollow=*/true);

    // The two full-bright sprites. Both are a solid blob in the original and
    // both take their colour from nothing but the tint, so both are the same
    // disc here; what tells a flame from a lava pop on screen is the ramp it
    // shrinks along and the light it carries, neither of which is in the image.
    disc(out, int(entity::kParticleTileFlame), 3.4f, 1.0f);
    disc(out, int(entity::kParticleTileLava), 3.4f, 1.0f);

    // The rain row, tiles 19 to 22 and the splash's 20 to 23 -- five tiles
    // between them, each a smaller drop than the last.
    for (int i = 0; i <= entity::kParticleTileRainSpan; ++i) {
        const int tile = int(entity::kParticleTileRainFirst) + i;
        disc(out, tile, 2.2f - float(i) * 0.3f, 0.9f);
    }
}

void buildParticleSheet(io::FileSystem& fs, std::string_view packPath,
                        std::vector<u8>* out)
{
    buildDevArtParticles(out);
    if (packPath.empty()) {
        return;
    }

    std::vector<u8> png;
    if (readPackFile(fs, packPath, kParticleFile, &png) != PackError::Ok) {
        return;
    }

    Image image;
    if (decodePng(png, &image, kMaxParticlePixels) != PngError::Ok) {
        return;
    }
    // **Square and a tile grid, or the stand-in stays.** The render divides by
    // 16 twice; a sheet that is not a multiple of 16 on a side has no tiles to
    // divide into, and one that is not square has two different tile sizes.
    if (image.width != image.height || image.width % kParticleTilesPerEdge != 0) {
        return;
    }

    scaleSquare(image, kParticleSheetEdge, out);
}

}  // namespace mc::texture
