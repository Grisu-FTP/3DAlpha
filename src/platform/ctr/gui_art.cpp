#include "platform/ctr/gui_art.hpp"

#include "core/texture/tiled.hpp"

#include <3ds.h>

#include <cstring>

namespace mc::ctr {

namespace {

using texture::kFontCellPixels;
using texture::kFontEdge;
using texture::kFontGlyphsPerEdge;
using texture::tiledOffset;

// GPU_RGBA8 reads A,B,G,R in memory order, which on this little-endian machine
// is this word. The same reversal the block atlas does, and for the same
// reason: core/texture/ hands over PNG's own R,G,B,A order.
constexpr u32 rgbaWord(u8 r, u8 g, u8 b, u8 a)
{
    return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a;
}

// The original samples 7.99 texels of an 8-texel cell rather than all 8, so a
// glyph can never pick up the first column of the one beside it. Kept, because
// it costs nothing and the failure it prevents -- a sliver of the next letter
// down the right-hand edge of every glyph -- is exactly the kind of thing that
// only shows up on hardware.
constexpr float kCellSample = 7.99f;

// One glyph's cell in the sheet, in citro2d's coordinates: v = 1 is the top of
// the image, which is what the unflipped upload in GuiTexture::init makes true.
Tex3DS_SubTexture glyphCell(int glyph)
{
    const float cellX = float((glyph % kFontGlyphsPerEdge) * kFontCellPixels);
    const float cellY = float((glyph / kFontGlyphsPerEdge) * kFontCellPixels);

    Tex3DS_SubTexture cell{};
    cell.width = u16(kFontCellPixels);
    cell.height = u16(kFontCellPixels);
    cell.left = cellX / float(kFontEdge);
    cell.right = (cellX + kCellSample) / float(kFontEdge);
    cell.top = 1.0f - cellY / float(kFontEdge);
    cell.bottom = 1.0f - (cellY + kCellSample) / float(kFontEdge);
    return cell;
}

}  // namespace

// ---------------------------------------------------------------------------
// GuiTexture
// ---------------------------------------------------------------------------

bool GuiTexture::init(const u8* rgba, int width, int height, bool repeat)
{
    if (ready_ || rgba == nullptr) {
        return false;
    }
    if (!C3D_TexInit(&tex_, u16(width), u16(height), GPU_RGBA8)) {
        return false;
    }

    const usize bytes = usize(width) * usize(height) * 4;
    u32* tiled = static_cast<u32*>(linearAlloc(bytes));
    if (tiled == nullptr) {
        C3D_TexDelete(&tex_);
        return false;
    }

    // **No vertical flip here, unlike the block atlas.** The atlas is sampled
    // by UVs the mesher computes, which put tile 0 at v ~ 0; these two are
    // sampled by subtextures written below in citro2d's convention, where the
    // top of the image is v = 1. Since v = 0 reads the last row in memory
    // (core/texture/tiled.hpp), leaving the rows alone is what makes v = 1 the
    // first row of the image.
    for (u32 y = 0; y < u32(height); ++y) {
        for (u32 x = 0; x < u32(width); ++x) {
            const usize i = (usize(y) * usize(width) + usize(x)) * 4;
            tiled[tiledOffset(x, y, u32(width))] =
                rgbaWord(rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]);
        }
    }

    GSPGPU_FlushDataCache(tiled, u32(bytes));
    C3D_TexUpload(&tex_, tiled);
    C3D_TexFlush(&tex_);
    linearFree(tiled);

    // Nearest, always: a Minecraft font and a Minecraft dirt tile magnified
    // with anything else stop looking like themselves.
    C3D_TexSetFilter(&tex_, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&tex_, repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
                   repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);

    ready_ = true;
    return true;
}

void GuiTexture::shutdown()
{
    if (ready_) {
        C3D_TexDelete(&tex_);
        ready_ = false;
    }
}

// ---------------------------------------------------------------------------
// Background
// ---------------------------------------------------------------------------

bool Background::init(const u8* rgba)
{
    return texture_.init(rgba, texture::kBackgroundEdge, texture::kBackgroundEdge,
                         /*repeat=*/true);
}

void Background::shutdown()
{
    texture_.shutdown();
}

void Background::draw(float width, float height, float depth) const
{
    if (!texture_.ready()) {
        return;
    }

    // One quad whose texture coordinates run off the end of the texture and
    // wrap, which is the original's own arithmetic: u = width / 32,
    // v = height / 32, at a vertex colour that is already in the texels.
    //
    // v counts *down* from 1 because citro2d puts the top of an image at v = 1;
    // the original counts up from 0 in a coordinate system whose origin is the
    // other corner, and the two agree.
    const float tile = float(texture::kBackgroundTilePixels);
    const float rows = height / tile;

    // The top edge starts at a whole number of tiles rather than at 1.0. Every
    // integer v is the same texel row under GPU_REPEAT, so this draws exactly
    // what starting at 1.0 would -- and it keeps the whole range positive
    // instead of running from 1.0 down to -6.5, which is a wrap the hardware
    // should handle and which there is no reason to be the first to rely on.
    Tex3DS_SubTexture sub{};
    sub.width = u16(width);
    sub.height = u16(height);
    sub.left = 0.0f;
    sub.right = width / tile;
    sub.top = float(int(rows) + (rows > float(int(rows)) ? 1 : 0));
    sub.bottom = sub.top - rows;

    const C2D_Image image{texture_.tex(), &sub};
    C2D_DrawImageAt(image, 0.0f, 0.0f, depth, nullptr, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// BitmapFont
// ---------------------------------------------------------------------------

bool BitmapFont::init(const texture::FontImage& image)
{
    if (image.empty()) {
        return false;
    }
    if (!texture_.init(image.rgba.data(), kFontEdge, kFontEdge, /*repeat=*/false)) {
        return false;
    }

    std::memcpy(widths_, image.widths, sizeof(widths_));
    return true;
}

void BitmapFont::shutdown()
{
    texture_.shutdown();
}

int BitmapFont::measure(std::string_view text) const
{
    return texture::textWidth(widths_, text);
}

void BitmapFont::draw(std::string_view text, float x, float y, int scale, u32 colour,
                      bool shadow, float depth) const
{
    if (!texture_.ready() || scale < 1) {
        return;
    }

    if (shadow) {
        // One pixel down and right at scale 1, and one *glyph* pixel at any
        // other scale -- the original's offset is in the same units its text
        // is. A quarter of every channel, which core/texture/font.hpp derives
        // from the original and which works on citro2d's ABGR word unchanged:
        // the mask leaves each byte's low two bits clear, so the shift never
        // carries between channels and alpha is untouched.
        drawGlyphs(text, x + float(scale), y + float(scale), scale,
                   texture::shadowColour(colour), true, depth);
    }
    drawGlyphs(text, x, y, scale, colour, false, depth + 0.1f);
}

void BitmapFont::drawGlyphs(std::string_view text, float x, float y, int scale, u32 colour,
                            bool shadow, float depth) const
{
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, colour, 1.0f);

    float pen = x;
    usize i = 0;
    while (i < text.size()) {
        const u32 codepoint = texture::nextCodepoint(text, &i);

        if (codepoint == 0x00A7 && i < text.size()) {
            // A colour code and its digit, both consumed. The shadow pass takes
            // the same quarter of the code's colour that it takes of the
            // caller's, which is what the original's second set of sixteen
            // display lists holds.
            const u32 rgb = texture::fontColour(
                texture::colourCodeIndex(texture::nextCodepoint(text, &i)));
            u32 next = C2D_Color32(u8(rgb >> 16), u8(rgb >> 8), u8(rgb), 0xFF);
            if (shadow) {
                next = texture::shadowColour(next);
            }
            C2D_PlainImageTint(&tint, next, 1.0f);
            continue;
        }

        const int glyph = texture::fontGlyph(codepoint);
        if (glyph < 0) {
            // Not in the original's character string, so it draws nothing and
            // advances nothing. A world name full of kanji comes out empty
            // rather than as a row of boxes, which is a1.1.2's own answer.
            continue;
        }

        // The subtexture is a local because citro2d reads it into its vertex
        // buffer inside the call rather than holding the pointer.
        const Tex3DS_SubTexture cell = glyphCell(glyph);
        const C2D_Image image{texture_.tex(), &cell};
        C2D_DrawImageAt(image, pen, y, depth, &tint, float(scale), float(scale));
        pen += float(widths_[glyph] * scale);
    }
}

std::string_view BitmapFont::clip(std::string_view text, int maxWidth, char* buffer,
                                  usize bufferSize) const
{
    if (measure(text) <= maxWidth) {
        return text;
    }

    // Room for the ellipsis, measured rather than assumed: a pack's full stop
    // is whatever width the pack drew it.
    const int room = maxWidth - int(widths_['.']) * 3;
    usize keep = room > 0 ? texture::fitBytes(widths_, text, room) : 0;

    if (keep + 4 > bufferSize) {
        keep = bufferSize > 4 ? bufferSize - 4 : 0;
        // Back off any continuation bytes the clamp cut through, so what is
        // copied is still whole characters.
        while (keep > 0 && (u8(text[keep]) & 0xC0) == 0x80) {
            --keep;
        }
    }

    std::memcpy(buffer, text.data(), keep);
    std::memcpy(buffer + keep, "...", 3);
    buffer[keep + 3] = '\0';
    return std::string_view(buffer, keep + 3);
}

}  // namespace mc::ctr
