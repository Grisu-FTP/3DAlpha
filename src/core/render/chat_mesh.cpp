#include "core/render/chat_mesh.hpp"

#include <cstring>
#include <string_view>

namespace mc::render {
namespace {

constexpr int kCell = texture::kFontCellPixels;
constexpr int kCells = texture::kFontGlyphsPerEdge;

i16 pixelUnits(int pixels)
{
    return i16(pixels * kChatUnitsPerPixel);
}

i16 fontUv(int texels)
{
    return i16(texels * mesh::kUvUnitsPerAtlas / texture::kFontEdge);
}

// `renderString` for one pass: every glyph of `text` at (x, y), in `rgb`.
// A colour code changes the colour for the rest of the line, in the shadow's
// palette when this is the shadow pass. Returns the vertices written, and
// stops short rather than writing past `max`.
int buildString(const texture::FontImage& font, std::string_view text, int x, int y,
                u32 rgb, bool shadow, mesh::DetailVertex* out, int max)
{
    int written = 0;
    int penX = x;
    usize pos = 0;
    while (pos < text.size()) {
        const u32 code = texture::nextCodepoint(text, &pos);
        if (code == 0xA7) {
            if (pos < text.size()) {
                const u32 digit = texture::nextCodepoint(text, &pos);
                const u32 coded = texture::fontColour(texture::colourCodeIndex(digit));
                rgb = shadow ? texture::shadowColour(coded) : coded;
            }
            continue;
        }
        const int glyph = texture::fontGlyph(code);
        if (glyph < 0) {
            continue;
        }
        if (written + 4 > max) {
            break;
        }
        const int u0 = (glyph % kCells) * kCell;
        const int v0 = (glyph / kCells) * kCell;
        const int corner[4][2] = {{0, 0}, {kCell, 0}, {kCell, kCell}, {0, kCell}};
        for (int c = 0; c < 4; ++c) {
            mesh::DetailVertex& v = out[written++];
            v.x = pixelUnits(penX + corner[c][0]);
            v.y = pixelUnits(y + corner[c][1]);
            v.z = 0;
            v.face = 0;
            v.u = fontUv(u0 + corner[c][0]);
            v.v = fontUv(v0 + corner[c][1]);
            v.r = u8(rgb >> 16);
            v.g = u8(rgb >> 8);
            v.b = u8(rgb);
            v.light = 0xFF;
        }
        penX += font.widths[glyph];
    }
    return written;
}

}  // namespace

int buildChatText(const gui::ChatLog& log, const texture::FontImage& font, int screenHeight,
                  mesh::DetailVertex* out, int maxVertices, ChatSpan* spans, int maxSpans)
{
    if (out == nullptr || spans == nullptr || font.empty()) {
        return 0;
    }
    constexpr u32 kWhite = 0xFFFFFF;
    const int base = screenHeight - gui::kChatBottomOffset;

    int written = 0;
    int count = 0;
    for (int i = 0; i < log.count() && i < gui::kChatShownLines && count < maxSpans; ++i) {
        const gui::ChatLine& line = log[i];
        const int alpha = gui::chatOpacity(line.age);
        if (alpha <= 0) {
            continue;
        }
        const int y = base - i * gui::kChatLineSpacing;
        const std::string_view text(line.text, std::strlen(line.text));

        ChatSpan& span = spans[count++];
        span.y = y;
        span.alpha = alpha;
        span.firstVertex = written;
        written += buildString(font, text, gui::kChatLeft + 1, y + 1,
                               texture::shadowColour(kWhite), true, out + written,
                               maxVertices - written);
        written += buildString(font, text, gui::kChatLeft, y, kWhite, false, out + written,
                               maxVertices - written);
        span.vertices = written - span.firstVertex;
    }
    return count;
}

}  // namespace mc::render
