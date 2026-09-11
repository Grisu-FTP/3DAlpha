// See arrow_mesh.hpp. `gk.a(Lkg;DDDFF)V` transcribed.

#include "core/render/arrow_mesh.hpp"

#include "core/render/draw_budget.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

constexpr float kPi = 3.1415927f;
constexpr float kDegrees = kPi / 180.0f;

// `RenderArrow` sets no vertex colour; the light byte does the darkening.
constexpr u8 kArrowShade = 255;

// The arrow's own page is square, unlike the three box-model pages beside it.
constexpr float kArrowPage = 32.0f;

// `glTranslatef(-4.0F, 0.0F, 0.0F)`, applied before the scale in effect --
// see the note in the builder.
constexpr float kArrowShift = -4.0f;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

// A fraction of the arrow's 32 x 32 page into the shared sheet's 1/16384 units.
i16 sheetU(int originTexels, float pageFraction)
{
    const float texels = float(originTexels) + pageFraction * kArrowPage;
    const float units =
        texels * float(mesh::kUvUnitsPerAtlas) / float(texture::kEntitySheetWidth);
    return i16(units + 0.5f);
}

i16 sheetV(int originTexels, float pageFraction)
{
    const float texels = float(originTexels) + pageFraction * kArrowPage;
    const float units =
        texels * float(mesh::kUvUnitsPerAtlas) / float(texture::kEntitySheetHeight);
    return i16(units + 0.5f);
}

// One quad of the model, in the arrow's own units before any transform.
struct ModelQuad {
    float x[4], y[4], z[4];
    float u[4], v[4];
    // How many quarter-turns about the arrow's axis this quad carries. The cap
    // pair is 0; fin `k` is `k + 1`, because the loop turns *before* it draws.
    int quarterTurns;
};

}  // namespace

int buildArrows(const entity::ArrowSystem& system, double originX, double originY,
                double originZ, float partial, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kArrowVerticesEach) {
        return 0;
    }

    // The four UV rectangles, as `doRender` computes them with its `i` at 0.
    // The fins take u 0..0.5 and v 0..5/32; the cap takes u 0..5/32 and
    // v 5/32..10/32.
    const float finU0 = 0.0f;
    const float finU1 = 0.5f;
    const float finV0 = 0.0f;
    const float finV1 = 5.0f / 32.0f;
    const float capU0 = 0.0f;
    const float capU1 = 0.15625f;  // 5/32
    const float capV0 = 5.0f / 32.0f;
    const float capV1 = 10.0f / 32.0f;

    // **The cap is drawn twice with opposite winding**, because the original
    // gives it two normals and the detail pass has culling off -- so what this
    // buys is not visibility but the original's exact vertex order, which is
    // what the UVs are attached to.
    const ModelQuad quads[kArrowQuads] = {
        {{-7, -7, -7, -7},
         {-2, -2, 2, 2},
         {-2, 2, 2, -2},
         {capU0, capU1, capU1, capU0},
         {capV0, capV0, capV1, capV1},
         0},
        {{-7, -7, -7, -7},
         {2, 2, -2, -2},
         {-2, 2, 2, -2},
         {capU0, capU1, capU1, capU0},
         {capV0, capV0, capV1, capV1},
         0},
        {{-8, 8, 8, -8},
         {-2, -2, 2, 2},
         {0, 0, 0, 0},
         {finU0, finU1, finU1, finU0},
         {finV0, finV0, finV1, finV1},
         1},
        {{-8, 8, 8, -8},
         {-2, -2, 2, 2},
         {0, 0, 0, 0},
         {finU0, finU1, finU1, finU0},
         {finV0, finV0, finV1, finV1},
         2},
        {{-8, 8, 8, -8},
         {-2, -2, 2, 2},
         {0, 0, 0, 0},
         {finU0, finU1, finU1, finU0},
         {finV0, finV0, finV1, finV1},
         3},
        {{-8, 8, 8, -8},
         {-2, -2, 2, 2},
         {0, 0, 0, 0},
         {finU0, finU1, finU1, finU0},
         {finV0, finV0, finV1, finV1},
         4},
    };

    int originU = 0;
    int originV = 0;
    texture::skinOrigin(texture::EntitySkin::Arrow, &originU, &originV);

    // Where an arrow is drawn relative to the origin, and whether it is drawn
    // at all. Shared by the nearest-first count and the build, which must agree.
    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::Arrow& a = system[index];
        if (!a.alive) {
            return false;
        }
        *rx = a.prevX + (a.x - a.prevX) * double(partial) - originX;
        *ry = a.prevY + (a.y - a.prevY) * double(partial) - originY;
        *rz = a.prevZ + (a.z - a.prevZ) * double(partial) - originZ;
        return *rx >= -kLimit && *rx <= kLimit && *ry >= -kLimit && *ry <= kLimit
               && *rz >= -kLimit && *rz <= kLimit;
    };
    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    DrawCutoff cutoff;
    if (system.count() * kArrowVerticesEach > max) {
        cutoff.compute(system.count(), max, place, [](int) { return kArrowVerticesEach; });
    }

    int written = 0;
    for (int index = 0; index < system.count(); ++index) {
        const entity::Arrow& a = system[index];
        double rx = 0.0;
        double ry = 0.0;
        double rz = 0.0;
        if (!place(index, &rx, &ry, &rz) || !cutoff.admit(rx, ry, rz, kArrowVerticesEach)) {
            continue;
        }
        if (written + kArrowVerticesEach > max) {
            break;
        }

        // `glRotatef(yaw - 90, 0,1,0)` then `glRotatef(pitch, 0,0,1)`, with
        // both angles interpolated the way `doRender` interpolates them.
        const float yaw = (a.prevYaw + (a.yaw - a.prevYaw) * partial - 90.0f) * kDegrees;
        float roll = (a.prevPitch + (a.pitch - a.prevPitch) * partial) * kDegrees;

        // The impact wobble, in degrees before the conversion:
        // `-sin(shake * 3) * shake`. It decays with the counter, so an arrow
        // that stuck a second ago is still.
        const float shake = float(a.shake) - partial;
        if (shake > 0.0f) {
            roll += -MathHelper::sin(shake * 3.0f) * shake * kDegrees;
        }

        const float sinYaw = MathHelper::sin(yaw);
        const float cosYaw = MathHelper::cos(yaw);
        const float sinRoll = MathHelper::sin(roll);
        const float cosRoll = MathHelper::cos(roll);
        // `glRotatef(45.0F, 1,0,0)` -- the fixed roll that stands the fin cross
        // on its diagonal.
        const float sinTilt = MathHelper::sin(45.0f * kDegrees);
        const float cosTilt = MathHelper::cos(45.0f * kDegrees);

        for (int q = 0; q < kArrowQuads; ++q) {
            const ModelQuad& quad = quads[q];
            // The fin loop's own quarter-turns about x. Whole quarter turns, so
            // this is a swap and a sign rather than a sine.
            const int turns = quad.quarterTurns & 3;

            for (int c = 0; c < 4; ++c) {
                float vx = quad.x[c];
                float vy = quad.y[c];
                float vz = quad.z[c];

                // Rx by whole quarter turns.
                for (int t = 0; t < turns; ++t) {
                    const float ny = -vz;
                    vz = vy;
                    vy = ny;
                }

                // **The shift then the scale.** The class file scales first and
                // translates second, but a translation after a uniform scale in
                // GL is applied in the scaled frame -- so the two are the same
                // as shifting in model units and then scaling, which is what
                // this does.
                vx += kArrowShift;
                vx *= kArrowScale;
                vy *= kArrowScale;
                vz *= kArrowScale;

                // Rx(45)
                float ty = vy * cosTilt - vz * sinTilt;
                float tz = vy * sinTilt + vz * cosTilt;
                vy = ty;
                vz = tz;

                // Rz(pitch + wobble)
                float tx = vx * cosRoll - vy * sinRoll;
                ty = vx * sinRoll + vy * cosRoll;
                vx = tx;
                vy = ty;

                // Ry(yaw - 90)
                tx = vx * cosYaw + vz * sinYaw;
                tz = -vx * sinYaw + vz * cosYaw;
                vx = tx;
                vz = tz;

                mesh::DetailVertex& out_v = out[written];
                out_v.x = toUnits(rx + double(vx));
                out_v.y = toUnits(ry + double(vy));
                out_v.z = toUnits(rz + double(vz));
                out_v.face = 0;
                out_v.u = sheetU(originU, quad.u[c]);
                out_v.v = sheetV(originV, quad.v[c]);
                out_v.r = kArrowShade;
                out_v.g = kArrowShade;
                out_v.b = kArrowShade;
                out_v.light = a.light;
                ++written;
            }
        }
    }
    return written;
}

}  // namespace mc::render
