#pragma once

// **Text in a pack's font, drawn into a software surface**: the tooltips on the
// bottom screen under the menu's settings lists.
//
// The top screen draws the same font on the GPU through platform/ctr/gui_art's
// BitmapFont. The bottom screen is not a GPU target (see core/gui/paint.hpp),
// and libctru's console paints a solid background behind every character, so
// text over the dirt backdrop has to be written pixel by pixel instead. Same
// glyph cells, same widths, same colour codes and same quarter-bright shadow one
// pixel down and right, so the two screens agree about what the text looks like.
//
// A pack with no `default.png` still needs a font down there, and the console
// already carries one. `fontFromBitmap` turns that 8x8 one-bit table into a
// FontImage so the drawing has a single path.

#include "core/gui/paint.hpp"
#include "core/texture/font.hpp"

#include <string_view>

namespace mc::gui {

// Draws `text` with its top-left corner at (x, y), one GUI pixel to a screen
// pixel, in `rgb` (0x00RRGGBB) until a colour code changes it. With `shadow`, a
// copy a quarter as bright goes one pixel down and right first. A texel is
// drawn when its alpha is at least half, multiplied by the colour -- the bottom
// screen is RGB565 and has nothing to blend with. Returns the x the next glyph
// would start at. Draws nothing, and returns `x`, for an empty font.
int drawText(const Surface& surface, int x, int y, const texture::FontImage& font,
             std::string_view text, u32 rgb, bool shadow);

// Builds a font out of an 8x8, one-bit-per-pixel table: `count` glyphs for the
// character codes starting at `first`, eight bytes each, top row first, bit 7
// leftmost -- the layout of libctru's console font. Every glyph is moved left by
// the columns empty in all the printable ASCII ones, so a font drawn for a grid
// does not carry its left margin into proportional text, and the widths are
// then measured exactly as a pack's are.
void fontFromBitmap(const u8* table, int first, int count, texture::FontImage* out);

}  // namespace mc::gui
