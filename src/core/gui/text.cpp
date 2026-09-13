#include "core/gui/text.hpp"

#include <algorithm>

namespace mc::gui {

namespace {

constexpr u32 kSectionSign = 0x00A7;

u8 scale(u8 texel, u32 channel)
{
    return u8((u32(texel) * (channel & 0xFF)) / 255u);
}

int drawGlyphs(const Surface& surface, int x, int y, const texture::FontImage& font,
               std::string_view text, u32 rgb, bool shadowPass)
{
    u32 colour = shadowPass ? texture::shadowColour(rgb) : rgb;
    int pen = x;
    usize i = 0;
    while (i < text.size()) {
        const u32 codepoint = texture::nextCodepoint(text, &i);
        if (codepoint == kSectionSign && i < text.size()) {
            const u32 next = texture::fontColour(
                texture::colourCodeIndex(texture::nextCodepoint(text, &i)));
            colour = shadowPass ? texture::shadowColour(next) : next;
            continue;
        }

        const int glyph = texture::fontGlyph(codepoint);
        if (glyph < 0) {
            continue;
        }

        const int cellX = (glyph % texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
        const int cellY = (glyph / texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
        for (int row = 0; row < texture::kFontCellPixels; ++row) {
            for (int col = 0; col < texture::kFontCellPixels; ++col) {
                const usize texel =
                    (usize(cellY + row) * texture::kFontEdge + usize(cellX + col)) * 4;
                if (font.rgba[texel + 3] < 128) {
                    continue;
                }
                Pixel* out = surface.at(pen + col, y + row);
                if (out == nullptr) {
                    continue;
                }
                *out = rgb565(scale(font.rgba[texel], colour >> 16),
                              scale(font.rgba[texel + 1], colour >> 8),
                              scale(font.rgba[texel + 2], colour));
            }
        }
        pen += font.widths[glyph];
    }
    return pen;
}

}  // namespace

int drawText(const Surface& surface, int x, int y, const texture::FontImage& font,
             std::string_view text, u32 rgb, bool shadow)
{
    if (font.empty()) {
        return x;
    }
    if (shadow) {
        drawGlyphs(surface, x + 1, y + 1, font, text, rgb, true);
    }
    return drawGlyphs(surface, x, y, font, text, rgb, false);
}

void fontFromBitmap(const u8* table, int first, int count, texture::FontImage* out)
{
    out->rgba.assign(texture::kFontBytes, 0);
    out->sourceEdge = texture::kFontEdge;

    const int begin = std::max(first, 0);
    const int end = std::min(first + count, 256);

    // The columns every printable ASCII glyph leaves empty on its left. Only
    // those: a console font's box-drawing glyphs fill their whole cell, and
    // counting them would keep the margin on every letter.
    u8 used = 0;
    for (int code = std::max(begin, 33); code < std::min(end, 127); ++code) {
        const u8* rows = table + usize(code - first) * 8;
        for (int row = 0; row < 8; ++row) {
            used |= rows[row];
        }
    }
    int shift = 0;
    while (shift < 7 && (used & (0x80 >> shift)) == 0) {
        ++shift;
    }

    for (int code = begin; code < end; ++code) {
        const u8* rows = table + usize(code - first) * 8;
        const int cellX = (code % texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
        const int cellY = (code / texture::kFontGlyphsPerEdge) * texture::kFontCellPixels;
        for (int row = 0; row < 8; ++row) {
            const u8 bits = u8(rows[row] << shift);
            for (int col = 0; col < 8; ++col) {
                if ((bits & (0x80 >> col)) == 0) {
                    continue;
                }
                const usize texel =
                    (usize(cellY + row) * texture::kFontEdge + usize(cellX + col)) * 4;
                out->rgba[texel] = 255;
                out->rgba[texel + 1] = 255;
                out->rgba[texel + 2] = 255;
                out->rgba[texel + 3] = 255;
            }
        }
    }

    texture::measureGlyphWidths(out->rgba.data(), out->widths);
}

}  // namespace mc::gui
