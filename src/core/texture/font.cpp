#include "core/texture/font.hpp"

#include <cstring>

namespace mc::texture {

namespace {

// The file the original's Minecraft constructor names.
constexpr char kFontName[] = "default.png";

// a1.1.2's character string, as code points, read out of `kd.class`'s constant
// pool rather than retyped. Position 0 is the space, and glyph = position + 32.
//
// Two things in it look like mistakes and are not: position 64 is an apostrophe
// where a backtick would belong, and the original's `indexOf` therefore never
// reaches cell 96; and position 95 is U+2302, the house glyph CP437 puts at
// 127. Both are reproduced because both are what the original draws.
constexpr u16 kCharacters[kFontGlyphCount] = {
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0027, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
};

// U+00A7. The original compares against the literal 167.
constexpr u32 kSectionSign = 0x00A7;

}  // namespace

void measureGlyphWidths(const u8* sheet, u8* widths)
{
    for (int glyph = 0; glyph < 256; ++glyph) {
        const int cellX = (glyph % kFontGlyphsPerEdge) * kFontCellPixels;
        const int cellY = (glyph / kFontGlyphsPerEdge) * kFontCellPixels;

        // From the right-hand column inwards, stopping at the first column with
        // anything in it. A cell that is entirely empty ends at -1 and so
        // advances 1, which is what the original does with the cells its
        // character string never reaches anyway.
        int column = kFontCellPixels - 1;
        for (; column >= 0; --column) {
            const int x = cellX + column;
            bool empty = true;
            for (int y = 0; y < kFontCellPixels && empty; ++y) {
                const usize texel = (usize(cellY + y) * kFontEdge + usize(x)) * 4;
                // Blue, not alpha. See the note in the header.
                if (sheet[texel + 2] > 0) {
                    empty = false;
                }
            }
            if (!empty) {
                break;
            }
        }

        if (glyph == 32) {
            // The space has nothing in any column, so the scan would make it
            // one pixel wide. The original overrides it here, not earlier.
            column = 2;
        }
        widths[glyph] = u8(column + 2);
    }
}

int fontGlyph(u32 codepoint)
{
    for (int i = 0; i < kFontGlyphCount; ++i) {
        if (kCharacters[i] == codepoint) {
            return kFontFirstGlyph + i;
        }
    }
    return -1;
}

u32 nextCodepoint(std::string_view text, usize* pos)
{
    const usize size = text.size();
    usize i = *pos;
    if (i >= size) {
        *pos = size;
        return 0;
    }

    const u8 lead = u8(text[i]);
    if (lead < 0x80) {
        *pos = i + 1;
        return lead;
    }

    int extra = 0;
    u32 value = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07u;
    } else {
        // A continuation byte or an invalid lead. One byte is consumed so a
        // malformed name cannot stall the caller's loop.
        *pos = i + 1;
        return 0xFFFD;
    }

    if (i + usize(extra) >= size) {
        *pos = size;
        return 0xFFFD;
    }
    for (int n = 1; n <= extra; ++n) {
        const u8 byte = u8(text[i + usize(n)]);
        if ((byte & 0xC0) != 0x80) {
            *pos = i + usize(n);
            return 0xFFFD;
        }
        value = (value << 6) | (byte & 0x3Fu);
    }
    *pos = i + usize(extra) + 1;
    return value;
}

int colourCodeIndex(u32 codepoint)
{
    if (codepoint >= '0' && codepoint <= '9') {
        return int(codepoint - '0');
    }
    if (codepoint >= 'a' && codepoint <= 'f') {
        return int(codepoint - 'a') + 10;
    }
    if (codepoint >= 'A' && codepoint <= 'F') {
        return int(codepoint - 'A') + 10;
    }
    return 15;
}

u32 fontColour(int index)
{
    const int i = index & 15;
    const int base = ((i >> 3) & 1) * 85;
    int red = ((i >> 2) & 1) * 170 + base;
    const int green = ((i >> 1) & 1) * 170 + base;
    const int blue = (i & 1) * 170 + base;
    if (i == 6) {
        // Gold, and the one colour the bit pattern does not produce on its own.
        red += 85;
    }
    return (u32(red) << 16) | (u32(green) << 8) | u32(blue);
}

u32 shadowColour(u32 argb)
{
    return (argb & 0xFF000000u) | ((argb & 0x00FCFCFCu) >> 2);
}

int textWidth(const u8* widths, std::string_view text)
{
    int width = 0;
    usize i = 0;
    while (i < text.size()) {
        const u32 codepoint = nextCodepoint(text, &i);
        if (codepoint == kSectionSign) {
            // Both characters go, whether or not the second is a hex digit.
            if (i < text.size()) {
                nextCodepoint(text, &i);
            }
            continue;
        }
        const int glyph = fontGlyph(codepoint);
        if (glyph >= 0) {
            width += widths[glyph];
        }
    }
    return width;
}

void wrapText(const u8* widths, std::string_view text, int maxWidth,
              std::vector<std::string>* lines)
{
    lines->clear();

    std::string line;
    std::string colour;
    std::string colourAtSpace;
    int width = 0;
    int widthAtSpace = 0;
    usize spaceAt = std::string::npos;
    bool drewText = false;

    const auto start = [&](const std::string& carried) {
        line = carried;
        width = 0;
        drewText = false;
        spaceAt = std::string::npos;
    };
    const auto finish = [&]() {
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        lines->push_back(line);
    };

    usize i = 0;
    while (i < text.size()) {
        const usize begin = i;
        const u32 codepoint = nextCodepoint(text, &i);

        if (codepoint == '\n') {
            finish();
            colour.clear();
            start(colour);
            continue;
        }
        if (codepoint == kSectionSign) {
            if (i < text.size()) {
                nextCodepoint(text, &i);
            }
            colour.assign(text.substr(begin, i - begin));
            line.append(text.substr(begin, i - begin));
            continue;
        }

        const int glyph = fontGlyph(codepoint);
        const int advance = glyph >= 0 ? widths[glyph] : 0;
        if (drewText && width + advance > maxWidth) {
            if (codepoint == ' ') {
                // The edge landed on a space: the break is free.
                finish();
                start(colour);
                continue;
            }
            if (spaceAt != std::string::npos) {
                const std::string rest = line.substr(spaceAt + 1);
                const int restWidth = width - widthAtSpace;
                line.resize(spaceAt);
                finish();
                start(colourAtSpace);
                line += rest;
                width = restWidth;
                drewText = restWidth > 0;
            }
            if (drewText && width + advance > maxWidth) {
                // One word wider than the line: cut it here.
                finish();
                start(colour);
            }
        }

        line.append(text.substr(begin, i - begin));
        width += advance;
        // A space is a break only once something is drawn before it, so an
        // indented line never breaks inside its own indentation.
        if (codepoint != ' ') {
            drewText = true;
        } else if (drewText) {
            spaceAt = line.size() - 1;
            widthAtSpace = width;
            colourAtSpace = colour;
        }
    }

    if (text.empty() || text.back() != '\n') {
        finish();
    }
}

usize fitBytes(const u8* widths, std::string_view text, int maxWidth)
{
    int width = 0;
    usize i = 0;
    usize fits = 0;
    while (i < text.size()) {
        const u32 codepoint = nextCodepoint(text, &i);
        if (codepoint == kSectionSign) {
            if (i < text.size()) {
                nextCodepoint(text, &i);
            }
            fits = i;
            continue;
        }
        const int glyph = fontGlyph(codepoint);
        if (glyph >= 0) {
            if (width + widths[glyph] > maxWidth) {
                return fits;
            }
            width += widths[glyph];
        }
        fits = i;
    }
    return fits;
}

PackError buildFont(io::FileSystem& fs, std::string_view packPath, FontImage* out)
{
    out->rgba.clear();
    out->sourceEdge = 0;
    std::memset(out->widths, 0, sizeof(out->widths));

    if (packPath.empty()) {
        // Dev Art has no font. There is generated block art because sixteen
        // flat colours are a texture; ninety-six legible glyphs are not.
        return PackError::NotFound;
    }

    std::vector<u8> png;
    const PackError read = readPackFile(fs, packPath, kFontName, &png);
    if (read != PackError::Ok) {
        return read;
    }

    Image image;
    const PngError error = decodePng(png, &image, kMaxFontPixels);
    if (error == PngError::TooLarge) {
        return PackError::TooLarge;
    }
    if (error != PngError::Ok) {
        return PackError::BadPng;
    }
    if (image.width != image.height) {
        return PackError::NotSquare;
    }
    if (image.width % kFontGlyphsPerEdge != 0) {
        return PackError::NotTileGrid;
    }

    out->sourceEdge = image.width;
    scaleSquare(image, kFontEdge, &out->rgba);
    measureGlyphWidths(out->rgba.data(), out->widths);
    return PackError::Ok;
}

}  // namespace mc::texture
