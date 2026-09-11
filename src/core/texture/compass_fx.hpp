#pragma once

// **The compass, which in a1.1.2 is a texture and not an item.**
//
// This one was reported as "the compass doesn't work", and the first place to
// look is the wrong one. Item 345 is a plain `di`: no `onItemUse`, no
// `onItemRightClick`, no subclass, nothing to hold and nothing to press. Read
// the item table and you conclude the compass is unimplemented in this version.
//
// It is not. `Minecraft`'s startup registers an `aa` -- TextureCompassFX --
// against tile 54 of `gui/items.png`, and `RenderEngine.updateDynamicTextures`
// overwrites those 256 texels every frame with a needle aimed at the world's
// spawn point. The compass *is* the animation. There is no behaviour anywhere
// to port, which is why this lives in core/texture/ beside the flames rather
// than in core/item/.
//
// So this is `aa.a()` transcribed, the same treatment `z`'s other five
// subclasses' cousin got in texture_fx.hpp:
//
//   * the base tile is re-blitted every tick from the sheet's own pixels, so a
//     pack's compass face is the pack's;
//   * a target angle is taken from the spawn point, the player's position and
//     the player's yaw;
//   * that angle is chased by a damped spring, which is what makes a real
//     compass swing past north and settle rather than snap;
//   * and a nine-texel grey bar and a twenty-five-texel needle are drawn over
//     the face, the front half of the needle red.
//
// **Which tile, asked of the jar rather than written down.** `ItemDef` carries
// an `animatedIcon` flag now, set by the generator from the FX class's own `b`
// and `f` fields -- the tile it owns and the sheet it owns it on. Nothing here
// names item 345 or tile 54, for the same reason `flameTile` does not name the
// fire block.
//
// **Two deviations, both stated rather than hidden.**
//
//   * **It ticks at 20 Hz, not once a frame.** The original's spring is stepped
//     from the render loop, so a compass on a 60 fps client settles three times
//     faster than one on a 20 fps client -- the needle's damping is a
//     frame-rate bug that happens to look like a feature. This project already
//     made the same call for fire and for particles and for the same reason: a
//     console drawing 24 frames a second and one drawing 60 should agree. The
//     visible effect is a needle that takes about half a second to settle
//     instead of about a sixth.
//   * **Anaglyph is not implemented.** `z.c` gates a greyscale conversion in
//     every one of these classes; the 3DS's stereo is real parallax and not a
//     red/cyan trick, so the branch has nowhere to go. It is skipped here as it
//     is in the flames.
//
// Nothing here talks to a GPU or to a world. It is handed two spawn
// coordinates, a position and a yaw, and it produces 16 x 16 of RGBA.

#include "core/util/types.hpp"

namespace mc::texture {

// The needle's own arithmetic, out of `aa.a()`. The spring pulls a tenth of the
// remaining error per step and keeps four fifths of its velocity, and the error
// it is fed is clamped to a radian first -- which is what stops a compass
// spinning the long way round when the player turns quickly.
inline constexpr double kCompassPull = 0.1;
inline constexpr double kCompassDamping = 0.8;
inline constexpr double kCompassMaxError = 1.0;

// The tile is 16 x 16 like every other, and the needle is drawn about
// (8.5, 7.5) -- half a texel right of centre and half a texel above it, which
// is the original's own asymmetry and not a rounding of ours.
inline constexpr double kCompassCentreX = 8.5;
inline constexpr double kCompassCentreY = 7.5;

// Which items-sheet tile the compass FX owns, or **-1** for a version that has
// no such item. Derived from `ItemDef::animatedIcon`; see the header.
int compassTile();

class CompassTexture {
public:
    // `base` is the 16 x 16 RGBA of the sheet's compass tile, 1,024 bytes,
    // R,G,B,A in memory order and top row first -- the same convention
    // `AtlasImage` uses. Copied, not borrowed: the buffer it comes out of is
    // the one this later writes back into.
    void setBase(const u8* base);

    // One call is one of the original's steps. `spawnX`/`spawnZ` are the
    // world's spawn point as `level.dat` stores it -- ints, widened here
    // exactly as `cn.o` and `cn.q` are; `yawDegrees` is the player's
    // `rotationYaw`, and the `- 90.0f` the original applies is done in float
    // before the widening because that is where the class file does it.
    //
    // **Returns whether the 256 texels differ from the ones the last call
    // produced**, which is the caller's cue to push the face and nothing else.
    //
    // The original re-uploads its tile every frame regardless, and on a PC
    // that costs a `glTexSubImage2D` nobody notices. Here one of the two
    // consumers is the bottom screen, and a face that has not changed still
    // costs a repaint of the whole hotbar band -- 320 x 32 pixels written by
    // the CPU into a framebuffer the LCD is scanning out of, twenty times a
    // second, forever, for a needle that stopped moving. So the caller is told
    // when there is something to push.
    //
    // It is exact rather than a threshold on the angle: the needle is plotted
    // through a truncation to int, so the spring can keep converging for as
    // long as it likes without moving a single texel, and the pixels are the
    // only honest answer to "did this change".
    bool tick(int spawnX, int spawnZ, double playerX, double playerZ, float yawDegrees);

    // The current face, 1,024 bytes of RGBA.
    const u8* texels() const { return texels_; }

    // Whether `tick` has ever run. A compass drawn before the first tick shows
    // the pack's own face with no needle on it, which is what an unspun
    // compass in the original looks like too.
    bool ready() const { return ready_; }

private:
    u8 base_[16 * 16 * 4] = {};
    u8 texels_[16 * 16 * 4] = {};
    // The face `tick` last reported a change for, which is the one the
    // consumers are holding. The comparison is against this rather than
    // against the previous tick's `texels_`, because those are the same buffer
    // and "has anything moved since they were last told" is the question.
    u8 pushed_[16 * 16 * 4] = {};
    // Forces the next tick to report a change. Set by `setBase`: a new pack
    // means the face under the needle is new even where the needle is not, and
    // the consumers are still holding the old pack's.
    bool pushDue_ = true;
    // `aa.k` and `aa.l` -- the needle's angle and its angular velocity. Doubles
    // in the original and doubles here; the spring is stiff enough that float
    // would be indistinguishable, but this is a transcription.
    double angle_ = 0.0;
    double velocity_ = 0.0;
    bool ready_ = false;
};

}  // namespace mc::texture
