// The generated water and lava tiles.
//
// **There is no oracle and there cannot be one**, exactly as for the flame:
// all four class files seed their bubbles from `Math.random()`, so two runs of
// the *original* disagree with each other. What is checkable is everything the
// noise is not -- the colour ramps, the translucency, the 2 x 2 block a
// flowing tile claims, and the scroll that makes a waterfall run downwards.
//
// The reason any of it matters: a real a1.1.2 `terrain.png` holds a plausible
// water tile and a lava tile that is almost pure red, and the client never
// shows either. "Lava looks a bit too red" is that tile being drawn.

#include "core/block/registry.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/fluid_fx.hpp"
#include "core/texture/texture_fx.hpp"
#include "framework.hpp"

#include <vector>

using namespace mc;
using mc::texture::applyFluidTiles;
using mc::texture::FluidAnimation;
using mc::texture::FluidFx;
using mc::texture::fluidTiles;
using mc::texture::FluidTexture;
using mc::texture::FluidTiles;
using mc::texture::kFluidEdge;
using mc::texture::kFluidSettleTicks;
using mc::texture::kFluidTexels;

namespace {

constexpr int kTilesPerEdge = mc::texture::kAtlasTilesPerEdge;
constexpr int kTileCount = kTilesPerEdge * kTilesPerEdge;

std::vector<u8> settled(FluidFx which, i64 seed, int ticks)
{
    FluidTexture fluid(which, seed);
    for (int i = 0; i < ticks; ++i) {
        fluid.tick();
    }
    std::vector<u8> texels(usize(kFluidTexels) * 4, 0);
    fluid.writeTo(texels.data());
    return texels;
}

}  // namespace

TEST(both_fluids_claim_a_still_tile_and_a_two_by_two_flowing_block)
{
    const FluidTiles water = fluidTiles(false);
    const FluidTiles lava = fluidTiles(true);

    // Derived from the block table rather than written down -- lava is the
    // fluid that emits light -- so this says the derivation found both and not
    // which tiles a1.1.2 happens to put them on.
    CHECK(water.still >= 0);
    CHECK(water.flowing >= 0);
    CHECK(lava.still >= 0);
    CHECK(lava.flowing >= 0);
    CHECK(water.still != lava.still);
    CHECK(water.flowing != lava.flowing);

    for (const FluidTiles& tiles : {water, lava}) {
        // The flowing block is four tiles and has to fit inside one pair of
        // rows: a flowing tile in the last column would wrap and tear.
        CHECK(tiles.flowing % kTilesPerEdge <= kTilesPerEdge - 2);
        CHECK(tiles.flowing + kTilesPerEdge + 1 < kTileCount);
        // The still tile is not inside its own flowing block.
        CHECK(tiles.still != tiles.flowing);
        CHECK(tiles.still != tiles.flowing + 1);
        CHECK(tiles.still != tiles.flowing + kTilesPerEdge);
        CHECK(tiles.still != tiles.flowing + kTilesPerEdge + 1);
    }
}

TEST(lava_is_orange_where_it_is_hot_and_never_the_flat_red_of_the_static_tile)
{
    // **The complaint this exists to answer.** `TextureLavaFX` pulls green up
    // as the *square* of the heat while red is already near its ceiling, so hot
    // lava runs orange and the hottest cells go pale. A tile that stays red is
    // the placeholder from terrain.png, not this.
    const std::vector<u8> texels = settled(FluidFx::Lava, 3, kFluidSettleTicks);

    int hottestRed = 0;
    int greenAtHottest = 0;
    int maxGreen = 0;
    for (int i = 0; i < kFluidTexels; ++i) {
        const int red = texels[usize(i) * 4 + 0];
        const int green = texels[usize(i) * 4 + 1];
        const int blue = texels[usize(i) * 4 + 2];

        // Red is `heat * 100 + 155`, so it has a floor of 155 and a ceiling of
        // 255 and lava is never dark.
        CHECK(red >= 155);
        CHECK(red <= 255);
        // Blue is the fourth power over 128, so it can never reach the ceiling.
        CHECK(blue <= 128);
        // Green cannot outrun red, because heat squared cannot outrun
        // heat * 100 + 155 for heat in 0..1.
        CHECK(green <= red);
        // Opaque, unlike water.
        CHECK_EQ(texels[usize(i) * 4 + 3], u8(255));

        if (red > hottestRed) {
            hottestRed = red;
            greenAtHottest = green;
        }
        maxGreen = green > maxGreen ? green : maxGreen;
    }

    // Settled lava has cells near the top of the ramp, and there green is most
    // of the way up. A flat red tile would have green in the sixties.
    CHECK(hottestRed >= 240);
    CHECK(greenAtHottest >= 180);
    CHECK(maxGreen >= 180);
}

TEST(water_is_blue_and_translucent)
{
    for (const FluidFx which : {FluidFx::Water, FluidFx::WaterFlow}) {
        const std::vector<u8> texels = settled(which, 11, kFluidSettleTicks);
        for (int i = 0; i < kFluidTexels; ++i) {
            // Blue is pinned at 255 and the other two move a little, so heat
            // reads as foam rather than as a different colour.
            CHECK_EQ(texels[usize(i) * 4 + 2], u8(255));
            CHECK(texels[usize(i) * 4 + 0] >= 32);
            CHECK(texels[usize(i) * 4 + 0] <= 64);
            CHECK(texels[usize(i) * 4 + 1] >= 50);
            CHECK(texels[usize(i) * 4 + 1] <= 114);
            // **Translucent**, 146 at rest and 196 at the crest. A pack whose
            // own water tile was opaque would be drawn as a wall of blue.
            CHECK(texels[usize(i) * 4 + 3] >= 146);
            CHECK(texels[usize(i) * 4 + 3] <= 196);
        }
    }
}

TEST(flowing_lava_is_still_lava_scrolled_a_row_every_three_ticks)
{
    // **An exact test for the scroll**, and it exists because the two lava
    // simulations are the same simulation: `TextureLavaFlowFX` differs from
    // `TextureLavaFX` only in keeping a tick counter and reading its field
    // through it. Same seed, same field, so the flowing output has to be the
    // still output rotated by whole rows -- and by one row every three ticks,
    // not every tick, which is the water one's rate.
    FluidTexture still(FluidFx::Lava, 29);
    FluidTexture flow(FluidFx::LavaFlow, 29);

    for (int tick = 1; tick <= 40; ++tick) {
        still.tick();
        flow.tick();

        u8 a[kFluidTexels * 4];
        u8 b[kFluidTexels * 4];
        still.writeTo(a);
        flow.writeTo(b);

        const int scroll = (tick / 3) * kFluidEdge;
        for (int i = 0; i < kFluidTexels; ++i) {
            const int source = ((i - scroll) % kFluidTexels + kFluidTexels) % kFluidTexels;
            for (int c = 0; c < 4; ++c) {
                CHECK_EQ(b[i * 4 + c], a[source * 4 + c]);
            }
        }
    }
}

TEST(a_still_fluid_does_not_scroll_and_a_flowing_one_moves_every_tick)
{
    // Still lava at consecutive ticks is *not* a rotation of itself -- the only
    // thing that moves is the simulation -- and a flowing tile changes every
    // tick even where the scroll has not advanced.
    FluidTexture still(FluidFx::Lava, 5);
    for (int i = 0; i < kFluidSettleTicks; ++i) {
        still.tick();
    }
    u8 before[kFluidTexels * 4];
    still.writeTo(before);
    still.tick();
    u8 after[kFluidTexels * 4];
    still.writeTo(after);

    bool rotated = false;
    for (int shift = kFluidEdge; shift < kFluidTexels; shift += kFluidEdge) {
        bool all = true;
        for (int i = 0; i < kFluidTexels && all; ++i) {
            const int source = (i - shift + kFluidTexels) % kFluidTexels;
            all = before[source * 4] == after[i * 4];
        }
        rotated = rotated || all;
    }
    CHECK(!rotated);

    int moved = 0;
    for (int i = 0; i < kFluidTexels * 4; ++i) {
        moved += before[i] != after[i] ? 1 : 0;
    }
    CHECK(moved > 0);
}

TEST(a_fluid_is_reproducible_from_its_seed)
{
    // The property the original does not have and this build wants: the noise
    // comes from a stated seed, so two runs agree and a test can say what the
    // tile is.
    for (const FluidFx which :
         {FluidFx::Water, FluidFx::WaterFlow, FluidFx::Lava, FluidFx::LavaFlow}) {
        CHECK(settled(which, 17, 60) == settled(which, 17, 60));
        CHECK(!(settled(which, 17, 60) == settled(which, 18, 60)));
    }
}

TEST(a_fluid_settles_into_something_and_keeps_moving)
{
    // A fresh field is all zeros, which is the bottom of every ramp; a settled
    // one is not, and it is still changing a tick later. "It animates" is the
    // whole of what the runtime path is for.
    for (const FluidFx which :
         {FluidFx::Water, FluidFx::WaterFlow, FluidFx::Lava, FluidFx::LavaFlow}) {
        const std::vector<u8> young = settled(which, 2, 0);
        const std::vector<u8> old = settled(which, 2, kFluidSettleTicks);
        CHECK(!(young == old));
        CHECK(!(settled(which, 2, kFluidSettleTicks)
                == settled(which, 2, kFluidSettleTicks + 1)));
    }
}

TEST(applying_the_fluid_tiles_overwrites_five_tiles_a_fluid_and_nothing_else)
{
    // The pack-load bake. Every one of a fluid's five tiles is replaced, the
    // four of the flowing block hold the **same** picture -- which is what
    // `tileSize = 2` means -- and no tile outside the two groups is touched.
    std::vector<u8> atlas(mc::texture::kAtlasBytes, 0x5A);
    applyFluidTiles(atlas.data());

    const FluidTiles water = fluidTiles(false);
    const FluidTiles lava = fluidTiles(true);

    bool claimed[kTileCount] = {};
    for (const FluidTiles& tiles : {water, lava}) {
        for (const int tile : {tiles.still, tiles.flowing, tiles.flowing + 1,
                               tiles.flowing + kTilesPerEdge, tiles.flowing + kTilesPerEdge + 1}) {
            claimed[tile] = true;
        }
    }

    const auto tileBytes = [&atlas](int tile) {
        std::vector<u8> out;
        const int x0 = (tile % kTilesPerEdge) * kFluidEdge;
        const int y0 = (tile / kTilesPerEdge) * kFluidEdge;
        for (int y = 0; y < kFluidEdge; ++y) {
            const usize at = (usize(y0 + y) * mc::texture::kAtlasEdge + usize(x0)) * 4;
            out.insert(out.end(), atlas.begin() + long(at),
                       atlas.begin() + long(at) + kFluidEdge * 4);
        }
        return out;
    };

    const std::vector<u8> untouched(usize(kFluidEdge) * kFluidEdge * 4, 0x5A);
    for (int tile = 0; tile < kTileCount; ++tile) {
        if (claimed[tile]) {
            CHECK(!(tileBytes(tile) == untouched));
        } else {
            CHECK(tileBytes(tile) == untouched);
        }
    }

    for (const FluidTiles& tiles : {water, lava}) {
        const std::vector<u8> first = tileBytes(tiles.flowing);
        CHECK(tileBytes(tiles.flowing + 1) == first);
        CHECK(tileBytes(tiles.flowing + kTilesPerEdge) == first);
        CHECK(tileBytes(tiles.flowing + kTilesPerEdge + 1) == first);
        CHECK(!(tileBytes(tiles.still) == first));
    }
}

TEST(the_animation_pushes_the_same_tiles_the_bake_wrote)
{
    // The runtime path and the load-time path have to agree about which tiles
    // they own, or the animation moves one picture while a stale one stays in
    // the atlas next to it. Six runs: a still tile and two rows of two, twice.
    FluidAnimation animation;
    CHECK(animation.present());
    CHECK_EQ(animation.writeCount(), 6);

    bool pushed[kTileCount] = {};
    for (int i = 0; i < animation.writeCount(); ++i) {
        CHECK(animation.across(i) == 1 || animation.across(i) == 2);
        CHECK(animation.tile(i) % kTilesPerEdge + animation.across(i) <= kTilesPerEdge);
        CHECK(animation.texels(i) != nullptr);
        for (int n = 0; n < animation.across(i); ++n) {
            CHECK(!pushed[animation.tile(i) + n]);
            pushed[animation.tile(i) + n] = true;
        }
    }

    for (const FluidTiles& tiles : {fluidTiles(false), fluidTiles(true)}) {
        for (const int tile : {tiles.still, tiles.flowing, tiles.flowing + 1,
                               tiles.flowing + kTilesPerEdge, tiles.flowing + kTilesPerEdge + 1}) {
            CHECK(pushed[tile]);
        }
    }
}

TEST(the_animation_starts_where_the_bake_left_off)
{
    // `FluidAnimation` settles from the same seeds `applyFluidTiles` uses, so
    // the first frame after a pack loads is the tile the atlas already holds
    // rather than a fluid filling in from nothing.
    std::vector<u8> atlas(mc::texture::kAtlasBytes, 0);
    applyFluidTiles(atlas.data());

    FluidAnimation animation;
    for (int i = 0; i < animation.writeCount(); ++i) {
        const int tile = animation.tile(i);
        const int x0 = (tile % kTilesPerEdge) * kFluidEdge;
        const int y0 = (tile / kTilesPerEdge) * kFluidEdge;
        for (int y = 0; y < kFluidEdge; ++y) {
            const usize at = (usize(y0 + y) * mc::texture::kAtlasEdge + usize(x0)) * 4;
            for (int b = 0; b < kFluidEdge * 4; ++b) {
                CHECK_EQ(atlas[at + usize(b)], animation.texels(i)[y * kFluidEdge * 4 + b]);
            }
        }
    }
}
