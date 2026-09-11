#pragma once

// **A sign as geometry** -- `jk` (ModelSign) through `in`
// (TileEntitySignRenderer), and the only thing in this project drawn from *two*
// textures in one logical object.
//
// **Two boxes and some text.** The model is a 24 x 12 x 2 board and a 2 x 14 x 2
// post, both off `item/sign.png`; the text is four lines of the pack's own
// bitmap font, drawn at a sixtieth of a block per font pixel and centred. They
// need different textures, so they are built separately and drawn in two
// passes -- `buildSignBoards` and `buildSignText`.
//
// **The post is hidden on a wall sign.** `TileEntitySignRenderer` sets
// `model.post.showModel` from which block it is, which is the one thing the two
// share a model for.
//
// The placement, transcribed:
//
//   * a **post** stands at `(x + 0.5, y + 0.75 * 2/3, z + 0.5)` and turns by
//     `-(metadata * 360 / 16)` degrees -- a sixteenth of a turn per step, which
//     is why a sign faces the way you were standing rather than a compass
//     point;
//   * a **wall sign** goes to the same place, turns by 0, 180, 90 or -90 for
//     metadata 2, 3, 4 and 5, and then steps back by `(0, -0.3125, -0.4375)` --
//     which is what puts the board against the wall instead of in the middle of
//     the block.
//
// **`glScalef(f, -f, -f)`** with `f = 2/3`, which is not the usual `(-1, -1, 1)`
// entity flip: a sign is mirrored on z rather than on x, and it is two thirds
// the size of the model it is built from.
//
// **The text is drawn with depth writes off** in the original. Here it is drawn
// after the boards in the same pass order and offset a little towards the
// viewer instead, because the detail pipeline writes depth for everything and
// splitting it would cost a state change per sign.

#include "core/mesh/vertex.hpp"
#include "core/render/draw_budget.hpp"
#include "core/texture/font.hpp"
#include "core/util/types.hpp"
#include "core/world/sign_store.hpp"

namespace mc::render {

// `0.6666667F`, the scale `TileEntitySignRenderer` wraps the whole sign in.
inline constexpr float kSignScale = 0.6666667f;

// `0.016666668F * scale` -- how big one font pixel is, in model units, before
// the sign's own scale. A sixtieth of a block once everything is multiplied
// out.
inline constexpr float kSignTextScale = 0.016666668f;

// Two boxes of six quads.
inline constexpr int kSignParts = 2;
inline constexpr int kSignVerticesEach = kSignParts * 6 * 4;

// A sign holds four lines of fifteen characters, and every glyph is one quad.
inline constexpr int kSignGlyphsEach = world::kSignLines * world::kSignLineLength;

// **What one frame draws**, which is not what exists: the store has no cap,
// and past this many in range the nearest are drawn -- see draw_budget.hpp.
inline constexpr int kSignDrawBudget = 64;
inline constexpr int kSignMaxVertices =
    kSignDrawBudget * (kSignVerticesEach + kSignGlyphsEach * 4);

// Fills `out` with the board and post quads for every sign in the store -- the
// nearest, when `max` could not also hold every one's text behind them.
//
// The boards and the text share one buffer and one `DrawCutoff`: whichever
// pass runs first, handed the whole buffer as `max`, settles it, and the other
// draws the same signs into what is left.
int buildSignBoards(const world::SignStore& store, double originX, double originY,
                    double originZ, mesh::DetailVertex* out, int max,
                    DrawCutoff* shared = nullptr);

// Fills `out` with the text quads. `font` supplies the glyph widths; a store
// with no font behind it draws no text, which is what a pack with no
// `default.png` gets.
int buildSignText(const world::SignStore& store, const texture::FontImage& font,
                  double originX, double originY, double originZ, mesh::DetailVertex* out,
                  int max, DrawCutoff* shared = nullptr);

}  // namespace mc::render
