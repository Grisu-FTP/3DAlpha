// GuiIngame's Survival rows, and the stand-in sheet they are cut from.
//
// The cells and the arithmetic are `lu.a(FZII)V`'s literals; these cases pin
// the parts of the method a player can see go wrong -- how many hearts, which
// half, when they flash, when they shake, and the bubbles.

#include "core/mesh/vertex.hpp"
#include "core/render/hud_mesh.hpp"
#include "core/texture/icon_sheet.hpp"
#include "framework.hpp"

#include <vector>

using namespace mc;
using mc::render::HudInput;
using mc::texture::IconCell;

namespace {

struct Built {
    std::vector<mesh::DetailVertex> v = std::vector<mesh::DetailVertex>(render::kHudMaxVertices);
    int count = 0;
};

Built build(const HudInput& in)
{
    Built b;
    b.count = render::buildHud(in, b.v.data(), render::kHudMaxVertices);
    return b;
}

int texel(i16 units) { return int(units) * texture::kIconSheetEdge / mesh::kUvUnitsPerAtlas; }

// How many quads start at this cell of the sheet.
int quadsOf(const Built& b, IconCell cell)
{
    int n = 0;
    for (int q = 0; q + 3 < b.count; q += 4) {
        if (texel(b.v[q].u) == cell.u && texel(b.v[q].v) == cell.v) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST(full_health_is_ten_containers_and_ten_hearts)
{
    const Built b = build(HudInput{});
    CHECK_EQ(b.count % 4, 0);
    CHECK_EQ(quadsOf(b, texture::kIconHeartContainer), 10);
    CHECK_EQ(quadsOf(b, texture::kIconHeartFull), 10);
    CHECK_EQ(quadsOf(b, texture::kIconHeartHalf), 0);
    // No armour, no water: nothing else.
    CHECK_EQ(b.count, 20 * 4);
}

TEST(odd_health_ends_on_a_half_heart)
{
    HudInput in;
    in.health = 7;
    in.prevHealth = 7;
    const Built b = build(in);
    CHECK_EQ(quadsOf(b, texture::kIconHeartFull), 3);
    CHECK_EQ(quadsOf(b, texture::kIconHeartHalf), 1);
    CHECK_EQ(quadsOf(b, texture::kIconHeartContainer), 10);
}

TEST(the_rows_sit_in_the_two_top_corners_and_the_bubbles_below_the_hearts)
{
    HudInput in;
    in.armour = 20;
    in.eyeInWater = true;
    const Built b = build(in);

    // The first quad of a slot is the armour piece, then the heart container:
    // armour hangs off the right edge, hearts off the left, both at the margin.
    // **Vertex units throughout**, because the scale is no longer a whole
    // number of pixels -- a cell is 270 units, which is 16.875 of them.
    const int margin = render::hudPixels(render::kHudMargin);
    const int cell = texture::kIconCellSize * render::kHudTexelUnits;
    CHECK_EQ(int(b.v[0].x), render::hudPixels(400 - render::kHudMargin) - cell);
    CHECK_EQ(int(b.v[0].y), margin);
    CHECK_EQ(int(b.v[4].x), margin);
    CHECK_EQ(int(b.v[4].y), margin);

    // A cell really is drawn kHudTexelUnits per texel of the sheet it cuts.
    CHECK_EQ(int(b.v[5].x) - int(b.v[4].x), cell);

    // Every bubble is below every heart, and in the hearts' own column.
    int bubbles = 0;
    for (int q = 0; q + 3 < b.count; q += 4) {
        if (texel(b.v[q].u) != texture::kIconBubble.u
            || texel(b.v[q].v) != texture::kIconBubble.v) {
            continue;
        }
        ++bubbles;
        CHECK(int(b.v[q].y) >= margin + cell);
        CHECK(int(b.v[q].x) >= margin);
    }
    CHECK_EQ(bubbles, 10);

    // Nothing runs off either edge of the screen.
    for (int q = 0; q < b.count; ++q) {
        CHECK(int(b.v[q].x) >= 0);
        CHECK(int(b.v[q].x) <= render::hudPixels(400));
    }

    // ...and the two rows do not meet in the middle.
    CHECK(margin + 10 * 8 * render::kHudTexelUnits
          < render::hudPixels(400 - render::kHudMargin) - 10 * 8 * render::kHudTexelUnits);
}

TEST(the_containers_flash_on_every_third_tick_of_a_fresh_window)
{
    HudInput in;
    in.health = 14;
    in.prevHealth = 18;
    // 19 / 3 = 6, even: not flashing.
    in.hurtResistant = 19;
    CHECK_EQ(quadsOf(build(in), texture::kIconHeartContainerFlash), 0);
    // 17 / 3 = 5, odd: flashing, with the lost health outlined.
    in.hurtResistant = 17;
    const Built flashing = build(in);
    CHECK_EQ(quadsOf(flashing, texture::kIconHeartContainerFlash), 10);
    CHECK_EQ(quadsOf(flashing, texture::kIconHeartFlashFull), 9);
    // Below ten the window no longer shows, whatever the division says.
    in.hurtResistant = 9;
    CHECK_EQ(quadsOf(build(in), texture::kIconHeartContainerFlash), 0);
}

TEST(hearts_shake_only_at_four_health_or_less)
{
    HudInput in;
    in.updateCounter = 7;
    const Built steady = build(in);
    for (int q = 0; q < steady.count; q += 4) {
        CHECK_EQ(int(steady.v[q].y), render::hudPixels(render::kHudMargin));
    }

    in.health = 4;
    in.prevHealth = 4;
    bool moved = false;
    for (u32 tick = 0; tick < 20 && !moved; ++tick) {
        in.updateCounter = tick;
        const Built shaking = build(in);
        for (int q = 0; q < shaking.count; q += 4) {
            const int y = int(shaking.v[q].y);
            const int rest = render::hudPixels(render::kHudMargin);
            CHECK(y == rest || y == rest + render::kHudTexelUnits);
            moved = moved || y != rest;
        }
    }
    CHECK(moved);

    // The same tick shakes the same way, which is what keeps the two eyes
    // agreeing.
    in.updateCounter = 3;
    const Built a = build(in);
    const Built b2 = build(in);
    for (int q = 0; q < a.count; ++q) {
        CHECK_EQ(int(a.v[q].y), int(b2.v[q].y));
    }
}

TEST(armour_fills_from_the_right_in_the_same_halves)
{
    HudInput in;
    in.armour = 13;
    const Built b = build(in);
    CHECK_EQ(quadsOf(b, texture::kIconArmourFull), 6);
    CHECK_EQ(quadsOf(b, texture::kIconArmourHalf), 1);
    CHECK_EQ(quadsOf(b, texture::kIconArmourEmpty), 3);
}

TEST(bubbles_show_only_under_water)
{
    HudInput in;
    in.air = 150;
    CHECK_EQ(quadsOf(build(in), texture::kIconBubble), 0);
    in.eyeInWater = true;
    const Built b = build(in);
    // ceil(148 * 10 / 300) = 5 whole; ceil(150 * 10 / 300) = 5, so none popping.
    CHECK_EQ(quadsOf(b, texture::kIconBubble), 5);
    CHECK_EQ(quadsOf(b, texture::kIconBubblePopping), 0);
    in.air = 91;
    const Built popping = build(in);
    // ceil(89 / 30) = 3 whole, ceil(91 / 30) = 4: one popping.
    CHECK_EQ(quadsOf(popping, texture::kIconBubble), 3);
    CHECK_EQ(quadsOf(popping, texture::kIconBubblePopping), 1);
    // Drowning: nothing left to show.
    in.air = -5;
    CHECK_EQ(quadsOf(build(in), texture::kIconBubble), 0);
}

TEST(the_icon_stand_in_paints_every_cell_the_overlay_cuts)
{
    std::vector<u8> sheet;
    texture::buildIconStandIn(&sheet);
    CHECK_EQ(sheet.size(), texture::kIconSheetBytes);
    const IconCell cells[] = {
        texture::kIconHeartContainer, texture::kIconHeartContainerFlash, texture::kIconHeartFull,
        texture::kIconHeartHalf,      texture::kIconHeartFlashFull,      texture::kIconHeartFlashHalf,
        texture::kIconArmourEmpty,    texture::kIconArmourHalf,          texture::kIconArmourFull,
        texture::kIconBubble,         texture::kIconBubblePopping,
    };
    for (const IconCell& cell : cells) {
        int opaque = 0;
        for (int y = 0; y < texture::kIconCellSize; ++y) {
            for (int x = 0; x < texture::kIconCellSize; ++x) {
                const usize i = (usize(cell.v + y) * texture::kIconSheetEdge + usize(cell.u + x)) * 4;
                opaque += sheet[i + 3] != 0 ? 1 : 0;
            }
        }
        CHECK(opaque > 0);
    }
    // And nothing outside them: the corner of the sheet is clear.
    CHECK_EQ(int(sheet[(usize(200) * texture::kIconSheetEdge + 200) * 4 + 3]), 0);
}
