#pragma once

// **The flames over the screen while the player is burning** -- `jh.d(F)V`,
// the fire half of `ItemRenderer.renderOverlays` (`jh.b(F)V`), which
// `renderHand` (`iq.b(FI)V`) calls straight after the held item whenever the
// player's `aT` -- the fire counter, `PlayerVitals::fire` -- is above zero. The
// port burned the player, dealt the damage and drew nothing.
//
// **Two sheets off the two fire tiles, in camera space**, and the whole of it
// is this, run for i = 0 and 1 with the modelview at identity:
//
//     glColor4f(1, 1, 1, 0.9F);                   // blended, alpha test off
//     int tile = Block.fire.blockIndexInTexture + i * 16;
//     glTranslatef(-(i * 2 - 1) * 0.24F, -0.3F, 0);
//     glRotatef((i * 2 - 1) * 10.0F, 0, 1, 0);
//     quad x in [-0.5, 0.5], y in [-0.5, 0.5], z = -0.5
//
// Sheet 0 is the right-hand one, turned 10 degrees so its outer edge comes
// toward the eye; sheet 1 mirrors it on the left, off the *second* fire tile --
// unlike `ak`'s entity flames, which read one tile for the whole stack. Both
// are the tiles `FlameAnimation` rewrites 20 times a second, so they animate for
// free. **The u axis runs backwards** across each sheet -- the original gives
// the low-x corners the tile's right edge -- so the flames are mirrored.
//
// **One deviation, and it is the height.** a1.1.2's -0.3 puts the top of the
// sheets three quarters of the way up the middle of the screen and four fifths
// of the way up its sides, so a burning player sees mostly fire.
// `kFireOverlayDrop` pulls both sheets down by another tenth of a block, which
// puts the top edge at about 63 % of the height in the middle of the screen and
// 66 % at its sides, and the flame tips, which are the sparse top rows of the
// tile, below that. Nothing else moves: the lean, the spread, the mirroring and
// the alpha are the original's.
//
// **Full bright and untinted**, `light = 0xFF` and a white vertex colour, as
// `glColor4f(1, 1, 1, ...)` with no lighting is. The 0.9 is not in the vertex
// -- the detail shader keeps the fog amount in vertex alpha -- so the platform
// applies `kFireOverlayAlpha` in the combiner.
//
// **The geometry never changes.** Nothing here depends on the frame: the
// animation is in the texture and the transform has no inputs. The platform
// builds it once and draws the same eight vertices every frame the player
// burns.

#include "core/mesh/vertex.hpp"

namespace mc::render {

// Two sheets, four corners each.
inline constexpr int kFireOverlayVertices = 8;

// `glColor4f(1, 1, 1, 0.9F)`.
inline constexpr float kFireOverlayAlpha = 0.9f;

// a1.1.2's `glTranslatef(..., -0.3F, 0)`, plus the tenth of a block this port
// lowers it by -- see above. The one number to change if the flames want to
// sit higher or lower.
inline constexpr float kFireOverlayDrop = -0.4f;

// Both sheets, in camera space (eye at the origin, looking down -Z), in the
// detail format. Returns how many vertices were written: 8, or 4 per sheet
// whose tile is missing from this version's atlas, or 0 if `max` cannot hold
// the pair.
int buildFireOverlay(mesh::DetailVertex* out, int max);

}  // namespace mc::render
