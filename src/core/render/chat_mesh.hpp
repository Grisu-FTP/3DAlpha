#pragma once

// **The chat lines as glyph quads on the top screen** -- the drawing half of
// core/gui/chat_log.hpp, which is `lu.a(FZII)V`'s chat loop and
// `kd.a(String, int, int, int)`, drawStringWithShadow.
//
// The vertex is `mesh::DetailVertex` for the reason sign text uses it: the
// detail pipeline is already built, the shared index buffer already expects
// four corners a quad, and the pack's font is already a texture. What is new
// is only the space: **positions are screen pixels, not blocks**, scaled by
// `kChatUnitsPerPixel` into the vertex's sixteen bits, and the renderer draws
// them through an orthographic matrix that undoes the scale.
//
// **Each line is its own span**, because each line fades on its own: the
// detail shader spends the vertex alpha on fog, so the fade cannot ride the
// vertex and is handed to the renderer as a number to put in the combiner.
// The strip behind a line is not built here either -- it has no texture, and
// the renderer draws it with the untextured outline program from the span's
// `y`.
//
// Within a span the shadow comes first and the text second, which is
// drawStringWithShadow's own order: `renderString(s, x + 1, y + 1, c, true)`
// then `renderString(s, x, y, c, false)`, the shadow in `(c & 0xFCFCFC) >> 2`.

#include "core/gui/chat_log.hpp"
#include "core/mesh/vertex.hpp"
#include "core/texture/font.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Sixteen vertex units to a screen pixel: 400 pixels is 6,400, well inside an
// i16, and a half-pixel is still exact.
inline constexpr int kChatUnitsPerPixel = 16;

// The most glyph vertices the builder writes. Two quads a character (shadow
// and text), four vertices each: 4,096 is 512 characters on screen at once,
// which is ten full lines of ordinary text. Past it the oldest lines are the
// ones left out, since the newest are built first.
inline constexpr int kChatMaxVertices = 4096;

// One line, as the renderer needs it.
struct ChatSpan {
    int y = 0;             // top of the text, in screen pixels
    int alpha = 0;         // 0..255 for the text; the strip is half this
    int firstVertex = 0;
    int vertices = 0;      // a multiple of four, and may be zero
};

// Every line with an opacity above zero, newest first, into `out`. Returns
// how many spans were written. `screenHeight` is the height the lines sit
// above -- 240 on the top screen.
int buildChatText(const gui::ChatLog& log, const texture::FontImage& font, int screenHeight,
                  mesh::DetailVertex* out, int maxVertices, ChatSpan* spans, int maxSpans);

}  // namespace mc::render
