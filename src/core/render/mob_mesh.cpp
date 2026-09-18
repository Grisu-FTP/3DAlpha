// See mob_mesh.hpp. `hg`, `ca`, `gx`, `bx`, `dv` and `kv`'s constructors and
// `setRotationAngles`, plus `dn.a(Lge;DDDFF)V`'s transform, transcribed.

#include "core/render/mob_mesh.hpp"

#include "core/render/draw_budget.hpp"
#include "core/render/player_model.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::render {
namespace {

constexpr float kPi = 3.1415927f;
constexpr double kPiD = 3.141592653589793;

// `57.295776f`, which is the models' own radians-per-degree literal. Not
// 180/pi computed here: the class file holds this constant and a model posed
// with a slightly different one is a head that sits a hair off.
constexpr float kDegreesPerRadian = 57.295776f;

// `hg.a(FFFFFF)`'s leg swing: `cos(limbSwing * 0.6662) * 1.4 * limbSwingAmount`.
constexpr float kLegFrequency = 0.6662f;
constexpr float kLegAmplitude = 1.4f;

// `dn`'s own numbers: the model unit, the lift, and the twenty ticks a corpse
// takes to fall over.
constexpr float kModelHeight = 24.0f;
constexpr float kFootLift = 0.0078125f;
constexpr float kDeathSpin = 1.6f;
constexpr float kDeathMaxRotation = 90.0f;

// How far the hurt flash pulls green and blue down. The original blends a red
// at 0.4 alpha over the model; this multiplies instead -- see the header.
constexpr u8 kHurtChannel = 90;

// Where the parts of a quadruped live in the array, so the pose can name them.
enum QuadrupedPart { kHead = 0, kBody, kLegFrontLeft, kLegFrontRight, kLegBackLeft, kLegBackRight, kQuadrupedParts };

ModelPart box(int texU, int texV, float x, float y, float z, int w, int h, int d, float grow,
              float pivotX, float pivotY, float pivotZ)
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
    return part;
}

// `hg(int legLength, float grow)` -- ModelQuadruped's constructor, the six boxes
// three of the four animals are built from.
void quadruped(int legLength, float grow, ModelPart* out)
{
    const float leg = float(legLength);
    out[kHead] = box(0, 0, -4.0f, -4.0f, -8.0f, 8, 8, 8, grow, 0.0f, 18.0f - leg, -6.0f);
    out[kBody] = box(28, 8, -5.0f, -10.0f, -7.0f, 10, 16, 8, grow, 0.0f, 17.0f - leg, 2.0f);
    out[kLegFrontLeft] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, legLength, 4, grow, -3.0f, 24.0f - leg, 7.0f);
    out[kLegFrontRight] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, legLength, 4, grow, 3.0f, 24.0f - leg, 7.0f);
    out[kLegBackLeft] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, legLength, 4, grow, -3.0f, 24.0f - leg, -5.0f);
    out[kLegBackRight] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, legLength, 4, grow, 3.0f, 24.0f - leg, -5.0f);
}

// `hg.a(FFFFFF)` -- setRotationAngles, shared by all three quadrupeds. The two
// front legs and the two back legs swing in opposite phase, and it is the
// *diagonal* pairs that match: `leg1` and `leg4` together, `leg2` and `leg3`.
void poseQuadruped(ModelPart* parts, float limbSwing, float limbAmount, float headYaw,
                   float headPitch)
{
    parts[kHead].angleX = -headPitch / kDegreesPerRadian;
    parts[kHead].angleY = headYaw / kDegreesPerRadian;
    parts[kBody].angleX = kPi / 2.0f;

    const float swing = MathHelper::cos(limbSwing * kLegFrequency) * kLegAmplitude * limbAmount;
    const float counter =
        MathHelper::cos(limbSwing * kLegFrequency + kPi) * kLegAmplitude * limbAmount;
    parts[kLegFrontLeft].angleX = swing;
    parts[kLegFrontRight].angleX = counter;
    parts[kLegBackLeft].angleX = counter;
    parts[kLegBackRight].angleX = swing;
}

// `gx` -- ModelSheep2, the body under the fleece: a quadruped with 12-unit legs
// whose head and body are replaced by smaller boxes.
void sheepBody(ModelPart* out)
{
    quadruped(12, 0.0f, out);
    out[kHead] = box(0, 0, -3.0f, -4.0f, -6.0f, 6, 6, 8, 0.0f, 0.0f, 6.0f, -8.0f);
    out[kBody] = box(28, 8, -4.0f, -10.0f, -7.0f, 8, 16, 6, 0.0f, 0.0f, 5.0f, 2.0f);
}

// `bx` -- ModelSheep1, the fleece. Every box is the same one grown, and the
// three different grow values are what make a sheep look woolly rather than
// merely bigger: 0.6 on the head, 1.75 on the body, 0.5 on the legs -- and the
// legs are **six units long rather than twelve**, so the fleece stops at the
// knee.
void sheepFur(ModelPart* out)
{
    quadruped(12, 0.0f, out);
    out[kHead] = box(0, 0, -3.0f, -4.0f, -4.0f, 6, 6, 6, 0.6f, 0.0f, 6.0f, -8.0f);
    out[kBody] = box(28, 8, -4.0f, -10.0f, -7.0f, 8, 16, 6, 1.75f, 0.0f, 5.0f, 2.0f);
    out[kLegFrontLeft] = box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.5f, -3.0f, 12.0f, 7.0f);
    out[kLegFrontRight] = box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.5f, 3.0f, 12.0f, 7.0f);
    out[kLegBackLeft] = box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.5f, -3.0f, 12.0f, -5.0f);
    out[kLegBackRight] = box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.5f, 3.0f, 12.0f, -5.0f);
}

// `dv` -- ModelCow. Nine boxes: the quadruped's six with a new head and body,
// two horns that copy the head's angles, and an udder laid flat like the body.
// The legs are moved a unit outwards and the back pair a unit backwards, which
// is why a cow stands wider than a sheep on the same six boxes.
enum CowExtra { kCowHornLeft = kQuadrupedParts, kCowHornRight, kCowUdder, kCowParts };

void cowModel(ModelPart* out)
{
    quadruped(12, 0.0f, out);
    out[kHead] = box(0, 0, -4.0f, -4.0f, -6.0f, 8, 8, 6, 0.0f, 0.0f, 4.0f, -8.0f);
    out[kBody] = box(18, 4, -6.0f, -10.0f, -7.0f, 12, 18, 10, 0.0f, 0.0f, 5.0f, 2.0f);
    out[kLegFrontLeft].pivotX -= 1.0f;
    out[kLegFrontRight].pivotX += 1.0f;
    out[kLegBackLeft].pivotX -= 1.0f;
    out[kLegBackRight].pivotX += 1.0f;
    out[kLegBackLeft].pivotZ -= 1.0f;
    out[kLegBackRight].pivotZ -= 1.0f;

    out[kCowHornLeft] = box(22, 0, -5.0f, -5.0f, -4.0f, 1, 3, 1, 0.0f, 0.0f, 3.0f, -7.0f);
    out[kCowHornRight] = box(22, 0, 4.0f, -5.0f, -4.0f, 1, 3, 1, 0.0f, 0.0f, 3.0f, -7.0f);
    out[kCowUdder] = box(52, 0, -2.0f, -3.0f, 0.0f, 4, 6, 2, 0.0f, 0.0f, 14.0f, 6.0f);
    out[kCowUdder].angleX = kPi / 2.0f;
}

// `kv` -- ModelChicken, and its eight boxes are the only model here that is not
// a quadruped. `y0` is 16 in its constructor and every rotation point is
// relative to it.
enum ChickenPart {
    kChickenHead = 0,
    kChickenBill,
    kChickenChin,
    kChickenBody,
    kChickenLegLeft,
    kChickenLegRight,
    kChickenWingLeft,
    kChickenWingRight,
    kChickenParts,
};

void chickenModel(ModelPart* out)
{
    constexpr float y0 = 16.0f;
    out[kChickenHead] = box(0, 0, -2.0f, -6.0f, -2.0f, 4, 6, 3, 0.0f, 0.0f, y0 - 1.0f, -4.0f);
    out[kChickenBill] = box(14, 0, -2.0f, -4.0f, -4.0f, 4, 2, 2, 0.0f, 0.0f, y0 - 1.0f, -4.0f);
    out[kChickenChin] = box(14, 4, -1.0f, -2.0f, -3.0f, 2, 2, 2, 0.0f, 0.0f, y0 - 1.0f, -4.0f);
    out[kChickenBody] = box(0, 9, -3.0f, -4.0f, -3.0f, 6, 8, 6, 0.0f, 0.0f, y0, 0.0f);
    out[kChickenLegLeft] = box(26, 0, -1.0f, 0.0f, -3.0f, 3, 5, 3, 0.0f, -2.0f, y0 + 3.0f, 1.0f);
    out[kChickenLegRight] = box(26, 0, -1.0f, 0.0f, -3.0f, 3, 5, 3, 0.0f, 1.0f, y0 + 3.0f, 1.0f);
    out[kChickenWingLeft] =
        box(24, 13, 0.0f, 0.0f, -3.0f, 1, 4, 6, 0.0f, -4.0f, y0 - 3.0f, 0.0f);
    out[kChickenWingRight] =
        box(24, 13, -1.0f, 0.0f, -3.0f, 1, 4, 6, 0.0f, 4.0f, y0 - 3.0f, 0.0f);
}

// `kv.a(FFFFFF)`. `flap` is `eq.a(Lmz;F)F` -- the renderer's own
// `handleRotationFloat`, which is `(sin(wingRotation) + 1) * destPos`
// interpolated across the frame, and it is the only place an animal's
// *animation* comes from a field rather than from how far it walked.
void poseChicken(ModelPart* parts, float limbSwing, float limbAmount, float headYaw,
                 float headPitch, float flap)
{
    parts[kChickenHead].angleX = -headPitch / kDegreesPerRadian;
    parts[kChickenHead].angleY = headYaw / kDegreesPerRadian;
    parts[kChickenBill].angleX = parts[kChickenHead].angleX;
    parts[kChickenBill].angleY = parts[kChickenHead].angleY;
    parts[kChickenChin].angleX = parts[kChickenHead].angleX;
    parts[kChickenChin].angleY = parts[kChickenHead].angleY;
    parts[kChickenBody].angleX = kPi / 2.0f;
    parts[kChickenLegLeft].angleX =
        MathHelper::cos(limbSwing * kLegFrequency) * kLegAmplitude * limbAmount;
    parts[kChickenLegRight].angleX =
        MathHelper::cos(limbSwing * kLegFrequency + kPi) * kLegAmplitude * limbAmount;
    parts[kChickenWingLeft].angleZ = flap;
    parts[kChickenWingRight].angleZ = -flap;
}

// ---------------------------------------------------------------------------
// The monsters
// ---------------------------------------------------------------------------

// `cr` -- **ModelBiped**, the seven boxes a zombie and a skeleton share with
// the player. The model and its part names live in player_model.hpp now, where
// the player's own pose is beside them.
ModelPart mirrored(ModelPart part)
{
    part.mirror = true;
    return part;
}

// `fv` -- ModelSkeleton, which is `cb` with **thinner limbs**: 2 x 12 x 2
// instead of 4 x 12 x 4, all four of them, and the arms hang from the same
// points. It is the whole of the class.
void skeletonModel(ModelPart* out)
{
    bipedModel(out);
    out[kBipedArmRight] = box(40, 16, -1.0f, -2.0f, -1.0f, 2, 12, 2, 0.0f, -5.0f, 2.0f, 0.0f);
    out[kBipedArmLeft] =
        mirrored(box(40, 16, -1.0f, -2.0f, -1.0f, 2, 12, 2, 0.0f, 5.0f, 2.0f, 0.0f));
    out[kBipedLegRight] = box(0, 16, -1.0f, 0.0f, -1.0f, 2, 12, 2, 0.0f, -2.0f, 12.0f, 0.0f);
    out[kBipedLegLeft] =
        mirrored(box(0, 16, -1.0f, 0.0f, -1.0f, 2, 12, 2, 0.0f, 2.0f, 12.0f, 0.0f));
}

// `cr.a(FFFFFF)` and then `cb.a(FFFFFF)` over the top of it -- **ModelZombie,
// which both the zombie and the skeleton are posed with** (`kx` maps `mb` to
// `cb` and `cw` to `fv`, and `fv extends cb`).
//
// The swing block `cr` guards with `if (swingProgress > -9990)` is **dead for
// every mob in this version**: `swingProgress` is only ever advanced by
// `swingItem`, which nothing but a player calls, so it sits at zero and every
// term it contributes is zero. It is left out rather than written and
// multiplied by nothing.
//
// What `cb` replaces is the arms, entirely: both are pushed forward to
// horizontal and given a slow idle sway on `ticksExisted`, which is the pose
// everybody recognises. `ageTicks` is `ticksExisted + partial`.
void poseBiped(ModelPart* parts, float limbSwing, float limbAmount, float headYaw,
               float headPitch, float ageTicks)
{
    parts[kBipedHead].angleY = headYaw / kDegreesPerRadian;
    parts[kBipedHead].angleX = headPitch / kDegreesPerRadian;
    parts[kBipedHat].angleY = parts[kBipedHead].angleY;
    parts[kBipedHat].angleX = parts[kBipedHead].angleX;

    // The arms swing at **half** the legs' amplitude in `cr` (`* 2.0f * ... *
    // 0.5f`), and `cb` throws all of it away a moment later.
    parts[kBipedLegRight].angleX =
        MathHelper::cos(limbSwing * kLegFrequency) * kLegAmplitude * limbAmount;
    parts[kBipedLegLeft].angleX =
        MathHelper::cos(limbSwing * kLegFrequency + kPi) * kLegAmplitude * limbAmount;
    parts[kBipedLegRight].angleY = 0.0f;
    parts[kBipedLegLeft].angleY = 0.0f;

    // `cb.a(FFFFFF)` with `swingProgress` at zero: both sines vanish, so what
    // is left is the two constants and the sway.
    parts[kBipedArmRight].angleZ = 0.0f;
    parts[kBipedArmLeft].angleZ = 0.0f;
    parts[kBipedArmRight].angleY = -0.1f;
    parts[kBipedArmLeft].angleY = 0.1f;
    parts[kBipedArmRight].angleX = -kPi / 2.0f;
    parts[kBipedArmLeft].angleX = -kPi / 2.0f;

    const float sway = MathHelper::cos(ageTicks * 0.09f) * 0.05f + 0.05f;
    const float roll = MathHelper::sin(ageTicks * 0.067f) * 0.05f;
    parts[kBipedArmRight].angleZ += sway;
    parts[kBipedArmLeft].angleZ -= sway;
    parts[kBipedArmRight].angleX += roll;
    parts[kBipedArmLeft].angleX -= roll;
}

// `em` -- ModelCreeper. Six boxes drawn out of seven built: **the hat is
// constructed and never rendered**, because `em.b` lists `a, c, d, e, f, g` and
// leaves `b` out. It is not built here at all.
enum CreeperPart {
    kCreeperHead = 0,
    kCreeperBody,
    kCreeperFootFrontLeft,
    kCreeperFootFrontRight,
    kCreeperFootBackLeft,
    kCreeperFootBackRight,
    kCreeperParts,
};

void creeperModel(ModelPart* out)
{
    constexpr float y0 = 4.0f;
    out[kCreeperHead] = box(0, 0, -4.0f, -8.0f, -4.0f, 8, 8, 8, 0.0f, 0.0f, y0, 0.0f);
    out[kCreeperBody] = box(16, 16, -4.0f, 0.0f, -2.0f, 8, 12, 4, 0.0f, 0.0f, y0, 0.0f);
    out[kCreeperFootFrontLeft] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.0f, -2.0f, 12.0f + y0, 4.0f);
    out[kCreeperFootFrontRight] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.0f, 2.0f, 12.0f + y0, 4.0f);
    out[kCreeperFootBackLeft] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.0f, -2.0f, 12.0f + y0, -4.0f);
    out[kCreeperFootBackRight] =
        box(0, 16, -2.0f, 0.0f, -2.0f, 4, 6, 4, 0.0f, 2.0f, 12.0f + y0, -4.0f);
}

void poseCreeper(ModelPart* parts, float limbSwing, float limbAmount, float headYaw,
                 float headPitch)
{
    parts[kCreeperHead].angleY = headYaw / kDegreesPerRadian;
    parts[kCreeperHead].angleX = headPitch / kDegreesPerRadian;
    const float swing = MathHelper::cos(limbSwing * kLegFrequency) * kLegAmplitude * limbAmount;
    const float counter =
        MathHelper::cos(limbSwing * kLegFrequency + kPi) * kLegAmplitude * limbAmount;
    // **The diagonals match**, as on the quadruped: front-left with back-right.
    parts[kCreeperFootFrontLeft].angleX = swing;
    parts[kCreeperFootFrontRight].angleX = counter;
    parts[kCreeperFootBackLeft].angleX = counter;
    parts[kCreeperFootBackRight].angleX = swing;
}

// `jy` -- ModelSpider. Eleven boxes: a head, a small thorax between head and
// body, the abdomen, and **eight legs, each a 16 x 2 x 2 bar** pivoted at the
// body and splayed by a fixed pair of angles that the walk then modulates.
enum SpiderPart {
    kSpiderHead = 0,
    kSpiderNeck,
    kSpiderBody,
    kSpiderLeg0,  // the eight, in `jy`'s own order: d, e, f, g, h, i, j, m
    kSpiderParts = kSpiderLeg0 + 8,
};

void spiderModel(ModelPart* out)
{
    constexpr float y0 = 15.0f;
    out[kSpiderHead] = box(32, 4, -4.0f, -4.0f, -8.0f, 8, 8, 8, 0.0f, 0.0f, y0, -3.0f);
    out[kSpiderNeck] = box(0, 0, -3.0f, -3.0f, -3.0f, 6, 6, 6, 0.0f, 0.0f, y0, 0.0f);
    out[kSpiderBody] = box(0, 12, -5.0f, -4.0f, -6.0f, 10, 8, 12, 0.0f, 0.0f, y0, 9.0f);

    // The four pairs, left then right, from the back of the body forwards. The
    // left ones start at x = -15 and the right ones at x = -1, which is the
    // same bar pointing the other way.
    constexpr float kLegZ[4] = {2.0f, 1.0f, 0.0f, -1.0f};
    for (int pair = 0; pair < 4; ++pair) {
        out[kSpiderLeg0 + pair * 2] =
            box(18, 0, -15.0f, -1.0f, -1.0f, 16, 2, 2, 0.0f, -4.0f, y0, kLegZ[pair]);
        out[kSpiderLeg0 + pair * 2 + 1] =
            box(18, 0, -1.0f, -1.0f, -1.0f, 16, 2, 2, 0.0f, 4.0f, y0, kLegZ[pair]);
    }
}

void poseSpider(ModelPart* parts, float limbSwing, float limbAmount, float headYaw,
                float headPitch)
{
    parts[kSpiderHead].angleY = headYaw / kDegreesPerRadian;
    parts[kSpiderHead].angleX = headPitch / kDegreesPerRadian;

    // `0.7853982` is a quarter turn and `0.3926991` an eighth; the fixed splay
    // is the outer pair at a quarter and the inner three pairs at 0.74 of one.
    constexpr float kQuarter = 0.7853982f;
    constexpr float kEighth = 0.3926991f;
    constexpr float kZ[8] = {-kQuarter,        kQuarter,         -kQuarter * 0.74f,
                             kQuarter * 0.74f, -kQuarter * 0.74f, kQuarter * 0.74f,
                             -kQuarter,        kQuarter};
    // The yaw splay, and note the **third pair's signs are swapped** relative
    // to the second -- `h` gets the negative and `i` the positive, where `f`
    // and `g` are the other way round. That is what fans the legs fore and aft
    // rather than leaving all eight pointing sideways.
    constexpr float kY[8] = {kEighth * 2.0f,  -kEighth * 2.0f, kEighth,        -kEighth,
                             -kEighth,        kEighth,         -kEighth * 2.0f, kEighth * 2.0f};
    for (int leg = 0; leg < 8; ++leg) {
        parts[kSpiderLeg0 + leg].angleZ = kZ[leg];
        parts[kSpiderLeg0 + leg].angleY = kY[leg];
    }

    // The walk: four phases a quarter turn apart, each applied to a pair and
    // negated for the other side. `0.6662 * 2` is twice the quadruped's leg
    // frequency, so a spider's legs move at twice a cow's for the same speed.
    const float phase[4] = {0.0f, kPi, kPi / 2.0f, kPi * 1.5f};
    for (int pair = 0; pair < 4; ++pair) {
        const float yaw =
            -(MathHelper::cos(limbSwing * kLegFrequency * 2.0f + phase[pair]) * 0.4f)
            * limbAmount;
        const float roll =
            std::fabs(MathHelper::sin(limbSwing * kLegFrequency + phase[pair]) * 0.4f)
            * limbAmount;
        parts[kSpiderLeg0 + pair * 2].angleY += yaw;
        parts[kSpiderLeg0 + pair * 2 + 1].angleY += -yaw;
        parts[kSpiderLeg0 + pair * 2].angleZ += roll;
        parts[kSpiderLeg0 + pair * 2 + 1].angleZ += -roll;
    }
}

// `hh` -- ModelSlime, and its constructor is a switch on the one argument it
// takes: `new hh(16)` is the **inner** body with the face on it and `new hh(0)`
// is the **outer** shell. `gq` passes them in that order, so the eyes are drawn
// solid and the jelly over the top of them.
enum SlimePart {
    kSlimeBody = 0,
    kSlimeEyeLeft,
    kSlimeEyeRight,
    kSlimeMouth,
    kSlimeInnerParts,
    kSlimeShell = kSlimeInnerParts,
    kSlimeParts,
};

void slimeModel(ModelPart* out)
{
    // `hh(16)`: a 6-unit cube at texture row 16 with three little boxes on its
    // front. None of them has a rotation point, so all four hang off the
    // model's origin.
    out[kSlimeBody] = box(0, 16, -3.0f, 17.0f, -3.0f, 6, 6, 6, 0.0f, 0.0f, 0.0f, 0.0f);
    out[kSlimeEyeLeft] = box(32, 0, -3.25f, 18.0f, -3.5f, 2, 2, 2, 0.0f, 0.0f, 0.0f, 0.0f);
    out[kSlimeEyeRight] = box(32, 4, 1.25f, 18.0f, -3.5f, 2, 2, 2, 0.0f, 0.0f, 0.0f, 0.0f);
    out[kSlimeMouth] = box(32, 8, 0.0f, 21.0f, -3.5f, 1, 1, 1, 0.0f, 0.0f, 0.0f, 0.0f);
    // `hh(0)`: the 8-unit shell.
    out[kSlimeShell] = box(0, 0, -4.0f, 16.0f, -4.0f, 8, 8, 8, 0.0f, 0.0f, 0.0f, 0.0f);
}

float interpolate(float previous, float current, float partial)
{
    return previous + (current - previous) * partial;
}

}  // namespace

// `d.a(Ldd;F)V` and `gq.a(Lma;F)V` -- **preRenderCallback**, the one hook `dn`
// gives a subclass between the `glScalef(-1, -1, 1)` and the model's own lift.
// Two of the nine use it and both use it for the same thing: they are not a
// fixed size.
void modelScale(const entity::Mob& mob, float partial, float* sx, float* sy, float* sz)
{
    *sx = *sy = *sz = 1.0f;
    if (mob.type == entity::MobType::Creeper) {
        // `dd.b(F)F` -- the swell: the fuse over `fuseTime - 2`, so it passes 1
        // two ticks before the blast and keeps going. The wobble `fA` is a
        // 100-times-faster sine of the same value, which is why a creeper
        // shivers as it inflates rather than growing smoothly.
        float swell = float(mob.prevFuse + (mob.fuse - mob.prevFuse) * partial)
                      / float(entity::kFuseTicksForSwell);
        const float wobble = 1.0f + MathHelper::sin(swell * 100.0f) * swell * 0.01f;
        if (swell < 0.0f) {
            swell = 0.0f;
        }
        if (swell > 1.0f) {
            swell = 1.0f;
        }
        const float quartic = swell * swell * swell * swell;
        *sx = *sz = (1.0f + quartic * 0.4f) * wobble;
        *sy = (1.0f + quartic * 0.1f) / wobble;
        return;
    }
    if (mob.type == entity::MobType::Slime) {
        // `gq.a(Lma;F)V`. The squish is divided by `size * 0.5 + 1` before it
        // is inverted, so a big slime wobbles proportionally less than a small
        // one, and the whole model is then multiplied by the size -- which is
        // the only thing that makes a size-4 slime bigger than a size-1 one,
        // since `hh`'s boxes are the same for both.
        const float squish = interpolate(mob.prevSquish, mob.squish, partial);
        const float size = float(mob.slimeSize);
        const float wobble = 1.0f / (squish / (size * 0.5f + 1.0f) + 1.0f);
        *sx = *sz = wobble * size;
        *sy = (1.0f / wobble) * size;
    }
}

Placement placeMob(const entity::Mob& mob, double originX, double originY, double originZ,
                   float partial)
{
    const float unit = kModelUnit;

    // `180 - renderYawOffset`, interpolated, and then the corpse's fall.
    const float bodyYaw = interpolate(mob.prevRenderYaw, mob.renderYaw, partial);
    const float turn = (180.0f - bodyYaw) * kPi / 180.0f;

    float death = 0.0f;
    if (mob.deathTime > 0) {
        // `sqrt((deathTime + partial - 1) / 20 * 1.6)`, clamped at one, times
        // ninety degrees -- so an animal falls on its side over the twenty
        // ticks it lies there, quickly at first.
        float fall = MathHelper::sqrtFloat((float(mob.deathTime) + partial - 1.0f) / 20.0f
                                           * kDeathSpin);
        if (fall > 1.0f) {
            fall = 1.0f;
        }
        death = fall * kDeathMaxRotation * kPi / 180.0f;
    }

    const float sinY = MathHelper::sin(turn);
    const float cosY = MathHelper::cos(turn);
    const float sinZ = MathHelper::sin(death);
    const float cosZ = MathHelper::cos(death);

    // The composed rotation `Ry * Rz`, applied to the three model axes after
    // `glScalef(-1, -1, 1)` has flipped two of them.
    const auto rotate = [&](float x, float y, float z, float* out) {
        // Rz first (it is nearer the vertex), then Ry.
        const float rx = x * cosZ - y * sinZ;
        const float ry = x * sinZ + y * cosZ;
        out[0] = rx * cosY + z * sinY;
        out[1] = ry;
        out[2] = z * cosY - rx * sinY;
    };

    // `preRenderCallback` runs **after** the flip and **before** the lift, so
    // the scale multiplies the model axes and the lift alike -- a swollen
    // creeper rises off the ground with its own feet rather than sinking into
    // it.
    float sx = 1.0f;
    float sy = 1.0f;
    float sz = 1.0f;
    modelScale(mob, partial, &sx, &sy, &sz);

    Placement place;
    rotate(-unit * sx, 0.0f, 0.0f, place.ax);
    rotate(0.0f, -unit * sy, 0.0f, place.ay);
    rotate(0.0f, 0.0f, unit * sz, place.az);

    // `glTranslatef(0, -24 * 0.0625 - 0.0078125, 0)`, and it runs **inside**
    // `glScalef(-1, -1, 1)` -- so a downward shift in the model's own frame is
    // an upward one in the world, and the model hangs from a point 1.5078125
    // blocks above the feet rather than sinking three below them. Getting the
    // sign wrong here draws every animal buried to the ears, which is exactly
    // what `a_drawn_animal_stands_on_its_own_feet` caught.
    float lift[3];
    rotate(0.0f, (kModelHeight * kModelUnit + kFootLift) * sy, 0.0f, lift);

    place.x = mob.body.renderX(partial) - originX + double(lift[0]);
    place.y = mob.body.renderEyeY(partial) - originY + double(lift[1]);
    place.z = mob.body.renderZ(partial) - originZ + double(lift[2]);
    return place;
}

int poseMob(const entity::Mob& mob, float partial, ModelPart* parts, texture::EntitySkin* skins,
            int max)
{
    if (parts == nullptr || skins == nullptr || max < kQuadrupedParts) {
        return 0;
    }

    // `dn`'s three interpolated inputs: how hard the legs are swinging, how far
    // through the swing they are, and where the head is relative to the body.
    float amount = interpolate(mob.prevLimbYaw, mob.limbYaw, partial);
    if (amount > 1.0f) {
        amount = 1.0f;
    }
    // `limbSwing - limbYaw * (1 - partial)`, which walks the phase *backwards*
    // from the current value rather than interpolating towards it -- so the
    // legs never stutter when a mob stops.
    const float swing = mob.limbSwing - mob.limbYaw * (1.0f - partial);

    const float headYaw = interpolate(mob.prevYaw, mob.yaw, partial)
                          - interpolate(mob.prevRenderYaw, mob.renderYaw, partial);
    const float headPitch = interpolate(mob.prevPitch, mob.pitch, partial);

    int count = 0;
    switch (mob.type) {
    case entity::MobType::Pig: {
        quadruped(6, 0.0f, parts);
        poseQuadruped(parts, swing, amount, headYaw, headPitch);
        for (int i = 0; i < kQuadrupedParts; ++i) {
            skins[i] = texture::EntitySkin::Pig;
        }
        count = kQuadrupedParts;
        // `gm.a(Lmv;I)Z` -- the saddle is the same model grown by a half,
        // drawn from its own page, and only while the pig is saddled.
        if (mob.flag && count + kQuadrupedParts <= max) {
            quadruped(6, 0.5f, parts + count);
            poseQuadruped(parts + count, swing, amount, headYaw, headPitch);
            for (int i = 0; i < kQuadrupedParts; ++i) {
                skins[count + i] = texture::EntitySkin::Saddle;
            }
            count += kQuadrupedParts;
        }
        break;
    }
    case entity::MobType::Sheep: {
        sheepBody(parts);
        poseQuadruped(parts, swing, amount, headYaw, headPitch);
        for (int i = 0; i < kQuadrupedParts; ++i) {
            skins[i] = texture::EntitySkin::Sheep;
        }
        count = kQuadrupedParts;
        // `ns.a(Lbo;I)Z` -- the fleece, while the sheep still has one.
        if (!mob.flag && count + kQuadrupedParts <= max) {
            sheepFur(parts + count);
            poseQuadruped(parts + count, swing, amount, headYaw, headPitch);
            for (int i = 0; i < kQuadrupedParts; ++i) {
                skins[count + i] = texture::EntitySkin::SheepFur;
            }
            count += kQuadrupedParts;
        }
        break;
    }
    case entity::MobType::Cow: {
        if (max < kCowParts) {
            return 0;
        }
        cowModel(parts);
        poseQuadruped(parts, swing, amount, headYaw, headPitch);
        // The horns follow the head exactly -- `dv.a(FFFFFF)`'s last four
        // lines -- and the udder never moves.
        parts[kCowHornLeft].angleX = parts[kHead].angleX;
        parts[kCowHornLeft].angleY = parts[kHead].angleY;
        parts[kCowHornRight].angleX = parts[kHead].angleX;
        parts[kCowHornRight].angleY = parts[kHead].angleY;
        for (int i = 0; i < kCowParts; ++i) {
            skins[i] = texture::EntitySkin::Cow;
        }
        count = kCowParts;
        break;
    }
    case entity::MobType::Chicken: {
        if (max < kChickenParts) {
            return 0;
        }
        chickenModel(parts);
        const float flap = (MathHelper::sin(interpolate(mob.prevWingRotation, mob.wingRotation,
                                                        partial))
                            + 1.0f)
                           * interpolate(mob.prevDestPos, mob.destPos, partial);
        poseChicken(parts, swing, amount, headYaw, headPitch, flap);
        for (int i = 0; i < kChickenParts; ++i) {
            skins[i] = texture::EntitySkin::Chicken;
        }
        count = kChickenParts;
        break;
    }

    case entity::MobType::Zombie:
    case entity::MobType::Skeleton: {
        if (max < kBipedParts) {
            return 0;
        }
        const bool bones = mob.type == entity::MobType::Skeleton;
        if (bones) {
            skeletonModel(parts);
        } else {
            bipedModel(parts);
        }
        poseBiped(parts, swing, amount, headYaw, headPitch,
                  float(mob.ticksExisted) + partial);
        for (int i = 0; i < kBipedParts; ++i) {
            skins[i] = bones ? texture::EntitySkin::Skeleton : texture::EntitySkin::Zombie;
        }
        count = kBipedParts;
        break;
    }

    case entity::MobType::Creeper: {
        if (max < kCreeperParts) {
            return 0;
        }
        creeperModel(parts);
        poseCreeper(parts, swing, amount, headYaw, headPitch);
        for (int i = 0; i < kCreeperParts; ++i) {
            skins[i] = texture::EntitySkin::Creeper;
        }
        count = kCreeperParts;
        break;
    }

    case entity::MobType::Spider: {
        if (max < kSpiderParts) {
            return 0;
        }
        spiderModel(parts);
        poseSpider(parts, swing, amount, headYaw, headPitch);
        for (int i = 0; i < kSpiderParts; ++i) {
            skins[i] = texture::EntitySkin::Spider;
        }
        count = kSpiderParts;
        // **The eyes, as one part rather than as a whole second model.** `ok`
        // draws `jy` again from `mob/spider_eyes.png`, blended at
        // `(1 - brightness) * 0.5`; the eye page is transparent everywhere but
        // the head, so redrawing the other ten boxes from it writes 240
        // vertices that the alpha test throws away. One part is the same
        // picture. See mob_mesh.hpp on what the blend becomes here.
        if (count < max) {
            parts[count] = parts[kSpiderHead];
            parts[count].grow = 0.01f;
            skins[count] = texture::EntitySkin::SpiderEyes;
            ++count;
        }
        break;
    }

    case entity::MobType::Slime: {
        if (max < kSlimeParts) {
            return 0;
        }
        // **`hh` has no `setRotationAngles` at all** -- its override is empty --
        // so a slime is the one model here that is never posed. Everything it
        // appears to do is the scale in `placeMob` and the squish behind it.
        slimeModel(parts);
        for (int i = 0; i < kSlimeParts; ++i) {
            skins[i] = texture::EntitySkin::Slime;
        }
        count = kSlimeParts;
        break;
    }
    }
    return count;
}

namespace {

// **`gq.a(Lma;I)Z` -- the one render pass in a1.1.2, and the one model that
// has one.** `RenderManager` builds the slime as `new gq(new hh(16), new hh(0),
// 0.25F)`: the main model is the inner body with the eyes and the mouth on it,
// and the *pass* model is the 8-unit shell. `shouldRenderPass(0)` enables
// `GL_BLEND` with `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and draws the shell over the
// face; `shouldRenderPass(1)` turns it off again.
//
// The shell has to be blended and not alpha-tested, because the jelly really is
// translucent in the texture -- `mob/slime.png`'s outer half is alpha 199 of
// 255 -- and alpha-testing it at the world's 0.1 reference keeps every one of
// those texels at full opacity, which is a slime with no face. That is exactly
// what this port drew until the shell was given its own pass.
//
// So a mob mesh comes out in three runs: everything solid, the slime shells, and
// the spider eyes. The last two are separate from each other and not merely from
// the first because they are different *state* -- the shell blends the texture's
// own alpha with depth writes off, and the eyes blend an alpha the lightmap
// computes with depth writes on. One draw call each, and neither is ever more
// than one box per mob.
bool shellPart(entity::MobType type, int part)
{
    return type == entity::MobType::Slime && part == kSlimeShell;
}

// `ok.a(ax, int)`'s pass: the head redrawn from `mob/spider_eyes.png`. `poseMob`
// appends it after the model's own eleven, so it is always the last part.
bool eyePart(entity::MobType type, int part)
{
    return type == entity::MobType::Spider && part == kSpiderParts;
}

enum class MobPass { Solid, Shell, Eyes };

bool partBelongsTo(entity::MobType type, int part, MobPass pass)
{
    if (shellPart(type, part)) {
        return pass == MobPass::Shell;
    }
    if (eyePart(type, part)) {
        return pass == MobPass::Eyes;
    }
    return pass == MobPass::Solid;
}

int buildMobRun(const entity::MobSystem& system, double originX, double originY,
                double originZ, float partial, mesh::DetailVertex* out, int max, MobPass pass)
{
    if (out == nullptr || max < kBoxVertices) {
        return 0;
    }

    constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
    constexpr double kLimit = 32000.0 / kUnits;

    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::Mob& mob = system[index];
        if (!mob.alive) {
            return false;
        }
        *rx = mob.body.renderX(partial) - originX;
        *ry = mob.body.renderEyeY(partial) - originY;
        *rz = mob.body.renderZ(partial) - originZ;
        return *rx >= -kLimit && *rx <= kLimit && *ry >= -kLimit && *ry <= kLimit
               && *rz >= -kLimit && *rz <= kLimit;
    };

    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    // A sheep with a fleece is twice the vertices of a shorn one, so the cost
    // is asked per animal rather than assumed.
    const auto cost = [&](int index) {
        ModelPart parts[kMobMaxParts];
        texture::EntitySkin skins[kMobMaxParts];
        return poseMob(system[index], partial, parts, skins, kMobMaxParts) * kBoxVertices;
    };

    DrawCutoff cutoff;
    if (system.count() * kMobVerticesEach > max) {
        cutoff.compute(system.count(), max, place, cost);
    }

    int written = 0;
    for (int index = 0; index < system.count(); ++index) {
        const entity::Mob& mob = system[index];
        double rx = 0.0;
        double ry = 0.0;
        double rz = 0.0;
        if (!place(index, &rx, &ry, &rz)) {
            continue;
        }

        ModelPart parts[kMobMaxParts];
        texture::EntitySkin skins[kMobMaxParts];
        const int count = poseMob(mob, partial, parts, skins, kMobMaxParts);
        if (count == 0) {
            continue;
        }
        const int vertices = count * kBoxVertices;
        if (!cutoff.admit(rx, ry, rz, vertices) || written + vertices > max) {
            continue;
        }

        const Placement placement = placeMob(mob, originX, originY, originZ, partial);
        const int before = written;
        for (int part = 0; part < count; ++part) {
            // **Admitted on the whole animal and written a pass at a time**, so
            // the two runs agree about which animals are drawn at all. A shell
            // whose face was cut for want of room would be a blob of jelly with
            // nothing inside it.
            if (!partBelongsTo(mob.type, part, pass)) {
                continue;
            }
            written += buildBox(parts[part], placement, skins[part], mob.light, out + written,
                                max - written);
        }

        // The hurt flash, and a dying animal keeps flashing while it falls.
        if (mob.hurtTime > 0 || mob.deathTime > 0) {
            for (int v = before; v < written; ++v) {
                out[v].g = kHurtChannel;
                out[v].b = kHurtChannel;
            }
        }
    }
    return written;
}

}  // namespace

int buildMobs(const entity::MobSystem& system, double originX, double originY, double originZ,
              float partial, mesh::DetailVertex* out, int max)
{
    return buildMobRun(system, originX, originY, originZ, partial, out, max, MobPass::Solid);
}

int buildMobShells(const entity::MobSystem& system, double originX, double originY,
                   double originZ, float partial, mesh::DetailVertex* out, int max)
{
    return buildMobRun(system, originX, originY, originZ, partial, out, max, MobPass::Shell);
}

int buildMobEyes(const entity::MobSystem& system, double originX, double originY,
                 double originZ, float partial, mesh::DetailVertex* out, int max)
{
    return buildMobRun(system, originX, originY, originZ, partial, out, max, MobPass::Eyes);
}

}  // namespace mc::render
