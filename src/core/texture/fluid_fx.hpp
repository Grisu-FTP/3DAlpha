#pragma once

// **Water and lava are generated too**, and for the same reason fire is.
//
// `Minecraft`'s startup registers six `z` -- TextureFX -- subclasses against
// `terrain.png`: two flames (core/texture/texture_fx.hpp) and these four.
// `TextureWaterFX` and `TextureLavaFX` own the still tile of their fluid;
// `TextureWaterFlowFX` and `TextureLavaFlowFX` own the flowing one and declare
// `tileSize = 2`, which makes `RenderEngine.updateDynamicTextures` write the
// same 256 texels into a **2 x 2 block** of tiles rather than one. Those four
// tiles are exactly the ones core/mesh/fluid.cpp already spins its sample
// square across, so the block is not decoration -- it is what a flowing face
// samples once its rotation carries it off the flowing tile's own corner.
//
// **What a pack's terrain.png holds there is never shown.** Unlike fire, whose
// placeholder says "fire" in pixel letters, a real jar's water and lava tiles
// are plausible-looking and wrong, so a build that loads them and stops draws
// something that reads as the right block in the wrong colour. That is what
// "lava looks a bit too red" is: `TextureLavaFX` ramps red to 255 and then
// pulls **green up as the square of the heat**, so hot lava is orange going on
// yellow, and the static tile has none of that.
//
// The four are transcribed from the class files, `at`, `eg`, `ml` and `ht`:
//
//   * **Water** blurs three cells wide in x, divides by 3.3, and adds a field
//     of rising bubbles at 0.8. Its ramp is a blue that stays blue -- blue is
//     pinned at 255 -- and it is **translucent**, alpha 146 rising to 196.
//   * **Flowing water** is the same with the blur turned through a right angle
//     (three cells in y, ending at the cell itself rather than centred),
//     divided by 3.2, bubbling harder, and its output **scrolls one row a
//     tick**, which is the whole of why a waterfall runs downwards.
//   * **Lava** blurs nine cells and divides by 10, with the sample square
//     displaced by a sine of the *other* axis -- which is what makes it swirl
//     rather than shimmer -- and its bubbles are rarer, slower and taller.
//     Its ramp is red 155..255, green as heat squared, blue as heat to the
//     fourth over 128, and it is opaque.
//   * **Flowing lava** is that, scrolling one row every **three** ticks.
//
// **The randomness cannot be transcribed**, exactly as for the flame: the
// bubble spawns come from `Math.random()`, seeded from the wall clock, so two
// runs of the *original* disagree. These draw from `JavaRandom` with stated
// seeds, which makes the output reproducible and therefore testable and leaves
// the algorithm identical everywhere it is not the noise.
//
// Nothing here talks to a GPU. Each simulation produces 16 x 16 of RGBA per
// tick and says which tiles it belongs to; getting those bytes into a texture
// is platform/ctr/textures.cpp's job.

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::texture {

// Every one of the four fields is 16 x 16, unlike the flame's 16 x 20: a fluid
// tile wraps rather than rising out of a hidden seed row.
inline constexpr int kFluidEdge = 16;
inline constexpr int kFluidTexels = kFluidEdge * kFluidEdge;

// How many ticks a fluid is run before it is written into the atlas. Water
// settles in a few dozen and lava wants longer because its heat arrives only
// through bubbles that spawn at 0.005 a cell; two hundred is the flame's
// number and is settled with room to spare for both.
inline constexpr int kFluidSettleTicks = 200;

// Which of the four class files a simulation is.
enum class FluidFx {
    Water,      // `ml`, TextureWaterFX
    WaterFlow,  // `ht`, TextureWaterFlowFX
    Lava,       // `at`, TextureLavaFX
    LavaFlow,   // `eg`, TextureLavaFlowFX
};

class FluidTexture {
public:
    FluidTexture(FluidFx which, i64 seed);

    // One call is one of the original's ticks.
    void tick();

    // 16 x 16 as RGBA, R,G,B,A in memory order and top row first -- the same
    // convention core/texture/png.hpp and the atlas use. `dst` is 1,024 bytes.
    void writeTo(u8* dst) const;

private:
    void tickWater();
    void tickLava();

    FluidFx which_;

    // `k` in the two flowing class files, which is the only thing either of
    // them keeps between ticks beyond the fields. Unsigned so that the wrap
    // after 2^32 ticks -- seven thousand years at 20 Hz -- is defined rather
    // than undefined; Java's is a signed wrap and the two agree everywhere a
    // console can reach.
    u32 step_ = 0;

    // The original's four `float[256]`, in its own order: `g` is what the
    // colour pass reads, `h` is where the next tick is written before the two
    // are swapped, `i` is the bubble field the blur adds in, and `j` is the
    // per-cell rise that feeds `i` and decays.
    float heat_[kFluidTexels] = {};
    float next_[kFluidTexels] = {};
    float bubble_[kFluidTexels] = {};
    float rise_[kFluidTexels] = {};

    // java.util.Random standing in for Math.random()'s shared generator, held
    // by value for the reason FlameTexture holds its own: the original's four
    // simulations *do* interleave their draws, because Math.random() shares one
    // generator across the process, and that is a difference in the noise and
    // in nothing else.
    JavaRandom rng_;
};

// **Where a fluid's generated tiles are, asked of the block table.**
//
// Renderer code references render types and never block ids -- see CONTRIBUTING
// -- so this is the same rule `flameTile` follows: whatever block renders as a
// fluid says where its still and flowing tiles are, and **lava is the fluid
// that emits light**, which is the only column that separates the two without
// naming a block. `still` is one tile; `flowing` is the top-left of a 2 x 2
// block of four, which is what `tileSize = 2` means. Both -1 for a version
// without that fluid.
struct FluidTiles {
    int still = -1;
    int flowing = -1;
};

FluidTiles fluidTiles(bool hot);

// **The four, still running**, and the counterpart of `FlameAnimation`.
//
// Stepped on the *world's* clock rather than once a frame, for the reason the
// flames and the particles already are: a console drawing 24 frames a second
// and one drawing 60 should see the same water, only sampled less often.
//
// It starts settled: the constructor runs each simulation the same
// `kFluidSettleTicks` `applyAnimatedTiles` does, so the first frame after a
// pack loads shows the fluid the baked atlas already holds rather than a tile
// filling in from nothing.
//
// **A write is a run of tiles, not a tile.** The two flowing simulations each
// own four tiles holding identical texels, and a row of that block is two
// tiles side by side -- which is one copy on the console rather than two, for
// the reason core/texture/tiled.hpp gives. So this reports (tile, across)
// pairs: six of them when both fluids exist, each 1 or 2 tiles wide.
class FluidAnimation {
public:
    // Water still, water flow top row, water flow bottom row, then the same
    // three for lava.
    static constexpr int kMaxWrites = 6;

    FluidAnimation();

    // Advances every simulation by `ticks` and rewrites their texels. Zero
    // ticks is the common case at 30 fps and costs a compare.
    void tick(int ticks);

    // How many tile runs there are to push, 0 if the version has no fluids.
    int writeCount() const { return writeCount_; }

    // The first tile of the `i`-th run, how many tiles across it is, and the
    // 16 x 16 RGBA every tile in it gets -- 1,024 bytes, top row first.
    int tile(int i) const { return writes_[i].tile; }
    int across(int i) const { return writes_[i].across; }
    const u8* texels(int i) const { return texels_[writes_[i].source]; }

    bool present() const { return writeCount_ > 0; }

private:
    // One simulation per fluid phase: still and flowing, water and lava.
    static constexpr int kSimCount = 4;

    struct Write {
        int tile;
        int across;
        int source;  // which of the four simulations fills it
    };

    // Appends the runs one fluid's tiles need, and returns whether it had any.
    void addFluid(bool hot, int stillSim, int flowSim);

    FluidTexture sims_[kSimCount];
    u8 texels_[kSimCount][kFluidTexels * 4] = {};
    Write writes_[kMaxWrites] = {};
    int writeCount_ = 0;
};

// Overwrites the four fluid tiles of each fluid in a decoded atlas. Called by
// `applyAnimatedTiles`, once, after a pack's terrain.png has been scaled in.
void applyFluidTiles(u8* atlasRgba);

}  // namespace mc::texture
