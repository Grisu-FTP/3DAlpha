#include "framework.hpp"
#include "texture_support.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/font.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mc;
using mc::test::makeRgbaPng;
using mc::test::TestZip;
using texture::FontImage;
using texture::kFontCellPixels;
using texture::kFontEdge;
using texture::PackError;

namespace {

// Real files rather than a stub, for the reason pack_test gives: the bugs at
// this layer are paths and archives, and neither exists against a stub.
struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_font_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }

    std::string at(const char* name) const { return std::string(path) + "/" + name; }
};

// A sheet where every glyph is blank except the ones a test fills in.
struct Sheet {
    int edge;
    int cell;
    std::vector<u8> rgba;

    explicit Sheet(int edgePixels = kFontEdge)
        : edge(edgePixels), cell(edgePixels / 16),
          rgba(usize(edgePixels) * usize(edgePixels) * 4, 0)
    {
    }

    // Fills columns [0, columns) of a glyph's cell with one colour, full height.
    void fill(int glyph, int columns, u8 r, u8 g, u8 b, u8 a = 255)
    {
        const int cellX = (glyph % 16) * cell;
        const int cellY = (glyph / 16) * cell;
        for (int y = 0; y < cell; ++y) {
            for (int x = 0; x < columns; ++x) {
                u8* p = rgba.data() + (usize(cellY + y) * usize(edge) + usize(cellX + x)) * 4;
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = a;
            }
        }
    }

    std::vector<u8> png() const { return makeRgbaPng(edge, edge, rgba); }
};

std::vector<u8> packWithFont(const Sheet& sheet)
{
    TestZip zip;
    zip.add("default.png", sheet.png(), /*deflate=*/true, /*dataDescriptor=*/false);
    return zip.finish();
}

}  // namespace

// ---------------------------------------------------------------------------
// The width scan, which is the one rule with a measurement behind it
// ---------------------------------------------------------------------------

TEST(font_width_is_last_occupied_column_plus_two)
{
    Sheet sheet;
    sheet.fill('A', 5, 255, 255, 255);  // columns 0..4, so the last is 4
    sheet.fill('B', 1, 255, 255, 255);  // only column 0
    sheet.fill('C', 8, 255, 255, 255);  // the whole cell

    u8 widths[256] = {};
    texture::measureGlyphWidths(sheet.rgba.data(), widths);

    CHECK_EQ(int(widths['A']), 6);
    CHECK_EQ(int(widths['B']), 2);
    CHECK_EQ(int(widths['C']), 9);
}

TEST(font_empty_glyph_advances_one)
{
    Sheet sheet;
    u8 widths[256] = {};
    texture::measureGlyphWidths(sheet.rgba.data(), widths);

    // The scan runs off the left edge to -1, and -1 + 2 is 1.
    CHECK_EQ(int(widths['Z']), 1);
}

TEST(font_space_is_forced_to_four)
{
    Sheet sheet;
    // Even a space somebody drew something in comes out at 4: the original
    // overrides the scan rather than trusting it.
    sheet.fill(' ', 7, 255, 255, 255);

    u8 widths[256] = {};
    texture::measureGlyphWidths(sheet.rgba.data(), widths);

    CHECK_EQ(int(widths[' ']), 4);
}

TEST(font_width_reads_blue_not_alpha)
{
    // a1.1.2 tests `pixel & 255` of an ARGB int, which is blue. A glyph drawn
    // in pure red is opaque and still measures as empty. Faithful, not correct.
    Sheet sheet;
    sheet.fill('R', 6, 255, 0, 0, 255);
    sheet.fill('G', 6, 0, 255, 0, 255);
    sheet.fill('U', 6, 0, 0, 1, 255);  // one unit of blue is enough

    u8 widths[256] = {};
    texture::measureGlyphWidths(sheet.rgba.data(), widths);

    CHECK_EQ(int(widths['R']), 1);
    CHECK_EQ(int(widths['G']), 1);
    CHECK_EQ(int(widths['U']), 7);
}

// ---------------------------------------------------------------------------
// Characters to glyphs
// ---------------------------------------------------------------------------

TEST(font_ascii_maps_to_itself)
{
    CHECK_EQ(texture::fontGlyph(' '), 32);
    CHECK_EQ(texture::fontGlyph('A'), 65);
    CHECK_EQ(texture::fontGlyph('z'), 122);
    CHECK_EQ(texture::fontGlyph('~'), 126);
}

TEST(font_upper_half_follows_the_original_string)
{
    CHECK_EQ(texture::fontGlyph(0x2302), 127);  // the house, where CP437 puts it
    CHECK_EQ(texture::fontGlyph(0x00C7), 128);  // C-cedilla
    CHECK_EQ(texture::fontGlyph(0x00BB), 175);  // the last one there is
}

TEST(font_rejects_what_the_original_cannot_draw)
{
    CHECK_EQ(texture::fontGlyph('`'), -1);      // the string repeats ' instead
    CHECK_EQ(texture::fontGlyph(0x00A7), -1);   // the colour marker itself
    CHECK_EQ(texture::fontGlyph(0x4E2D), -1);   // anything outside the sheet
    CHECK_EQ(texture::fontGlyph(0x00E5 + 0x2000), -1);
}

TEST(font_apostrophe_wins_the_first_match)
{
    // The original's string carries ' twice -- at 7 and at 64 -- and indexOf
    // returns the first, so cell 96 is unreachable. Reproduced deliberately.
    CHECK_EQ(texture::fontGlyph('\''), 39);
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

TEST(font_decodes_utf8)
{
    const std::string text = "A\xC3\xA9\xE2\x8C\x82";  // A, e-acute, house
    usize pos = 0;
    CHECK_EQ(int(texture::nextCodepoint(text, &pos)), int('A'));
    CHECK_EQ(int(pos), 1);
    CHECK_EQ(int(texture::nextCodepoint(text, &pos)), 0x00E9);
    CHECK_EQ(int(pos), 3);
    CHECK_EQ(int(texture::nextCodepoint(text, &pos)), 0x2302);
    CHECK_EQ(int(pos), 6);
}

TEST(font_malformed_utf8_always_advances)
{
    // A name off a card can be anything. What matters is that the caller's loop
    // ends, not what a broken sequence draws as.
    const std::string text = "\xFF\xC3";
    usize pos = 0;
    usize guard = 0;
    while (pos < text.size() && guard < 8) {
        texture::nextCodepoint(text, &pos);
        ++guard;
    }
    CHECK_EQ(int(pos), int(text.size()));
}

// ---------------------------------------------------------------------------
// Measuring, colour codes and clipping
// ---------------------------------------------------------------------------

TEST(font_text_width_adds_advances)
{
    FontImage font;
    font.rgba.assign(texture::kFontBytes, 0);
    for (int i = 0; i < 256; ++i) {
        font.widths[i] = 6;
    }

    CHECK_EQ(texture::textWidth(font.widths, "abc"), 18);
    // Colour codes cost nothing and consume both characters.
    CHECK_EQ(texture::textWidth(font.widths, "\xC2\xA7" "4abc"), 18);
    // A character the sheet has no glyph for costs nothing either.
    CHECK_EQ(texture::textWidth(font.widths, "a`c"), 12);
}

TEST(font_fit_bytes_clips_on_a_character_boundary)
{
    FontImage font;
    font.rgba.assign(texture::kFontBytes, 0);
    for (int i = 0; i < 256; ++i) {
        font.widths[i] = 6;
    }

    CHECK_EQ(int(texture::fitBytes(font.widths, "abcdef", 18)), 3);
    CHECK_EQ(int(texture::fitBytes(font.widths, "abcdef", 17)), 2);
    CHECK_EQ(int(texture::fitBytes(font.widths, "abcdef", 600)), 6);
    // Two bytes per character, and neither is ever cut in half.
    CHECK_EQ(int(texture::fitBytes(font.widths, "\xC3\xA9\xC3\xA9", 6)), 2);
}

TEST(font_colours_are_the_originals)
{
    CHECK_EQ(int(texture::fontColour(0)), 0x000000);
    CHECK_EQ(int(texture::fontColour(1)), 0x0000AA);
    CHECK_EQ(int(texture::fontColour(6)), 0xFFAA00);  // gold, the odd one
    CHECK_EQ(int(texture::fontColour(7)), 0xAAAAAA);
    CHECK_EQ(int(texture::fontColour(8)), 0x555555);
    CHECK_EQ(int(texture::fontColour(15)), 0xFFFFFF);
}

TEST(font_unknown_colour_code_is_white)
{
    CHECK_EQ(texture::colourCodeIndex('0'), 0);
    CHECK_EQ(texture::colourCodeIndex('a'), 10);
    CHECK_EQ(texture::colourCodeIndex('F'), 15);
    CHECK_EQ(texture::colourCodeIndex('z'), 15);
}

TEST(font_shadow_is_a_quarter_of_each_channel)
{
    CHECK_EQ(int(texture::shadowColour(0xFFFFFFFFu)), int(0xFF3F3F3Fu));
    CHECK_EQ(int(texture::shadowColour(0xFF00AA00u)), int(0xFF002A00u));
    // The same rule the original applies to its own palette's shadow half.
    CHECK_EQ(int(texture::shadowColour(0xFF000000u | texture::fontColour(15))),
             int(0xFF000000u | (texture::fontColour(15) & 0xFCFCFCu) >> 2));
}

// ---------------------------------------------------------------------------
// Loading one out of a pack
// ---------------------------------------------------------------------------

TEST(font_loads_from_a_zip_pack)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    Sheet sheet;
    sheet.fill('A', 5, 255, 255, 255);
    const std::string path = dir.at("pack.zip");

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(path.c_str(), packWithFont(sheet)));

    FontImage font;
    CHECK_EQ(int(texture::buildFont(fs, path, &font)), int(PackError::Ok));
    CHECK_EQ(int(font.rgba.size()), int(texture::kFontBytes));
    CHECK_EQ(font.sourceEdge, kFontEdge);
    CHECK_EQ(int(font.widths['A']), 6);
}

TEST(font_loads_from_a_directory_pack)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    Sheet sheet;
    sheet.fill('A', 3, 255, 255, 255);

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(dir.at("default.png").c_str(), sheet.png()));

    FontImage font;
    CHECK_EQ(int(texture::buildFont(fs, dir.path, &font)), int(PackError::Ok));
    CHECK_EQ(int(font.widths['A']), 4);
}

TEST(font_hd_sheet_is_scaled_to_the_cell_size)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    // 256x256, so 16-pixel cells. a1.1.2 would read this as a garbled 128x128;
    // scaling first is the one deviation this file makes.
    Sheet sheet(256);
    sheet.fill('A', 16, 255, 255, 255);
    const std::string path = dir.at("hd.zip");

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(path.c_str(), packWithFont(sheet)));

    FontImage font;
    CHECK_EQ(int(texture::buildFont(fs, path, &font)), int(PackError::Ok));
    CHECK_EQ(font.sourceEdge, 256);
    CHECK_EQ(int(font.rgba.size()), int(texture::kFontBytes));
    // Sixteen source columns are eight scaled ones, so the cell is full.
    CHECK_EQ(int(font.widths['A']), kFontCellPixels + 1);
}

TEST(font_pack_without_one_is_not_an_error_worth_a_name)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    TestZip zip;
    zip.add("terrain.png", mc::test::makeTerrainPng(256), true, false);
    const std::string path = dir.at("bare.zip");

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(path.c_str(), zip.finish()));

    FontImage font;
    CHECK_EQ(int(texture::buildFont(fs, path, &font)), int(PackError::NotFound));
    CHECK(font.empty());
}

TEST(font_dev_art_has_none)
{
    io::PosixFileSystem fs;
    FontImage font;
    CHECK_EQ(int(texture::buildFont(fs, "", &font)), int(PackError::NotFound));
    CHECK(font.empty());
}

TEST(font_refuses_a_sheet_that_is_not_a_grid)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    io::PosixFileSystem fs;

    std::vector<u8> oblong(usize(128) * 64 * 4, 0);
    CHECK(fs.writeFileAtomic(dir.at("default.png").c_str(), makeRgbaPng(128, 64, oblong)));
    FontImage font;
    CHECK_EQ(int(texture::buildFont(fs, dir.path, &font)), int(PackError::NotSquare));

    std::vector<u8> odd(usize(130) * 130 * 4, 0);
    CHECK(fs.writeFileAtomic(dir.at("default.png").c_str(), makeRgbaPng(130, 130, odd)));
    CHECK_EQ(int(texture::buildFont(fs, dir.path, &font)), int(PackError::NotTileGrid));
}
