// GuiIngame's Survival rows. See hud_mesh.hpp.

#include "core/render/hud_mesh.hpp"

#include "core/texture/icon_sheet.hpp"
#include "core/util/java_random.hpp"

#include <cmath>

namespace mc::render {

namespace {

using texture::IconCell;

// The sheet's own numbers, in texels, before kHudTexelUnits is applied to any
// of them: a 9 x 9 cell stepped eight along, which is the one-pixel overlap
// that makes a row of hearts touch.
constexpr int kCell = texture::kIconCellSize;
constexpr int kSlots = 10;
constexpr int kSpacing = 8;

// **Everything below is in vertex units, not pixels**, which is the whole of
// what lets the row be scaled by a fraction without anything rounding: a texel
// is 30 units, a cell 270 and a step 240, and none of those is a division.
constexpr int kCellUnits = kCell * kHudTexelUnits;
constexpr int kStepUnits = kSpacing * kHudTexelUnits;

struct Writer {
    mesh::DetailVertex* out;
    int max;
    int written = 0;

    // One 9 x 9 cell of the sheet, drawn kHudTexelUnits per texel with its top
    // left corner at (x, y) in vertex units. The chat builder's corner order.
    void quad(int x, int y, IconCell cell)
    {
        if (written + 4 > max) {
            return;
        }
        constexpr int corner[4][2] = {{0, 0}, {kCell, 0}, {kCell, kCell}, {0, kCell}};
        for (int c = 0; c < 4; ++c) {
            mesh::DetailVertex& v = out[written++];
            v.x = i16(x + corner[c][0] * kHudTexelUnits);
            v.y = i16(y + corner[c][1] * kHudTexelUnits);
            v.z = 0;
            v.face = 0;
            v.u = i16((int(cell.u) + corner[c][0]) * mesh::kUvUnitsPerAtlas
                      / texture::kIconSheetEdge);
            v.v = i16((int(cell.v) + corner[c][1]) * mesh::kUvUnitsPerAtlas
                      / texture::kIconSheetEdge);
            v.r = 255;
            v.g = 255;
            v.b = 255;
            v.light = 0xFF;
        }
    }
};

}  // namespace

int buildHud(const HudInput& in, mesh::DetailVertex* out, int maxVertices)
{
    Writer w{out, maxVertices};

    // The two corners the rows hang from. See the header for why these are the
    // corners and not the original's bottom centre.
    const int rowY = hudPixels(kHudMargin);
    const int heartsX = hudPixels(kHudMargin);
    const int armourX = hudPixels(in.screenWidth - kHudMargin) - kCellUnits;

    // `boolean flag = player.heartsLife / 3 % 2 == 1; if (heartsLife < 10) flag = false;`
    bool flash = (in.hurtResistant / 3) % 2 == 1;
    if (in.hurtResistant < 10) {
        flash = false;
    }

    // `rand.setSeed((long)(updateCounter * 312871))` -- an int product, wrapped,
    // then widened.
    JavaRandom rand(i64(i32(in.updateCounter * 312871u)));

    for (int k = 0; k < kSlots; ++k) {
        int y = rowY;

        if (in.armour > 0) {
            // **Right to left**, as the original's `left + 91 - i * 8 - 9` is:
            // slot 0 is the outermost piece and the row grows inwards.
            const int x = armourX - k * kStepUnits;
            const int step = k * 2 + 1;
            if (step < in.armour) {
                w.quad(x, y, texture::kIconArmourFull);
            }
            if (step == in.armour) {
                w.quad(x, y, texture::kIconArmourHalf);
            }
            if (step > in.armour) {
                w.quad(x, y, texture::kIconArmourEmpty);
            }
        }

        const int x = heartsX + k * kStepUnits;
        if (in.health <= 4) {
            // One *source* pixel of shake, so it stays the same fraction of a
            // heart however large the row is drawn.
            y += rand.nextInt(2) * kHudTexelUnits;
        }
        w.quad(x, y, flash ? texture::kIconHeartContainerFlash : texture::kIconHeartContainer);
        const int step = k * 2 + 1;
        if (flash) {
            if (step < in.prevHealth) {
                w.quad(x, y, texture::kIconHeartFlashFull);
            }
            if (step == in.prevHealth) {
                w.quad(x, y, texture::kIconHeartFlashHalf);
            }
        }
        if (step < in.health) {
            w.quad(x, y, texture::kIconHeartFull);
        }
        if (step == in.health) {
            w.quad(x, y, texture::kIconHeartHalf);
        }
    }

    if (in.eyeInWater) {
        // `ceil((air - 2) * 10 / 300.0)` whole, and `ceil(air * 10 / 300.0)` less
        // that popping.
        const int whole = int(std::ceil(double(in.air - 2) * 10.0 / 300.0));
        const int popping = int(std::ceil(double(in.air) * 10.0 / 300.0)) - whole;
        // **Below the hearts**, not above them: above is the top of the screen
        // here. The gap is the original's one pixel between the two rows,
        // scaled with everything else.
        const int bubbleY = rowY + kCellUnits + kHudTexelUnits;
        for (int k = 0; k < whole + popping; ++k) {
            const int x = heartsX + k * kStepUnits;
            w.quad(x, bubbleY, k < whole ? texture::kIconBubble : texture::kIconBubblePopping);
        }
    }
    return w.written;
}

}  // namespace mc::render
