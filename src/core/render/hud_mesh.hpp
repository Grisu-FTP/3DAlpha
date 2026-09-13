#pragma once

// **The hearts, the armour row and the air bubbles** -- the Survival half of
// `lu.a(FZII)V`, GuiIngame's overlay, as quads on the top screen.
//
// **The arithmetic is the method's; the placement is not, and deliberately.**
// a1.1.2 draws ten 9 x 9 cells eight pixels apart, the hearts starting 91 to
// the left of centre and the armour 91 to the right running the other way, both
// 32 up from the bottom, with the bubbles in the row *above* the hearts -- a
// layout whose whole shape is decided by the hotbar it sits on top of, centred
// at the bottom of the same screen.
//
// **This port's hotbar is on the other screen.** Keeping the rows where the
// original put them left them floating across the middle-bottom of the world
// with nothing under them, small enough on a 400 x 240 panel held at arm's
// length to be hard to read at a glance. So they go where a handheld's status
// readouts go and where there is nothing to collide with: hearts in the top
// left, armour in the top right, bubbles in the row *below* the hearts rather
// than above them, since above is now the screen edge. See `kHudTexelUnits` for the
// size, and docs/3ds-performance.md section 11, which already owes this overlay
// a fill measurement.
//
// **Three behaviours the method has and a plain "draw n hearts" would not**, all
// of them kept exactly:
//
//   * While the hurt window is open -- `hurtResistantTime >= 10` -- the
//     containers flash on every third tick of it, and the health the last hit
//     took away is drawn in outline over the flashing containers.
//   * At four health or below every heart jitters a pixel, off a `Random`
//     seeded with the overlay's tick counter times 312871 -- so the shake is
//     the same in both eyes and changes once a tick, not once a frame. The
//     jitter is one *source* pixel, so it scales with the rest.
//   * The bubbles are shown only with the eye under water, as whole bubbles
//     for the air left and popping ones for what is about to go.
//
// The vertex is `mesh::DetailVertex` in screen pixels, which is the chat's
// arrangement: see core/render/chat_mesh.hpp. UVs index `gui/icons.png` by
// texel through core/texture/icon_sheet.hpp's cells.

#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Sixteen vertex units to a screen pixel, as the chat uses.
inline constexpr int kHudUnitsPerPixel = 16;

// **One texel of the sheet to this many vertex units**, which is 1.875 screen
// pixels -- the row was asked to come down by about a twentieth and this is
// 6.25 % under the 2.0 it was drawn at.
//
// **It is deliberately no longer an integer number of pixels**, and that is a
// trade rather than a free win. The icons are pixel art sampled without
// filtering, so a non-integer scale means a heart's nine texel rows are not all
// the same height on screen: at 1.875 most land on two pixels and a few on one.
// That is the artefact the old comment here rejected at 1.5 -- and it rejected
// it for the right reason, because at 1.5 a third of every row is half the
// height of the rest. At 1.875 the unevenness is one pixel in eight and the row
// reads as a slightly smaller version of itself, which is what was wanted.
//
// It is expressed in *units* rather than pixels so the arithmetic stays exact:
// 30 sixteenths is a whole number in the space the vertices are written in, and
// nothing in the builder ever rounds.
inline constexpr int kHudTexelUnits = 30;

// Screen pixels are still the unit the two margins below are written in, so the
// conversion has a name rather than a bare multiply.
constexpr int hudPixels(int pixels) { return pixels * kHudUnitsPerPixel; }

// How far the two rows sit from the top and side edges of the screen, in screen
// pixels. Not a texel count: it is a margin against the panel, not part of the
// art.
inline constexpr int kHudMargin = 4;

// Thirty quads of hearts at most (container, outline, heart), ten of armour
// and ten of bubbles, four vertices each.
inline constexpr int kHudMaxVertices = 200;

struct HudInput {
    int screenWidth = 400;
    int screenHeight = 240;
    int health = 20;          // `E`
    int prevHealth = 20;      // `F`
    int hurtResistant = 0;    // `aW`
    int armour = 0;           // `eu.f()`
    int air = 300;            // `aX`
    bool eyeInWater = false;  // `isInsideOfMaterial(water)`
    u32 updateCounter = 0;    // `lu.h`, one a tick
};

// Writes the quads into `out` and returns how many vertices were written -- a
// multiple of four, never more than `maxVertices`.
int buildHud(const HudInput& input, mesh::DetailVertex* out, int maxVertices);

}  // namespace mc::render
