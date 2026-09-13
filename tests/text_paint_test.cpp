// Pack-font text drawn into a software surface, and the console font turned
// into one. See core/gui/text.hpp.

#include "framework.hpp"

#include "core/gui/text.hpp"

#include <vector>

using namespace mc;

namespace {

struct Canvas {
    std::vector<gui::Pixel> pixels;
    gui::Surface surface;

    Canvas(int width, int height) : pixels(usize(width) * usize(height), 0)
    {
        surface.pixels = pixels.data();
        surface.strideX = 1;
        surface.strideY = width;
        surface.width = width;
        surface.height = height;
    }

    gui::Pixel at(int x, int y) const { return pixels[usize(y * surface.width + x)]; }
};

// A font whose 'A' is one white texel in the top-left corner of its cell, three
// pixels wide.
texture::FontImage oneDotFont()
{
    texture::FontImage font;
    font.rgba.assign(texture::kFontBytes, 0);
    const int glyph = texture::fontGlyph('A');
    const int cellX = (glyph % texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
    const int cellY = (glyph / texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
    const usize texel = (usize(cellY) * texture::kFontEdge + usize(cellX)) * 4;
    for (usize c = 0; c < 4; ++c) {
        font.rgba[texel + c] = 255;
    }
    font.widths[glyph] = 3;
    return font;
}

}  // namespace

TEST(bottom_screen_text_draws_each_glyph_and_its_shadow_one_pixel_down_right)
{
    Canvas canvas(16, 16);
    const texture::FontImage font = oneDotFont();
    const int pen = gui::drawText(canvas.surface, 2, 3, font, "AA", 0xFFFFFF, true);

    CHECK_EQ(pen, 8);
    CHECK_EQ(canvas.at(2, 3), gui::rgb565(0xFFFFFF));
    CHECK_EQ(canvas.at(5, 3), gui::rgb565(0xFFFFFF));
    CHECK_EQ(canvas.at(3, 4), gui::rgb565(texture::shadowColour(0xFFFFFF)));
    CHECK_EQ(canvas.at(2, 4), gui::Pixel(0));
}

TEST(bottom_screen_text_takes_the_colour_of_a_colour_code)
{
    Canvas canvas(16, 16);
    const texture::FontImage font = oneDotFont();
    gui::drawText(canvas.surface, 0, 0, font, "A§cA", 0xFFFFFF, false);

    CHECK_EQ(canvas.at(0, 0), gui::rgb565(0xFFFFFF));
    CHECK_EQ(canvas.at(3, 0), gui::rgb565(texture::fontColour(12)));
}

TEST(bottom_screen_text_with_no_font_draws_nothing)
{
    Canvas canvas(8, 8);
    const texture::FontImage empty;
    CHECK_EQ(gui::drawText(canvas.surface, 1, 1, empty, "A", 0xFFFFFF, true), 1);
    CHECK_EQ(canvas.at(1, 1), gui::Pixel(0));
}

TEST(a_one_bit_console_font_loses_its_shared_left_margin_and_is_measured)
{
    // libctru's 'L': a two-pixel stroke starting in column 2, and a foot.
    u8 table[256 * 8] = {};
    const u8 letter[8] = {0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3F, 0x00};
    for (int row = 0; row < 8; ++row) {
        table['L' * 8 + row] = letter[row];
    }

    texture::FontImage font;
    gui::fontFromBitmap(table, 0, 256, &font);
    CHECK(!font.empty());

    const int glyph = texture::fontGlyph('L');
    const int cellX = (glyph % texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
    const int cellY = (glyph / texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
    const auto alpha = [&](int col, int row) {
        return font.rgba[(usize(cellY + row) * texture::kFontEdge + usize(cellX + col)) * 4 + 3];
    };
    CHECK_EQ(alpha(0, 0), u8(255));
    CHECK_EQ(alpha(2, 0), u8(0));
    CHECK_EQ(alpha(5, 6), u8(255));
    // Last occupied column 5, plus two.
    CHECK_EQ(font.widths[glyph], u8(7));
}
