// The player's biped and `cr.a(FFFFFF)`'s pose for it, which the Skins
// screen's walking character is drawn with. See core/render/player_model.hpp.

#include "framework.hpp"

#include "core/render/player_model.hpp"

#include <cmath>

using namespace mc;
using namespace mc::render;

namespace {

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-3f;
}

}  // namespace

TEST(player_pose_swings_the_arms_against_the_legs)
{
    ModelPart parts[kBipedParts];
    bipedModel(parts);
    posePlayer(parts, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);

    // cos(0) = 1 and cos(pi) = -1: the right leg forward, the right arm back.
    CHECK(near(parts[kBipedLegRight].angleX, 1.4f));
    CHECK(near(parts[kBipedLegLeft].angleX, -1.4f));
    // The arms are `2 * amount * 0.5` -- one, not the legs' 1.4 -- plus the
    // idle roll, which is sin(0) = 0 here.
    CHECK(near(parts[kBipedArmRight].angleX, -1.0f));
    CHECK(near(parts[kBipedArmLeft].angleX, 1.0f));
    // The idle sway at age 0 is cos(0) * 0.05 + 0.05.
    CHECK(near(parts[kBipedArmRight].angleZ, 0.1f));
    CHECK(near(parts[kBipedArmLeft].angleZ, -0.1f));
    CHECK(near(parts[kBipedArmRight].pivotX, -5.0f));
    CHECK(near(parts[kBipedArmLeft].pivotX, 5.0f));
}

TEST(player_pose_with_no_amount_stands_still_and_the_hat_follows_the_head)
{
    ModelPart parts[kBipedParts];
    bipedModel(parts);
    posePlayer(parts, 37.0f, 0.0f, 0.0f, 30.0f, -10.0f);
    CHECK(near(parts[kBipedLegRight].angleX, 0.0f));
    CHECK(near(parts[kBipedLegLeft].angleX, 0.0f));
    CHECK(near(parts[kBipedHead].angleY, 30.0f / 57.295776f));
    CHECK(near(parts[kBipedHat].angleY, parts[kBipedHead].angleY));
    CHECK(near(parts[kBipedHat].angleX, parts[kBipedHead].angleX));
}

TEST(player_preview_moves_the_uvs_to_the_page_it_is_given)
{
    ModelPart parts[kBipedParts];
    bipedModel(parts);
    const Placement place = placeAt(0.0, 0.0, 0.0, 0.0f);

    mesh::DetailVertex atPlayer[kPlayerPreviewVertices];
    mesh::DetailVertex atOrigin[kPlayerPreviewVertices];
    int originX = 0;
    int originY = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &originX, &originY);

    CHECK_EQ(buildPlayerPreview(parts, place, originX, originY, atPlayer,
                                kPlayerPreviewVertices),
             kPlayerPreviewVertices);
    CHECK_EQ(buildPlayerPreview(parts, place, 0, 32, atOrigin, kPlayerPreviewVertices),
             kPlayerPreviewVertices);

    // Same positions, UVs moved by whole texels of a 256-texel sheet.
    for (int v = 0; v < kPlayerPreviewVertices; ++v) {
        CHECK_EQ(atOrigin[v].x, atPlayer[v].x);
        CHECK_EQ(atOrigin[v].u, i16(atPlayer[v].u - originX * 64));
        CHECK_EQ(atOrigin[v].v, i16(atPlayer[v].v + (32 - originY) * 64));
    }

    // Too small a buffer writes nothing rather than half a player.
    CHECK_EQ(buildPlayerPreview(parts, place, 0, 0, atOrigin, kPlayerPreviewVertices - 1), 0);
}

// **Half way through a swing**, worked by hand from `cr.a(FFFFFF)`'s swing
// block with k = 0.5, standing still, head level and age zero. The body turns
// away and the right arm comes up and across; at k = 0 the block leaves the
// pose exactly as the test above has it.
TEST(player_pose_mid_swing_turns_the_body_and_raises_the_right_arm)
{
    ModelPart parts[kBipedParts];
    bipedModel(parts);
    posePlayer(parts, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f);

    // sin(sqrt(0.5) * 2pi) * 0.2
    CHECK(near(parts[kBipedBody].angleY, -0.19274f));
    CHECK(near(parts[kBipedArmRight].pivotX, -4.9074f));
    CHECK(near(parts[kBipedArmLeft].pivotX, 4.9074f));
    // -(sin((1 - 0.5^4) * pi) * 1.2 + sin(pi / 2) * 0.7 * 0.75)
    CHECK(near(parts[kBipedArmRight].angleX, -0.7591f));
    // sin(pi / 2) * -0.4, then the idle sway's 0.1 on top.
    CHECK(near(parts[kBipedArmRight].angleZ, -0.3f));
    // The body's turn, twice more for the right arm and once for the left.
    CHECK(near(parts[kBipedArmRight].angleY, -0.19274f * 3.0f));
    CHECK(near(parts[kBipedArmLeft].angleY, -0.19274f));

    ModelPart still[kBipedParts];
    bipedModel(still);
    posePlayer(still, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(near(still[kBipedBody].angleY, 0.0f));
    CHECK(near(still[kBipedArmRight].angleX, -1.0f));
}

// **The crouch**, `cr.a(FFFFFF)`'s branch on `j`, standing still with the head
// level and age zero: the body leans half a radian, the arms come 0.4 with it,
// the legs move up to nine and forward to four, and the head drops a pixel --
// but not the hat, whose pivot nothing copies.
TEST(player_pose_crouched_leans_the_body_and_draws_the_legs_up_under_it)
{
    ModelPart parts[kBipedParts];
    bipedModel(parts);
    posePlayer(parts, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, /*sneaking=*/true);

    CHECK(near(parts[kBipedBody].angleX, 0.5f));
    CHECK(near(parts[kBipedArmRight].angleX, 0.4f));
    CHECK(near(parts[kBipedArmLeft].angleX, 0.4f));
    CHECK(near(parts[kBipedLegRight].pivotY, 9.0f));
    CHECK(near(parts[kBipedLegLeft].pivotY, 9.0f));
    CHECK(near(parts[kBipedLegRight].pivotZ, 4.0f));
    CHECK(near(parts[kBipedLegLeft].pivotZ, 4.0f));
    CHECK(near(parts[kBipedHead].pivotY, 1.0f));
    CHECK(near(parts[kBipedHat].pivotY, 0.0f));

    // And standing again, the same parts go back: a pose is written over the
    // last one, never built fresh.
    posePlayer(parts, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, /*sneaking=*/false);
    CHECK(near(parts[kBipedBody].angleX, 0.0f));
    CHECK(near(parts[kBipedArmRight].angleX, 0.0f));
    CHECK(near(parts[kBipedLegRight].pivotY, 12.0f));
    CHECK(near(parts[kBipedLegRight].pivotZ, 0.0f));
    CHECK(near(parts[kBipedHead].pivotY, 0.0f));
}
