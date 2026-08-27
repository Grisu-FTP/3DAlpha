#pragma once

// The pack's own font: `default.png`, the per-glyph widths derived from it, and
// the mapping from text to glyphs.
//
// **Every rule here was read out of a1.1.2's own font renderer** -- `kd.class`,
// reached from `Minecraft.<init>`'s `new kd(gameSettings, "/default.png",
// renderEngine)` -- rather than from a wiki or from memory:
//
//   * The sheet is a 16x16 grid of 8-pixel cells, 128x128 in the original.
//   * A glyph's advance is found by scanning its cell from the right for the
//     last column that has anything in it, and is that column plus two. The
//     space is forced to column 2, so it advances 4.
//   * **"Anything in it" means the blue channel, not the alpha channel.** The
//     original reads `BufferedImage.getRGB` and tests `pixel & 255`, which on
//     an ARGB int is blue. For a white-on-transparent font the two agree; for a
//     pack whose glyphs are, say, pure red, the original measures every column
//     as empty and draws the text one pixel wide per character. That is a1.1.2
//     behaviour and it is reproduced rather than corrected -- see
//     docs/status.md on being faithful to the game rather than to its bugs.
//   * Text is not indexed by character code. The original looks each character
//     up in a fixed string that starts at the space and ends at `»`, and adds
//     32 to the position -- so ASCII maps to itself and the upper half is
//     CP437-ish, in the order that string gives. A character that is not in it
//     draws nothing and advances nothing.
//   * `§` plus a hex digit switches colour, and both characters are consumed by
//     drawing and by measuring alike. An unrecognised digit means white, which
//     is the original's clamp rather than an invention.
//
// The one deviation is HD packs. a1.1.2 hardcodes 8 and 128.0f and would read a
// 256x256 `default.png` as a garbled 16x16 grid of the top-left quarter; here
// the sheet is scaled to 128x128 first, exactly as terrain.png is scaled to the
// atlas, and every rule above then applies to the scaled copy. See
// core/texture/atlas_image.hpp for why scaling rather than a variable size.

#include "core/io/file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/png.hpp"
#include "core/util/types.hpp"

#include <string_view>
#include <vector>

namespace mc::texture {

// The grid, and the cell size in the GUI pixels the menu draws in.
inline constexpr int kFontGlyphsPerEdge = 16;
inline constexpr int kFontCellPixels = 8;
inline constexpr int kFontEdge = kFontGlyphsPerEdge * kFontCellPixels;
inline constexpr usize kFontBytes = usize(kFontEdge) * kFontEdge * 4;

// The same ceiling terrain.png gets, and for the same reason: -fno-exceptions
// makes a failed allocation an abort, so anything that might not fit has to be
// refused before it is asked for.
inline constexpr usize kMaxFontPixels = 1024u * 1024u;

// The highest glyph the original's character string reaches. 144 characters
// starting at 32, so 32..175; the 80 cells above that are in the sheet and are
// unreachable, exactly as they are in the original.
inline constexpr int kFontFirstGlyph = 32;
inline constexpr int kFontGlyphCount = 144;

struct FontImage {
    // kFontEdge^2 * 4 bytes, R,G,B,A in memory order, top row first -- the same
    // layout AtlasImage uses, so the same upload path serves both.
    std::vector<u8> rgba;
    // Advance in GUI pixels, indexed by glyph rather than by character.
    u8 widths[256] = {};
    // The edge of the default.png this came from, before scaling. Reported the
    // way AtlasImage::sourceEdge is.
    int sourceEdge = 0;

    bool empty() const { return rgba.size() != kFontBytes; }
};

// a1.1.2's width scan over a 128x128 sheet. Exposed because it is the one rule
// here with a measurement behind it that a test can pin.
void measureGlyphWidths(const u8* sheet, u8* widths);

// The glyph a Unicode code point draws as, or -1 for one the original cannot
// draw. First match wins, which is why U+0027 is glyph 39 and cell 96 -- where
// the original's string repeats the apostrophe instead of a backtick -- can
// never be reached.
int fontGlyph(u32 codepoint);

// Decodes one UTF-8 code point and advances *pos. Malformed input yields
// U+FFFD and consumes one byte, so a name off a card can never spin this.
u32 nextCodepoint(std::string_view text, usize* pos);

// 0..15 for a colour code's hex digit, upper or lower case. **15 for anything
// else**, which is the original's clamp: `"0123456789abcdef".indexOf(c)` is
// tested for `< 0 || > 15` and forced to 15.
int colourCodeIndex(u32 codepoint);

// The original's 16 colours, built the way its constructor builds them rather
// than copied as a table of literals: one bit each for red, green and blue at
// 170, plus 85 from the intensity bit, and 85 more of red for gold. Returns
// 0x00RRGGBB.
u32 fontColour(int index);

// The shadow of a colour: every channel divided by four, which is what the
// original's `(colour & 0xFCFCFC) >> 2` does and also what it does to its own
// palette for the shadow half. Alpha is carried through untouched.
u32 shadowColour(u32 argb);

// The width of a string in GUI pixels, colour codes consumed and unknown
// characters skipped, exactly as the original measures it.
//
// Takes the width table rather than the FontImage, because the console side
// keeps the 256 widths and gives the pixels to the GPU -- see
// platform/ctr/gui_art.hpp.
int textWidth(const u8* widths, std::string_view text);

// How much of `text` fits in `maxWidth` GUI pixels, as a byte count. Never
// splits a UTF-8 sequence. Used to clip a name that came off a card.
usize fitBytes(const u8* widths, std::string_view text, int maxWidth);

// Builds the font for a pack. An empty `packPath` has none -- there is no
// generated font the way there is generated art -- and returns NotFound, which
// is the caller's cue to fall back to whatever the platform can draw with.
PackError buildFont(io::FileSystem& fs, std::string_view packPath, FontImage* out);

}  // namespace mc::texture
