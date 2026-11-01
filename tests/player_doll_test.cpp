// The inventory's player: where it stands, which way it faces and what it samples.

#include "framework.hpp"

#include "core/gui/paint.hpp"
#include "core/gui/player_doll.hpp"
#include "core/texture/entity_skins.hpp"

#include <vector>

using namespace mc;

namespace {

constexpr int kW = 80;
constexpr int kH = 80;
constexpr int kFootX = 40;
constexpr int kFootY = 70;
constexpr float kScale = 30.0f;
constexpr gui::Pixel kClear = gui::rgb565(1, 2, 3);

struct Canvas {
    std::vector<gui::Pixel> pixels = std::vector<gui::Pixel>(kW * kH, kClear);
    gui::Surface surface() { return gui::Surface{pixels.data(), 1, kW, kW, kH}; }
    gui::Pixel at(int x, int y) const { return pixels[usize(y) * kW + usize(x)]; }
};

// The whole sheet green, with the front of the player's head -- (8, 8) to (16, 16) on
// the skin page -- pure red, so a pixel says which face of which box it came from.
std::vector<u8> markedSheet()
{
    std::vector<u8> sheet(texture::kEntitySheetBytes);
    for (usize i = 0; i < sheet.size(); i += 4) {
        sheet[i] = 0;
        sheet[i + 1] = 255;
        sheet[i + 2] = 0;
        sheet[i + 3] = 255;
    }
    int pageX = 0;
    int pageY = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &pageX, &pageY);
    for (int y = 8; y < 16; ++y) {
        for (int x = 8; x < 16; ++x) {
            const usize at = (usize(pageY + y) * texture::kEntitySheetWidth + usize(pageX + x)) * 4;
            sheet[at] = 255;
            sheet[at + 1] = 0;
        }
    }
    // The hat's cells, (32, 0) to (64, 16), are clear, as they are on most skins:
    // an opaque hat would cover the face this looks for.
    for (int y = 0; y < 16; ++y) {
        for (int x = 32; x < 64; ++x) {
            sheet[(usize(pageY + y) * texture::kEntitySheetWidth + usize(pageX + x)) * 4 + 3] = 0;
        }
    }
    return sheet;
}

bool isRed(gui::Pixel p)
{
    return (p >> 11) > 8 && ((p >> 5) & 0x3F) == 0;
}

}  // namespace

TEST(the_doll_stands_on_its_feet_about_two_blocks_tall)
{
    const std::vector<u8> sheet = markedSheet();
    gui::DollSheets sheets;
    sheets.entity = sheet.data();
    Canvas canvas;
    gui::drawPlayerDoll(canvas.surface(), kFootX, kFootY, kScale, 0, 0, kW, kH, sheets,
                        gui::DollPose{});
    int top = kH;
    int bottom = -1;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (canvas.at(x, y) != kClear) {
                top = y < top ? y : top;
                bottom = y > bottom ? y : bottom;
            }
        }
    }
    // Feet on the line, within a pixel of rounding.
    CHECK(bottom >= kFootY - 2 && bottom <= kFootY);
    // 32 model pixels are two blocks, plus the hat's half pixel of growth.
    const int tall = bottom - top + 1;
    CHECK(tall >= int(kScale * 1.9f) && tall <= int(kScale * 2.1f));
}

TEST(the_doll_shows_its_face_to_the_viewer_and_turns_it_the_way_it_is_told)
{
    const std::vector<u8> sheet = markedSheet();
    gui::DollSheets sheets;
    sheets.entity = sheet.data();
    const auto redCentre = [&](float yaw) {
        Canvas canvas;
        gui::DollPose pose;
        pose.bodyYaw = yaw;
        gui::drawPlayerDoll(canvas.surface(), kFootX, kFootY, kScale, 0, 0, kW, kH, sheets, pose);
        int sum = 0;
        int count = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                if (isRed(canvas.at(x, y))) {
                    sum += x;
                    ++count;
                }
            }
        }
        return count > 0 ? float(sum) / float(count) : -1000.0f;
    };
    // Straight on, the face is in the middle.
    CHECK(std::abs(redCentre(0.0f) - float(kFootX)) < 2.0f);
    // A positive yaw turns it to the right of the surface, and a negative one left.
    CHECK(redCentre(30.0f) > float(kFootX) + 1.0f);
    CHECK(redCentre(-30.0f) < float(kFootX) - 1.0f);
}

TEST(the_doll_writes_nothing_outside_its_clip_and_nothing_without_a_sheet)
{
    const std::vector<u8> sheet = markedSheet();
    gui::DollSheets sheets;
    sheets.entity = sheet.data();
    Canvas clipped;
    gui::drawPlayerDoll(clipped.surface(), kFootX, kFootY, kScale, 30, 30, 20, 20, sheets,
                        gui::DollPose{});
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (x < 30 || x >= 50 || y < 30 || y >= 50) {
                CHECK_EQ(clipped.at(x, y), kClear);
            }
        }
    }
    Canvas empty;
    gui::drawPlayerDoll(empty.surface(), kFootX, kFootY, kScale, 0, 0, kW, kH, gui::DollSheets{},
                        gui::DollPose{});
    for (gui::Pixel p : empty.pixels) {
        CHECK_EQ(p, kClear);
    }
}

TEST(worn_armour_is_drawn_over_the_body_from_its_own_sheet)
{
    const std::vector<u8> sheet = markedSheet();
    std::vector<u8> armour(texture::kArmourSheetBytes);
    for (usize i = 0; i < armour.size(); i += 4) {
        armour[i] = 0;
        armour[i + 1] = 0;
        armour[i + 2] = 255;
        armour[i + 3] = 255;
    }
    gui::DollSheets sheets;
    sheets.entity = sheet.data();
    sheets.armour = armour.data();
    const auto blues = [&](int helmet) {
        Canvas canvas;
        gui::DollPose pose;
        pose.armour[0] = helmet;
        gui::drawPlayerDoll(canvas.surface(), kFootX, kFootY, kScale, 0, 0, kW, kH, sheets, pose);
        int count = 0;
        for (gui::Pixel p : canvas.pixels) {
            count += (p & 0x1F) > 8 && ((p >> 5) & 0x3F) == 0 && (p >> 11) == 0 ? 1 : 0;
        }
        return count;
    };
    CHECK_EQ(blues(-1), 0);
    CHECK(blues(0) > 0);
}
