// See sign_mesh.hpp. `jk`'s constructor and `in.a(Lob;DDDF)V`.

#include "core/render/sign_mesh.hpp"

#include "core/render/box_model.hpp"
#include "core/render/draw_budget.hpp"
#include "core/util/math_helper.hpp"

#include <cstring>

namespace mc::render {
namespace {

constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

constexpr float kPi = 3.1415927f;
constexpr float kDegrees = kPi / 180.0f;

constexpr u8 kSignShade = 255;

// The font sheet is 128 x 128 of 8-pixel cells, sixteen across.
constexpr int kFontSheetEdge = texture::kFontEdge;
constexpr int kFontCell = texture::kFontCellPixels;
constexpr int kFontCells = texture::kFontGlyphsPerEdge;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

i16 fontUv(float texels)
{
    const float units = texels * float(mesh::kUvUnitsPerAtlas) / float(kFontSheetEdge);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

// `jk`'s two boxes. The board is at texture offset (0, 0) and the post at
// (0, 14), both on the sign page.
const ModelPart& signBoard()
{
    static const ModelPart part = {-12.0f, -14.0f, -1.0f, 24, 12, 2, 0.0f, 0,     0,
                                   false,  0.0f,   0.0f,  0.0f, 0.0f, 0.0f, 0.0f};
    return part;
}

const ModelPart& signPost()
{
    static const ModelPart part = {-1.0f, -2.0f, -1.0f, 2,    14,   2,    0.0f, 0,   14,
                                   false, 0.0f,  0.0f,  0.0f, 0.0f, 0.0f, 0.0f};
    return part;
}

// The turn a sign's metadata means, in degrees, and the step back a wall sign
// takes afterwards. Both straight out of `TileEntitySignRenderer`.
struct SignPose {
    float degrees;
    float backX, backY, backZ;
};

SignPose poseOf(const world::SignText& sign)
{
    if (!sign.wall) {
        // **A sixteenth of a turn per metadata step**, which is why a post
        // faces where you stood rather than a compass point.
        return SignPose{float(int(sign.metadata) * 360) / 16.0f, 0.0f, 0.0f, 0.0f};
    }
    float degrees = 0.0f;
    if (sign.metadata == 2) degrees = 180.0f;
    if (sign.metadata == 4) degrees = 90.0f;
    if (sign.metadata == 5) degrees = -90.0f;
    // The step that puts the board against the wall rather than in the middle
    // of the block.
    return SignPose{degrees, 0.0f, -0.3125f, -0.4375f};
}

// The sign's origin and its three axes, in blocks, with the 2/3 scale and the
// `(f, -f, -f)` flip folded in.
struct SignFrame {
    double x, y, z;
    float ax[3], ay[3], az[3];
};

SignFrame frameOf(const world::SignText& sign, double originX, double originY,
                  double originZ, bool* inRange)
{
    const SignPose pose = poseOf(sign);
    const float radians = -pose.degrees * kDegrees;
    const float s = MathHelper::sin(radians);
    const float c = MathHelper::cos(radians);

    // `glTranslatef(x + 0.5F, y + 0.75F * scale, z + 0.5F)`.
    double px = double(sign.x) + 0.5;
    double py = double(sign.y) + double(0.75f * kSignScale);
    double pz = double(sign.z) + 0.5;

    // ...then the wall sign's step back, which happens *after* the turn and so
    // is in the sign's own frame.
    px += double(pose.backX * c + pose.backZ * s);
    py += double(pose.backY);
    pz += double(-pose.backX * s + pose.backZ * c);

    SignFrame f{};
    f.x = px - originX;
    f.y = py - originY;
    f.z = pz - originZ;
    *inRange = !(f.x < -kLimit || f.x > kLimit || f.y < -kLimit || f.y > kLimit
                 || f.z < -kLimit || f.z > kLimit);

    // **`glScalef(f, -f, -f)`**, which is not the `(-1, -1, 1)` every entity
    // model gets: a sign is mirrored on z rather than on x. Times the model
    // unit, and then turned about y.
    const float m = kSignScale * kModelUnit;
    const float base[3][3] = {{m, 0.0f, 0.0f}, {0.0f, -m, 0.0f}, {0.0f, 0.0f, -m}};
    float* axes[3] = {f.ax, f.ay, f.az};
    for (int a = 0; a < 3; ++a) {
        axes[a][0] = base[a][0] * c + base[a][2] * s;
        axes[a][1] = base[a][1];
        axes[a][2] = -base[a][0] * s + base[a][2] * c;
    }
    return f;
}

// **Nearest first when there are more signs in range than the buffer holds**
// -- see core/render/draw_budget.hpp. Every sign is charged its board, its post
// and a full four lines of text against the *whole* buffer the two passes
// share, in both passes; the second rewinds the edge, so both pick the same
// signs and the text always has room behind the boards.
constexpr int kSignDrawEach = kSignVerticesEach + kSignGlyphsEach * 4;

DrawCutoff& settleSignCutoff(const world::SignStore& store, double originX, double originY,
                             double originZ, int budget, DrawCutoff* shared, DrawCutoff* local)
{
    DrawCutoff& cutoff = shared != nullptr ? *shared : *local;
    if (cutoff.computed()) {
        cutoff.rewind();
        return cutoff;
    }
    if (store.count() * kSignDrawEach <= budget) {
        cutoff.drawAll();
        return cutoff;
    }
    const auto place = [&](int i, double* dx, double* dy, double* dz) {
        const world::SignText& sign = store[i];
        if (!sign.used) {
            return false;
        }
        bool inRange = false;
        const SignFrame f = frameOf(sign, originX, originY, originZ, &inRange);
        *dx = f.x;
        *dy = f.y;
        *dz = f.z;
        return inRange;
    };
    cutoff.compute(store.count(), budget, place, [](int) { return kSignDrawEach; });
    return cutoff;
}

}  // namespace

int buildSignBoards(const world::SignStore& store, double originX, double originY,
                    double originZ, mesh::DetailVertex* out, int max, DrawCutoff* shared)
{
    if (out == nullptr || max < kSignVerticesEach) {
        return 0;
    }

    DrawCutoff local;
    DrawCutoff& cutoff =
        settleSignCutoff(store, originX, originY, originZ, max, shared, &local);

    int written = 0;
    for (int i = 0; i < store.count(); ++i) {
        const world::SignText& sign = store[i];
        if (!sign.used) {
            continue;
        }
        if (written + kSignVerticesEach > max) {
            break;
        }

        bool inRange = false;
        const SignFrame f = frameOf(sign, originX, originY, originZ, &inRange);
        if (!inRange || !cutoff.admit(f.x, f.y, f.z, kSignDrawEach)) {
            continue;
        }

        Placement place;
        place.x = f.x;
        place.y = f.y;
        place.z = f.z;
        for (int a = 0; a < 3; ++a) {
            place.ax[a] = f.ax[a];
            place.ay[a] = f.ay[a];
            place.az[a] = f.az[a];
        }

        const u8 light = sign.light;
        written += buildBox(signBoard(), place, texture::EntitySkin::Sign, light,
                            out + written, max - written);
        // **The post is hidden on a wall sign**, which is the one thing the two
        // blocks share a model for.
        if (!sign.wall) {
            written += buildBox(signPost(), place, texture::EntitySkin::Sign, light,
                                out + written, max - written);
        }
    }
    return written;
}

int buildSignText(const world::SignStore& store, const texture::FontImage& font,
                  double originX, double originY, double originZ, mesh::DetailVertex* out,
                  int max, DrawCutoff* shared)
{
    // **A pack with no `default.png` shows a blank sign.** There is no
    // generated font the way there is generated art -- see
    // core/texture/font.hpp -- so this is the one place where Dev Art is less
    // than a pack, and it is stated rather than papered over.
    if (out == nullptr || font.empty()) {
        return 0;
    }

    DrawCutoff local;
    DrawCutoff& cutoff =
        settleSignCutoff(store, originX, originY, originZ, max, shared, &local);

    int written = 0;
    for (int i = 0; i < store.count(); ++i) {
        const world::SignText& sign = store[i];
        if (!sign.used) {
            continue;
        }

        bool inRange = false;
        const SignFrame f = frameOf(sign, originX, originY, originZ, &inRange);
        if (!inRange || !cutoff.admit(f.x, f.y, f.z, kSignDrawEach)) {
            continue;
        }
        const u8 light = sign.light;

        // The text's own frame: translated up and forward, then scaled to a
        // font pixel and flipped on y. **It is not inside the model's
        // `glScalef(f, -f, -f)`** -- `in` pops that before the text -- so it
        // hangs off the turned frame alone, and working in `frameOf`'s model
        // axes means undoing that scale's y and z flips here.
        //
        // `glTranslatef(0, 0.5F * scale, 0.07F * scale)` is in *block* units
        // for the same reason, so it is converted here.
        const float lift = 0.5f * kSignScale / (kSignScale * kModelUnit);
        const float toward = 0.07f * kSignScale / (kSignScale * kModelUnit);
        // ...and a font pixel is `0.016666668F * scale` blocks, likewise.
        const float glyph = kSignTextScale * kSignScale / (kSignScale * kModelUnit);

        for (int line = 0; line < world::kSignLines; ++line) {
            const char* text = sign.lines[line];
            const usize length = std::strlen(text);
            if (length == 0) {
                continue;
            }
            const std::string_view view(text, length);
            const int lineWidth = texture::textWidth(font.widths, view);

            // `drawString(line, -width / 2, i * 10 - lines * 5, colour)`, in
            // font pixels.
            float penX = float(-lineWidth / 2);
            const float penY = float(line * 10 - world::kSignLines * 5);

            usize pos = 0;
            while (pos < length) {
                const u32 code = texture::nextCodepoint(view, &pos);
                // A colour code eats its digit and changes nothing here: a sign
                // is drawn in one colour and the original passes 0 for it.
                if (code == 0xA7) {
                    if (pos < length) {
                        texture::nextCodepoint(view, &pos);
                    }
                    continue;
                }
                const int index = texture::fontGlyph(code);
                if (index < 0) {
                    continue;
                }
                const int advance = int(font.widths[index]);
                if (written + 4 > max) {
                    return written;
                }

                const float cellX = float(index % kFontCells) * float(kFontCell);
                const float cellY = float(index / kFontCells) * float(kFontCell);

                // The quad, in font pixels, mapped into the sign's model units.
                const float x0 = penX * glyph;
                const float x1 = (penX + float(kFontCell)) * glyph;
                // The turned frame's y is `lift - penY * glyph`, font rows
                // running down the board, and its z is `toward`, out of the
                // front. The model axes carry both negated.
                const float y0 = penY * glyph - lift;
                const float y1 = (penY + float(kFontCell)) * glyph - lift;
                const float z = -toward;

                const float corner[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
                const float uv[4][2] = {{cellX, cellY},
                                        {cellX + float(kFontCell), cellY},
                                        {cellX + float(kFontCell), cellY + float(kFontCell)},
                                        {cellX, cellY + float(kFontCell)}};

                for (int c = 0; c < 4; ++c) {
                    const float mx = corner[c][0];
                    const float my = corner[c][1];
                    mesh::DetailVertex& v = out[written];
                    v.x = toUnits(f.x + double(mx * f.ax[0] + my * f.ay[0] + z * f.az[0]));
                    v.y = toUnits(f.y + double(mx * f.ax[1] + my * f.ay[1] + z * f.az[1]));
                    v.z = toUnits(f.z + double(mx * f.ax[2] + my * f.ay[2] + z * f.az[2]));
                    v.face = 0;
                    v.u = fontUv(uv[c][0]);
                    v.v = fontUv(uv[c][1]);
                    v.r = kSignShade;
                    v.g = kSignShade;
                    v.b = kSignShade;
                    v.light = light;
                    ++written;
                }
                penX += float(advance);
            }
        }
    }
    return written;
}

}  // namespace mc::render
