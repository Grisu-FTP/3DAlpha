#pragma once

// The tiles a1.1.2 **generates** instead of reading out of terrain.png.
//
// **Fire's texture in a real client jar is a placeholder that says so.** Tile 31
// of terrain.png is red pixel-lettering on transparency and tile 47 is a flat
// magenta square, and neither is ever shown: `Minecraft` registers a
// `TextureFX` for each of them at startup and `RenderEngine` overwrites those
// 256 texels every tick with a procedural flame. A build that loads a pack's
// terrain.png and stops there draws the placeholder, which is exactly what
// "fire has a texture that says fire text" is.
//
// So this is `TextureFlamesFX`, transcribed from the class file -- the same
// treatment the physics and the cave carver got. The whole of it: a 16 x 20
// heat field, a seed row of noise at the bottom, a six-neighbour blur that
// pulls each cell towards the one below it, and a colour ramp that turns heat
// into red, then yellow, then white, with everything under half heat
// transparent.
//
// **What is not transcribed is the randomness, and it cannot be.** The seed row
// comes from `Math.random()`, whose generator is seeded from the wall clock, so
// there is no sequence to match and no oracle to compare against -- two runs of
// the *original* disagree. Ours draws from `JavaRandom` with a stated seed
// instead, which makes the output reproducible and therefore testable, and
// leaves the algorithm identical everywhere it is not the noise.
//
// **A pack's own fire tile is overwritten, deliberately.** That is what the
// original does -- the FX is registered whatever terrain.png holds -- so a pack
// that has drawn a fire tile has drawn one the real game would not show either.
//
// **It is baked once and it also runs.** `applyAnimatedTiles` settles the
// simulation and writes the result into the atlas at load time, which is what
// every software reader of the atlas gets -- an item icon on the bottom screen,
// a map sample, a headless tool. `FlameAnimation` is the same two simulations
// kept running at 20 Hz for the one reader that can show it moving: the GPU
// atlas, whose two fire tiles are 2 KB of a 256 KB texture and can be pushed
// on their own. See core/texture/tiled.hpp's `tileRunsFlipped` for why that is
// two small copies rather than a re-upload.
//
// Nothing here talks to a GPU. The animation produces 16 x 16 of RGBA per tile
// per tick and says which tile each belongs to; getting those bytes into a
// texture is platform/ctr/textures.cpp's job and no part of this.

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::texture {

// The field is sixteen wide and twenty tall: the four rows past the tile are
// where the noise is seeded and the flame rises out of.
inline constexpr int kFlameWidth = 16;
inline constexpr int kFlameHeight = 20;
inline constexpr int kFlameVisibleRows = 16;

// How many ticks a flame is run before it is written into the atlas. The field
// starts at zero and fills from the bottom at roughly a row a tick, so anything
// past about forty is settled; two hundred is settled with room to spare and
// costs a fraction of a millisecond once per pack load.
inline constexpr int kFlameSettleTicks = 200;

class FlameTexture {
public:
    explicit FlameTexture(i64 seed);

    // One call is one of the original's ticks.
    void tick();

    // The visible 16 x 16 as RGBA, R,G,B,A in memory order and top row first --
    // the same convention core/texture/png.hpp and the atlas use. `dst` is
    // 1,024 bytes.
    void writeTo(u8* dst) const;

private:
    float current_[kFlameWidth * kFlameHeight] = {};
    float next_[kFlameWidth * kFlameHeight] = {};
    // java.util.Random, standing in for Math.random()'s shared one. Held by
    // value so a FlameTexture is self-contained and two of them do not
    // interleave their draws -- the original's *do*, because Math.random()
    // shares one generator across every caller in the process. That is a
    // difference in the noise and in nothing else.
    JavaRandom rng_;
};

// Which atlas tile the `which`-th generated flame occupies, or **-1** for a
// version with no fire block.
//
// **Derived, not written down**: the block table is asked which block renders
// as fire and its texture column is the first tile, with the second sixteen
// along -- which is `new TextureFlamesFX(0)` and `new TextureFlamesFX(1)`,
// whose constructor is `Block.fire.blockIndexInTexture + i * 16`.
int flameTile(int which);

// How many flames a1.1.2 registers, and it is the two the fire renderer
// alternates between rather than one doubled.
inline constexpr int kFlameCount = 2;

// Overwrites every generated tile in a decoded atlas. Called once, after a
// pack's terrain.png has been scaled in.
void applyAnimatedTiles(std::vector<u8>* atlasRgba);

// **The two flames, still running.**
//
// `RenderEngine.updateDynamicTextures` steps every registered `TextureFX` once
// per frame in the original and re-uploads its 256 texels; this steps them on
// the *world's* clock instead, for the same reason the particles do -- a
// console drawing 24 frames a second and one drawing 60 should see the same
// fire, only sampled less often. `Minecraft.i()` runs the tick loop at 20 Hz
// and the flame is a 20 Hz thing.
//
// **It starts settled**, not black: the constructor runs each simulation the
// same `kFlameSettleTicks` `applyAnimatedTiles` does, so the first frame after
// a pack loads shows a flame rather than a tile filling in from the bottom.
//
// Self-contained and allocation-free: 2.5 KB of field and 2 KB of output, held
// by value. A version with no fire block leaves `tile()` at -1 and `tick()`
// does nothing.
class FlameAnimation {
public:
    FlameAnimation();

    // Advances every flame by `ticks` and rewrites their texels. Zero ticks is
    // the common case at 30 fps and costs a compare.
    void tick(int ticks);

    // The atlas tile this flame belongs to, or -1.
    int tile(int which) const
    {
        return which >= 0 && which < kFlameCount ? tile_[which] : -1;
    }

    // 16 x 16 RGBA, R,G,B,A in memory order and **top row first** -- the same
    // convention core/texture/png.hpp and the atlas use. 1,024 bytes.
    const u8* texels(int which) const { return texels_[which]; }

    // Whether there is anything to draw at all.
    bool present() const { return tile_[0] >= 0; }

private:
    FlameTexture flames_[kFlameCount];
    u8 texels_[kFlameCount][kFlameWidth * kFlameVisibleRows * 4] = {};
    int tile_[kFlameCount] = {-1, -1};
};

}  // namespace mc::texture
