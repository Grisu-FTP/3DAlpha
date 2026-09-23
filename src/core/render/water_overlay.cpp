// See water_overlay.hpp.

#include "core/render/water_overlay.hpp"

#include "core/texture/water_overlay_image.hpp"

#include <cmath>

namespace mc::render {
namespace {

// `float f5 = -1, f6 = 1, f7 = -1, f8 = 1, f9 = -0.5F`.
constexpr float kHalf = 1.0f;
constexpr float kDepth = -0.5f;

// `float f4 = 4.0F`: the image repeats this many times across the quad, and
// the sheet already holds that many -- so the original's 0..4 is 0..1 here.
static_assert(texture::kWaterOverlayRepeats == 4, "jh.c(F) repeats the image four times");

// 1/16384 units, the detail vertex's UV scale.
constexpr float kUvUnits = 16384.0f;

i16 toUnits(float blocks)
{
    const float units = blocks * float(mesh::kDetailUnitsPerBlock);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

// An original UV -- whole units are whole repeats -- as sheet units. The
// scroll's integer part is dropped, which the texture's own repetition makes
// invisible, and what is left is 0..1 of a repeat: at most 5/4 of the sheet
// with the four repeats added, inside the s16's two sheet widths.
i16 sheetUv(float repeats, float scroll)
{
    const float fraction = scroll - std::floor(scroll);
    const float sheet = (repeats + fraction) / float(texture::kWaterOverlayRepeats);
    return i16(sheet * kUvUnits + 0.5f);
}

}  // namespace

int buildWaterOverlayQuad(float yawDegrees, float pitchDegrees, u8 light,
                          mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kWaterOverlayVertices) {
        return 0;
    }

    // `float f10 = -player.rotationYaw / 64.0F; float f11 = player.rotationPitch / 64.0F`.
    const float du = -yawDegrees / 64.0f;
    const float dv = pitchDegrees / 64.0f;

    // The original's corner order and UVs; see the header.
    const float cornerX[4] = {-kHalf, kHalf, kHalf, -kHalf};
    const float cornerY[4] = {-kHalf, -kHalf, kHalf, kHalf};
    const float cornerU[4] = {4.0f, 0.0f, 0.0f, 4.0f};
    const float cornerV[4] = {4.0f, 4.0f, 0.0f, 0.0f};

    for (int k = 0; k < kWaterOverlayVertices; ++k) {
        mesh::DetailVertex& v = out[k];
        v.x = toUnits(cornerX[k]);
        v.y = toUnits(cornerY[k]);
        v.z = toUnits(kDepth);
        v.face = 0;
        v.u = sheetUv(cornerU[k], du);
        v.v = sheetUv(cornerV[k], dv);
        v.r = 0xFF;
        v.g = 0xFF;
        v.b = 0xFF;
        v.light = light;
    }
    return kWaterOverlayVertices;
}

}  // namespace mc::render
