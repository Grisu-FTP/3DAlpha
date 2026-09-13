// See player_model.hpp. `cr`'s constructor and `cr.a(FFFFFF)`, transcribed.

#include "core/render/player_model.hpp"

#include "core/texture/entity_skins.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr float kPi = 3.1415927f;
constexpr float kDegreesPerRadian = 57.295776f;
constexpr float kLimbFrequency = 0.6662f;
constexpr float kLegAmplitude = 1.4f;

ModelPart box(int texU, int texV, float x, float y, float z, int w, int h, int d, float grow,
              float pivotX, float pivotY, float pivotZ, bool mirror = false)
{
    ModelPart part;
    part.texU = texU;
    part.texV = texV;
    part.x = x;
    part.y = y;
    part.z = z;
    part.w = w;
    part.h = h;
    part.d = d;
    part.grow = grow;
    part.pivotX = pivotX;
    part.pivotY = pivotY;
    part.pivotZ = pivotZ;
    part.mirror = mirror;
    return part;
}

}  // namespace

void bipedModel(ModelPart* out)
{
    out[kBipedHead] = box(0, 0, -4.0f, -8.0f, -4.0f, 8, 8, 8, 0.0f, 0.0f, 0.0f, 0.0f);
    // **The hat is the head grown by a half** and is drawn even when the page
    // has nothing in that corner -- `cr.b` renders all seven. On a mob skin the
    // second layer is blank, so it costs 24 vertices and draws nothing.
    out[kBipedHat] = box(32, 0, -4.0f, -8.0f, -4.0f, 8, 8, 8, 0.5f, 0.0f, 0.0f, 0.0f);
    out[kBipedBody] = box(16, 16, -4.0f, 0.0f, -2.0f, 8, 12, 4, 0.0f, 0.0f, 0.0f, 0.0f);
    // The two `mirror` flags are `ip.g`, and they are what let one page texture
    // a left and a right limb.
    out[kBipedArmRight] = box(40, 16, -3.0f, -2.0f, -2.0f, 4, 12, 4, 0.0f, -5.0f, 2.0f, 0.0f);
    out[kBipedArmLeft] =
        box(40, 16, -1.0f, -2.0f, -2.0f, 4, 12, 4, 0.0f, 5.0f, 2.0f, 0.0f, /*mirror=*/true);
    out[kBipedLegRight] = box(0, 16, -2.0f, 0.0f, -2.0f, 4, 12, 4, 0.0f, -2.0f, 12.0f, 0.0f);
    out[kBipedLegLeft] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, 12, 4, 0.0f, 2.0f, 12.0f, 0.0f, /*mirror=*/true);
}

void posePlayer(ModelPart* parts, float limbSwing, float limbAmount, float ageTicks,
                float headYaw, float headPitch)
{
    parts[kBipedHead].angleY = headYaw / kDegreesPerRadian;
    parts[kBipedHead].angleX = headPitch / kDegreesPerRadian;
    parts[kBipedHat].angleY = parts[kBipedHead].angleY;
    parts[kBipedHat].angleX = parts[kBipedHead].angleX;

    const float phase = limbSwing * kLimbFrequency;
    parts[kBipedArmRight].angleX = MathHelper::cos(phase + kPi) * 2.0f * limbAmount * 0.5f;
    parts[kBipedArmLeft].angleX = MathHelper::cos(phase) * 2.0f * limbAmount * 0.5f;
    parts[kBipedArmRight].angleZ = 0.0f;
    parts[kBipedArmLeft].angleZ = 0.0f;
    parts[kBipedLegRight].angleX = MathHelper::cos(phase) * kLegAmplitude * limbAmount;
    parts[kBipedLegLeft].angleX = MathHelper::cos(phase + kPi) * kLegAmplitude * limbAmount;
    parts[kBipedLegRight].angleY = 0.0f;
    parts[kBipedLegLeft].angleY = 0.0f;
    parts[kBipedArmRight].angleY = 0.0f;
    parts[kBipedArmLeft].angleY = 0.0f;

    // The swing block with `swingProgress` at zero: the body does not turn and
    // the arms hang from their constructor's points.
    parts[kBipedBody].angleY = 0.0f;
    parts[kBipedBody].angleX = 0.0f;
    parts[kBipedArmRight].pivotX = -5.0f;
    parts[kBipedArmRight].pivotZ = 0.0f;
    parts[kBipedArmLeft].pivotX = 5.0f;
    parts[kBipedArmLeft].pivotZ = 0.0f;

    // Not riding: the legs stand at twelve and the head at zero.
    parts[kBipedLegRight].pivotZ = 0.0f;
    parts[kBipedLegLeft].pivotZ = 0.0f;
    parts[kBipedLegRight].pivotY = 12.0f;
    parts[kBipedLegLeft].pivotY = 12.0f;
    parts[kBipedHead].pivotY = 0.0f;

    const float sway = MathHelper::cos(ageTicks * 0.09f) * 0.05f + 0.05f;
    const float roll = MathHelper::sin(ageTicks * 0.067f) * 0.05f;
    parts[kBipedArmRight].angleZ += sway;
    parts[kBipedArmLeft].angleZ -= sway;
    parts[kBipedArmRight].angleX += roll;
    parts[kBipedArmLeft].angleX -= roll;
}

int buildPlayerPreview(const ModelPart* parts, const Placement& place, int pageX, int pageY,
                       mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kPlayerPreviewVertices) {
        return 0;
    }

    // `buildBox` addresses the player's page in the 256 x 256 entity sheet.
    // A sheet of the same size with the page somewhere else is the same UVs
    // moved by a whole number of texels, and a texel is 64 units in both.
    static_assert(texture::kEntitySheetWidth == 256 && texture::kEntitySheetHeight == 256,
                  "the preview sheet is laid out against the entity sheet's size");
    constexpr int kUnitsPerTexel = mesh::kUvUnitsPerAtlas / 256;
    int originX = 0;
    int originY = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &originX, &originY);
    const int du = (pageX - originX) * kUnitsPerTexel;
    const int dv = (pageY - originY) * kUnitsPerTexel;

    int written = 0;
    for (int part = 0; part < kBipedParts; ++part) {
        const int count = buildBox(parts[part], place, texture::EntitySkin::Player, 0xFF,
                                   out + written, max - written);
        for (int v = written; v < written + count; ++v) {
            out[v].u = i16(out[v].u + du);
            out[v].v = i16(out[v].v + dv);
        }
        written += count;
    }
    return written;
}

}  // namespace mc::render
