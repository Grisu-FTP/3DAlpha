#pragma once

// What the menu draws with once a pack supplies it: the dirt backdrop and the
// bitmap font.
//
// Both are the pack's own files, assembled and measured in core/texture/ --
// nothing here decides what a glyph is worth or how dark the backdrop is. This
// file is the console half and nothing else: a linear-memory RGBA8 texture, the
// tiling of one quad, and a glyph-per-quad text drawer.
//
// **Neither is required.** A pack with no `default.png` leaves the font empty
// and the menu falls back to the 3DS system font, which is what it drew with
// before any of this existed; a pack with no `dirt.png` still gets a backdrop
// out of its terrain.png, and Dev Art gets one out of the generated atlas. See
// core/texture/background.hpp.

#include "core/texture/background.hpp"
#include "core/texture/font.hpp"
#include "core/util/types.hpp"

#include <citro2d.h>
#include <citro3d.h>

#include <string_view>

namespace mc::ctr {

// An RGBA8 texture citro2d can draw, uploaded through the CPU tiling path.
//
// **Ordinary linear memory, not VRAM.** The block atlas is in VRAM because
// every fragment of every chunk samples it; these two are sampled by a menu
// that draws a few hundred quads a frame, and VRAM is the thing the renderer is
// measured against.
class GuiTexture {
public:
    // `width` and `height` must be powers of two between 8 and 1024, which is
    // what the PICA addresses. False when there is no memory for it, which the
    // caller treats as "draw the old way" rather than as fatal.
    bool init(const u8* rgba, int width, int height, bool repeat);
    void shutdown();

    bool ready() const { return ready_; }
    // citro2d takes a non-const pointer even to read from it.
    C3D_Tex* tex() const { return const_cast<C3D_Tex*>(&tex_); }

private:
    C3D_Tex tex_{};
    bool ready_ = false;
};

// a1.1.2's menu backdrop: the darkened dirt tile, repeating under one quad.
//
// One quad rather than a grid of them, which is what the original draws and is
// also 100-odd fewer citro2d objects per frame -- the menu's vertex buffer is
// sized for the busiest screen and text is what should be spending it.
class Background {
public:
    bool init(const u8* rgba);
    void shutdown();
    bool ready() const { return texture_.ready(); }

    // Fills `width` x `height` from the origin, tiled at the original's 32
    // pixels per tile.
    void draw(float width, float height, float depth) const;

private:
    GuiTexture texture_;
};

// The pack's font, drawn the way a1.1.2 draws it: one quad per glyph, the pen
// advancing by the glyph's own width, the shadow one pixel down and right in a
// quarter of the colour.
class BitmapFont {
public:
    bool init(const texture::FontImage& image);
    void shutdown();
    bool ready() const { return texture_.ready(); }

    // GUI pixels at scale 1. Multiply by the scale for what is drawn.
    int measure(std::string_view text) const;
    int lineHeight() const { return texture::kFontCellPixels; }

    // Draws with (x, y) as the top-left of the first cell. Colour is citro2d's
    // ABGR word, and `§` codes inside the text override it exactly as they do
    // in the original.
    void draw(std::string_view text, float x, float y, int scale, u32 colour, bool shadow,
              float depth) const;

    // As much of `text` as fits in `maxWidth` GUI pixels at scale 1, with an
    // ellipsis when something was dropped. Returns the text to draw, borrowed
    // from `buffer`.
    std::string_view clip(std::string_view text, int maxWidth, char* buffer,
                          usize bufferSize) const;

private:
    void drawGlyphs(std::string_view text, float x, float y, int scale, u32 colour,
                    bool shadow, float depth) const;

    GuiTexture texture_;
    // The 256 widths and nothing else. **A table of 256 subtextures was the
    // obvious thing and is not affordable**: the Menu that owns this font is a
    // local in `runShell`, the 3DSX main thread has a 32 KB stack that no
    // symbol can enlarge, and 256 Tex3DS_SubTextures are 5 KB of it -- enough
    // to trip the build's own -Werror=stack-usage=8192. A glyph's cell is four
    // divisions, computed where it is drawn.
    u8 widths_[256] = {};
};

}  // namespace mc::ctr
