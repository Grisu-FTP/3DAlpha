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
