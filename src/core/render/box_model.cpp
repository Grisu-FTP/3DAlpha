// See box_model.hpp. `ip.addBox` and `ll`'s UV constructor, transcribed.

#include "core/render/box_model.hpp"

#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

// **Full brightness on all three channels.** `ModelBase.render` never sets a
// vertex colour; the light byte is what darkens a boat in a cave, exactly as it
// is for a dropped item.
constexpr u8 kModelShade = 255;

// `ll`'s two hard-coded divisors, and the tenth-of-a-texel inset it applies in
// each. See the header: these are the class file's, not the PICA's.
constexpr float kPageU = float(texture::kSkinPageWidth);   // 64.0f
constexpr float kPageV = float(texture::kSkinPageHeight);  // 32.0f
constexpr float kFudgeU = 0.1f / kPageU;
constexpr float kFudgeV = 0.1f / kPageV;

// Rounded half away from zero, and **refused rather than wrapped** when the
// short cannot hold it -- see the note on `buildBox`.
bool toUnits(double blocks, double unitsPerBlock, i16* out)
{
    const double units = blocks * unitsPerBlock;
    const double rounded = units >= 0.0 ? units + 0.5 : units - 0.5;
    if (!(rounded > -32768.0 && rounded < 32768.0)) {
        return false;
    }
    *out = i16(rounded);
    return true;
}

// **The page fraction, clamped into its own page.**
//
// Some of these models address texels past the edge of their 64 x 32 page. The
// minecart's underside plate is the clear case: it is 18 x 14 x 1 at texture
// offset (44, 10), and the unwrap needs `2 * (w + d)` = 38 texels of width from
// u 44 -- which reaches u 82 on a page 64 wide. The original does not care,
// because `item/cart.png` is a texture of its own and OpenGL wraps: those UVs
// land back on some other part of the cart's own image, and the faces they
// belong to are the *inside* of the cart's floor and are never seen anyway.
//
// **Here the pages share a sheet, so wrapping is not harmless**: unclamped, the
// minecart's hidden faces would sample the arrow's page beside it, or run off
// the sheet entirely. Clamping keeps every page self-contained, which is the
// whole reason the pages have fixed offsets. The visible difference from the
// original is confined to faces the original also draws with off-page UVs.
float clampToPage(float pageFraction)
{
    if (pageFraction < 0.0f) {
        return 0.0f;
    }
    if (pageFraction > 1.0f) {
        return 1.0f;
    }
    return pageFraction;
}

// A page-space u (0..1 across 64 texels) into the whole sheet's 1/16384 units.
i16 sheetU(int originTexels, float pageFractionRaw)
{
    const float pageFraction = clampToPage(pageFractionRaw);
    const float texels = float(originTexels) + pageFraction * kPageU;
    const float units = texels * float(mesh::kUvUnitsPerAtlas)
                        / float(texture::kEntitySheetWidth);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

i16 sheetV(int originTexels, float pageFractionRaw)
{
    const float pageFraction = clampToPage(pageFractionRaw);
    const float texels = float(originTexels) + pageFraction * kPageV;
    const float units = texels * float(mesh::kUvUnitsPerAtlas)
                        / float(texture::kEntitySheetHeight);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

}  // namespace

Placement placeModel(double x, double y, double z, float yawRadians, float fallRadians)
{
    // The same two steps `placeMob` takes with no per-axis scale: the flip,
    // and then the lift expressed in the flipped frame so a downward shift in
    // the model's own axes is an upward one in the world. The fall is `Rz`,
    // nearer the vertex than the yaw's `Ry`, as `placeMob` composes them.
    constexpr float kModelHeight = 24.0f;
    constexpr float kFootLift = 0.0078125f;

    const float s = MathHelper::sin(yawRadians);
    const float c = MathHelper::cos(yawRadians);
    const float sz = MathHelper::sin(fallRadians);
    const float cz = MathHelper::cos(fallRadians);
    const auto turn = [&](float mx, float my, float mz, float* out) {
        const float rx = mx * cz - my * sz;
        const float ry = mx * sz + my * cz;
        out[0] = rx * c + mz * s;
        out[1] = ry;
        out[2] = mz * c - rx * s;
    };

    Placement place;
    turn(-kModelUnit, 0.0f, 0.0f, place.ax);
    turn(0.0f, -kModelUnit, 0.0f, place.ay);
    turn(0.0f, 0.0f, kModelUnit, place.az);

    float lift[3];
    turn(0.0f, kModelHeight * kModelUnit + kFootLift, 0.0f, lift);
    place.x = x + double(lift[0]);
    place.y = y + double(lift[1]);
    place.z = z + double(lift[2]);
    return place;
}

float deathFall(int deathTime, float partial)
{
    constexpr float kDeathSpin = 1.6f;
    constexpr float kDeathMaxRotation = 90.0f;
    constexpr float kPi = 3.1415927f;
    if (deathTime <= 0) {
        return 0.0f;
    }
    float fall = MathHelper::sqrtFloat((float(deathTime) + partial - 1.0f) / 20.0f * kDeathSpin);
    if (fall > 1.0f) {
        fall = 1.0f;
    }
    return fall * kDeathMaxRotation * kPi / 180.0f;
}

Placement placeAt(double x, double y, double z, float yawRadians, float scale)
{
    const float s = MathHelper::sin(yawRadians);
    const float c = MathHelper::cos(yawRadians);
    Placement place;
    place.x = x;
    place.y = y;
    place.z = z;
    // A right-handed turn about +Y, which is the same one the item entity's
    // spin uses -- x' = x cos + z sin, z' = z cos - x sin.
    place.ax[0] = c * scale;
    place.ax[1] = 0.0f;
    place.ax[2] = -s * scale;
    place.ay[0] = 0.0f;
    place.ay[1] = scale;
    place.ay[2] = 0.0f;
    place.az[0] = s * scale;
    place.az[1] = 0.0f;
    place.az[2] = c * scale;
    return place;
}

int buildBox(const ModelPart& part, const Placement& place, texture::EntitySkin skin, u8 light,
             mesh::DetailVertex* out, int max, int unitsPerBlock)
{
    if (out == nullptr || max < kBoxVertices) {
        return 0;
    }

    // `addBox`, line for line. The second corner is the first plus the size,
    // *then* both are pushed apart by `grow` -- so `grow` widens the box rather
    // than offsetting it.
    float x1 = part.x;
    float y1 = part.y;
    float z1 = part.z;
    float x2 = part.x + float(part.w);
    float y2 = part.y + float(part.h);
    float z2 = part.z + float(part.d);

    x1 -= part.grow;
    y1 -= part.grow;
    z1 -= part.grow;
    x2 += part.grow;
    y2 += part.grow;
    z2 += part.grow;

    if (part.mirror) {
        const float swap = x2;
        x2 = x1;
        x1 = swap;
    }

    // The eight corners in the original's own order, which the quad table below
    // indexes. Not `x + 2y + 4z`: this is `ed[0..7]` as `addBox` fills it.
    const float cx[8] = {x1, x2, x2, x1, x1, x2, x2, x1};
    const float cy[8] = {y1, y1, y2, y2, y1, y1, y2, y2};
    const float cz[8] = {z1, z1, z1, z1, z2, z2, z2, z2};

    // The six quads: four corner indices, then the u/v rectangle in page
    // texels. Read straight off `ip.addBox`'s six `new ll(...)` calls, in its
    // order -- +X, -X, bottom, top, -Z, +Z.
    const int w = part.w;
    const int h = part.h;
    const int d = part.d;
    const int tu = part.texU;
    const int tv = part.texV;

    struct Quad {
        int corner[4];
        int u1, v1, u2, v2;
    };
    const Quad quads[6] = {
        {{5, 1, 2, 6}, tu + d + w, tv + d, tu + d + w + d, tv + d + h},
        {{0, 4, 7, 3}, tu, tv + d, tu + d, tv + d + h},
        {{5, 4, 0, 1}, tu + d, tv, tu + d + w, tv + d},
        {{2, 3, 7, 6}, tu + d + w, tv, tu + d + w + w, tv + d},
        {{1, 0, 3, 2}, tu + d, tv + d, tu + d + w, tv + d + h},
        {{4, 5, 6, 7}, tu + d + w + d, tv + d, tu + d + w + d + w, tv + d + h},
    };

    int originU = 0;
    int originV = 0;
    texture::skinOrigin(skin, &originU, &originV);

    // The part's own rotation, applied to a corner before the pivot is added.
    // **X first, then Y, then Z** -- the three `glRotatef` calls in `render`
    // are written Z, Y, X and post-multiply, so the matrix is Rz Ry Rx and a
    // point meets Rx first. Getting this backwards is invisible on a boat,
    // whose only non-zero angle is one, and wrong on anything with two.
    const float sinX = MathHelper::sin(part.angleX);
    const float cosX = MathHelper::cos(part.angleX);
    const float sinY = MathHelper::sin(part.angleY);
    const float cosY = MathHelper::cos(part.angleY);
    const float sinZ = MathHelper::sin(part.angleZ);
    const float cosZ = MathHelper::cos(part.angleZ);

    const double units = double(unitsPerBlock);
    int written = 0;
    for (int q = 0; q < 6; ++q) {
        const Quad& quad = quads[q];

        // `ll(vertices, u1, v1, u2, v2)` writes these four pairs in this order.
        // Note that it is not a simple min/max walk: vertex 0 takes u2 and
        // vertex 1 takes u1, which is what winds the quad the way the class
        // file winds it.
        const float us[4] = {float(quad.u2) / kPageU - kFudgeU,
                             float(quad.u1) / kPageU + kFudgeU,
                             float(quad.u1) / kPageU + kFudgeU,
                             float(quad.u2) / kPageU - kFudgeU};
        const float vs[4] = {float(quad.v1) / kPageV + kFudgeV,
                             float(quad.v1) / kPageV + kFudgeV,
                             float(quad.v2) / kPageV - kFudgeV,
                             float(quad.v2) / kPageV - kFudgeV};

        for (int c = 0; c < 4; ++c) {
            const int index = quad.corner[c];
            float px = cx[index];
            float py = cy[index];
            float pz = cz[index];

            // Rx
            float ty = py * cosX - pz * sinX;
            float tz = py * sinX + pz * cosX;
            py = ty;
            pz = tz;
            // Ry
            float tx = px * cosY + pz * sinY;
            tz = pz * cosY - px * sinY;
            px = tx;
            pz = tz;
            // Rz
            tx = px * cosZ - py * sinZ;
            ty = px * sinZ + py * cosZ;
            px = tx;
            py = ty;

            // The rotation point is added *after* the rotation, because the box
            // is defined relative to it -- that is the whole meaning of a
            // rotation point.
            px += part.pivotX;
            py += part.pivotY;
            pz += part.pivotZ;

            const double wx = place.x + double(px * place.ax[0] + py * place.ay[0]
                                               + pz * place.az[0]);
            const double wy = place.y + double(px * place.ax[1] + py * place.ay[1]
                                               + pz * place.az[1]);
            const double wz = place.z + double(px * place.ax[2] + py * place.ay[2]
                                               + pz * place.az[2]);

            mesh::DetailVertex& v = out[written];
            if (!toUnits(wx, units, &v.x) || !toUnits(wy, units, &v.y)
                || !toUnits(wz, units, &v.z)) {
                return 0;
            }
            v.face = 0;
            v.u = sheetU(originU, us[c]);
            v.v = sheetV(originV, vs[c]);
            v.r = kModelShade;
            v.g = kModelShade;
            v.b = kModelShade;
            v.light = light;
            ++written;
        }
    }
    return written;
}

}  // namespace mc::render
