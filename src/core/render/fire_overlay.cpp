// See fire_overlay.hpp.

#include "core/render/fire_overlay.hpp"

#include "core/texture/texture_fx.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr float kPi = 3.1415927f;

// `glTranslatef(-(i * 2 - 1) * 0.24F, ...)`: how far each sheet sits from the
// middle of the view.
constexpr float kSheetSpread = 0.24f;

// `glRotatef((i * 2 - 1) * 10.0F, 0, 1, 0)`.
constexpr float kSheetTurnDegrees = 10.0f;

// `float f16 = -0.5F`, and the half-width of a unit sheet.
constexpr float kSheetDepth = -0.5f;
constexpr float kSheetHalf = 0.5f;

i16 toUnits(float blocks)
{
    const float units = blocks * float(mesh::kDetailUnitsPerBlock);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

}  // namespace

int buildFireOverlay(mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kFireOverlayVertices) {
        return 0;
    }

    int written = 0;
    for (int sheet = 0; sheet < 2; ++sheet) {
        // `Block.fire.blockIndexInTexture + i * 16` is exactly `flameTile(i)`.
        const int tile = texture::flameTile(sheet);
        if (tile < 0) {
            continue;
        }
        const int column = tile % mesh::kAtlasTilesPerEdge;
        const int row = tile / mesh::kAtlasTilesPerEdge;
        const i16 u0 = mesh::tileUvMin(column);
        const i16 u1 = mesh::tileUvMax(column);
        const i16 v0 = mesh::tileUvMin(row);
        const i16 v1 = mesh::tileUvMax(row);

        const float side = float(sheet * 2 - 1);  // -1 for the right, +1 the left
        const float turn = side * kSheetTurnDegrees / 180.0f * kPi;
        const float s = MathHelper::sin(turn);
        const float c = MathHelper::cos(turn);
        const float tx = -side * kSheetSpread;

        // The original's corner order, and its mirrored u: low x takes the
        // tile's right edge, and the bottom row of the tile is at low y.
        const float cornerX[4] = {-kSheetHalf, kSheetHalf, kSheetHalf, -kSheetHalf};
        const float cornerY[4] = {-kSheetHalf, -kSheetHalf, kSheetHalf, kSheetHalf};
        const i16 cornerU[4] = {u1, u0, u0, u1};
        const i16 cornerV[4] = {v1, v1, v0, v0};

        for (int k = 0; k < 4; ++k) {
            // Rotate about +y, then translate -- the GL stack's order.
            const float x = cornerX[k] * c + kSheetDepth * s + tx;
            const float y = cornerY[k] + kFireOverlayDrop;
            const float z = kSheetDepth * c - cornerX[k] * s;

            mesh::DetailVertex& v = out[written++];
            v.x = toUnits(x);
            v.y = toUnits(y);
            v.z = toUnits(z);
            v.face = 0;
            v.u = cornerU[k];
            v.v = cornerV[k];
            v.r = 0xFF;
            v.g = 0xFF;
            v.b = 0xFF;
            v.light = 0xFF;
        }
    }
    return written;
}

}  // namespace mc::render
