// The generated fire tile.
//
// **There is no oracle and there cannot be one**: `TextureFlamesFX` seeds its
// bottom row from `Math.random()`, so two runs of the *original* disagree with
// each other. What is checkable is everything the noise is not -- that the heat
// rises, that the colour ramp is the class file's, that the alpha cutoff falls
// where it does, and that the result is reproducible from a stated seed, which
// is the property the original does not have and this build wants.
//
// The reason any of it matters: tile 31 of a real a1.1.2 terrain.png is red
// pixel-lettering on transparency, and a build that shows it is showing a
// placeholder the real client never displays.

#include "core/block/registry.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/texture_fx.hpp"
#include "framework.hpp"

#include <vector>

using namespace mc;
using mc::texture::FlameTexture;
using mc::texture::kFlameSettleTicks;
using mc::texture::kFlameVisibleRows;
using mc::texture::kFlameWidth;

namespace {

std::vector<u8> settledFlame(i64 seed, int ticks)
{
    FlameTexture flame(seed);
    for (int i = 0; i < ticks; ++i) {
        flame.tick();
    }
    std::vector<u8> texels(usize(kFlameWidth) * kFlameVisibleRows * 4, 0);
    flame.writeTo(texels.data());
    return texels;
}

// Mean alpha of one row, 0..255. The flame is opaque where it is hot, so this
// is "how much fire is in this row".
int rowHeat(const std::vector<u8>& texels, int row)
{
    int total = 0;
    for (int x = 0; x < kFlameWidth; ++x) {
        total += texels[(usize(row) * kFlameWidth + usize(x)) * 4 + 3];
    }
    return total / kFlameWidth;
}

}  // namespace

TEST(a_fresh_flame_is_cold_and_a_settled_one_is_not)
{
    // One tick in, the seed row is the only thing with any heat in it and it is
    // below the visible sixteen -- so the tile is still empty.
    const std::vector<u8> young = settledFlame(1, 1);
    int youngHeat = 0;
    for (int row = 0; row < kFlameVisibleRows; ++row) {
        youngHeat += rowHeat(young, row);
    }
    CHECK_EQ(youngHeat, 0);

    const std::vector<u8> settled = settledFlame(1, kFlameSettleTicks);
    int settledHeat = 0;
    for (int row = 0; row < kFlameVisibleRows; ++row) {
        settledHeat += rowHeat(settled, row);
    }
    CHECK(settledHeat > 0);
}

TEST(the_flame_is_hottest_at_the_bottom)
{
    const std::vector<u8> texels = settledFlame(7, kFlameSettleTicks);
    // Row 15 is the bottom of the tile and the one next to the seed row; row 0
    // is the top. Every cell is mostly a copy of the one below it, so heat can
    // only decrease going up -- which is the whole reason this looks like fire
    // rather than like noise.
    const int bottom = rowHeat(texels, kFlameVisibleRows - 1);
    const int top = rowHeat(texels, 0);
    CHECK(bottom > top);
    CHECK(bottom > 0);
}

TEST(the_colour_ramp_is_red_then_yellow_then_white)
{
    const std::vector<u8> texels = settledFlame(11, kFlameSettleTicks);
    bool sawOpaque = false;
    for (int i = 0; i < kFlameWidth * kFlameVisibleRows; ++i) {
        const u8 r = texels[usize(i) * 4 + 0];
        const u8 g = texels[usize(i) * 4 + 1];
        const u8 b = texels[usize(i) * 4 + 2];
        const u8 a = texels[usize(i) * 4 + 3];

        // Red is `heat * 155 + 100`, so it never falls below 100 and never
        // reaches 256 -- a fire texel is never dark and never blows out.
        CHECK(r >= 100);
        // Green is heat squared and blue is heat to the tenth, so they can only
        // ever be ordered this way. This is the ramp, as an invariant.
        CHECK(g <= r);
        CHECK(b <= g);
        // Alpha is a cutout, never a blend.
        CHECK(a == 0 || a == 255);
        sawOpaque = sawOpaque || a == 255;
    }
    CHECK(sawOpaque);
}

TEST(alpha_follows_the_half_heat_cutoff)
{
    const std::vector<u8> texels = settledFlame(3, kFlameSettleTicks);
    for (int i = 0; i < kFlameWidth * kFlameVisibleRows; ++i) {
        const u8 r = texels[usize(i) * 4 + 0];
        const u8 a = texels[usize(i) * 4 + 3];
        // Heat is recoverable from red: r = heat * 155 + 100, and the cutoff is
        // at heat 0.5, so a texel is opaque exactly when red is at least 177.
        // Truncation to int can put the boundary texel either side by one, so
        // the two ends are checked and the seam is not.
        if (r >= 179) {
            CHECK_EQ(int(a), 255);
        }
        if (r <= 176) {
            CHECK_EQ(int(a), 0);
        }
    }
}

TEST(the_same_seed_gives_the_same_flame)
{
    // The property the original does not have: `Math.random()` is seeded from
    // the clock, so a real client's fire differs run to run. Ours is stated, so
    // a pack loaded twice looks the same both times -- and so this test can
    // exist at all.
    CHECK(settledFlame(42, 60) == settledFlame(42, 60));
    CHECK(!(settledFlame(42, 60) == settledFlame(43, 60)));
}

TEST(applying_the_generated_tiles_replaces_the_fire_tile)
{
    // The fire block's own texture column, asked of the render type rather than
    // named -- the same way applyAnimatedTiles finds it.
    int fireTile = -1;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (def.known && def.render == block::RenderType::Fire) {
            fireTile = int(def.texture);
        }
    }
    CHECK(fireTile >= 0);

    // An atlas of a single flat colour, so anything that changed was written by
    // the generator rather than already there.
    std::vector<u8> atlas(texture::kAtlasBytes, 0x40);
    texture::applyAnimatedTiles(&atlas);

    const int tiles = texture::kAtlasTilesPerEdge;
    const int px = texture::kAtlasTilePixels;
    for (int tile = 0; tile < tiles * tiles; ++tile) {
        const int x = (tile % tiles) * px;
        const int y = (tile / tiles) * px;
        bool changed = false;
        for (int row = 0; row < px && !changed; ++row) {
            for (int column = 0; column < px; ++column) {
                const usize i = (usize(y + row) * texture::kAtlasEdge + usize(x + column)) * 4;
                if (atlas[i] != 0x40 || atlas[i + 3] != 0x40) {
                    changed = true;
                    break;
                }
            }
        }
        // Exactly the two the client registers a TextureFlamesFX for, one row
        // of the atlas apart, and nothing else in the sheet touched.
        const bool expected = tile == fireTile || tile == fireTile + tiles;
        CHECK_EQ(changed, expected);
    }
}

TEST(applying_the_generated_tiles_refuses_a_wrong_sized_buffer)
{
    // A caller that skipped the decode, which must not be a write past the end.
    std::vector<u8> tooSmall(16, 0);
    texture::applyAnimatedTiles(&tooSmall);
    CHECK_EQ(int(tooSmall.size()), 16);
    texture::applyAnimatedTiles(nullptr);
}

// ---------------------------------------------------------------------------
// The flame still running
// ---------------------------------------------------------------------------
//
// `applyAnimatedTiles` bakes one settled frame and stops; `FlameAnimation`
// keeps the same two simulations going so the fire in the world moves. What is
// checkable without a GPU is that it starts settled rather than black, that a
// tick changes it, that the two flames are not the same flame, and that it
// names the tiles the baked path patches -- because if those disagree the
// animation would be uploaded over something else.

TEST(the_animation_names_the_same_two_tiles_the_baked_path_patches)
{
    const texture::FlameAnimation flames;
    CHECK(flames.present());

    int fireTile = -1;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (mcver::kBlocks[id].known
            && mcver::kBlocks[id].render == block::RenderType::Fire) {
            fireTile = int(mcver::kBlocks[id].texture);
            break;
        }
    }
    CHECK(fireTile >= 0);
    CHECK_EQ(flames.tile(0), fireTile);
    CHECK_EQ(flames.tile(1), fireTile + texture::kAtlasTilesPerEdge);

    // Out of range asks for nothing rather than reading past the end.
    CHECK_EQ(flames.tile(-1), -1);
    CHECK_EQ(flames.tile(texture::kFlameCount), -1);
}

TEST(the_animation_starts_settled_rather_than_black)
{
    // The constructor runs the same kFlameSettleTicks the baked path does, so
    // the first frame after a pack loads is a flame and not a tile filling in
    // from the bottom. "Settled" here is "the bottom of the tile is opaque",
    // which a black field is not.
    const texture::FlameAnimation flames;
    int opaque = 0;
    const u8* texels = flames.texels(0);
    for (int i = 0; i < texture::kFlameWidth * texture::kFlameVisibleRows; ++i) {
        opaque += texels[i * 4 + 3] != 0 ? 1 : 0;
    }
    CHECK(opaque > 0);
}

TEST(a_tick_moves_the_flame_and_zero_ticks_does_not)
{
    texture::FlameAnimation flames;
    const usize bytes = usize(texture::kFlameWidth) * texture::kFlameVisibleRows * 4;

    std::vector<u8> before(flames.texels(0), flames.texels(0) + bytes);

    flames.tick(0);
    CHECK(std::vector<u8>(flames.texels(0), flames.texels(0) + bytes) == before);

    flames.tick(1);
    CHECK(!(std::vector<u8>(flames.texels(0), flames.texels(0) + bytes) == before));
}

TEST(the_two_flames_are_two_simulations_and_not_one_doubled)
{
    // `new TextureFlamesFX(0)` and `new TextureFlamesFX(1)` are separate
    // objects with separate noise in the original, which is what makes fire
    // two alternating layers rather than one drawn twice. Same here, by
    // seeding them apart.
    texture::FlameAnimation flames;
    flames.tick(5);
    const usize bytes = usize(texture::kFlameWidth) * texture::kFlameVisibleRows * 4;
    CHECK(!(std::vector<u8>(flames.texels(0), flames.texels(0) + bytes)
            == std::vector<u8>(flames.texels(1), flames.texels(1) + bytes)));
}

TEST(the_animation_is_reproducible_from_its_stated_seed)
{
    // Two of them, ticked the same number of times, agree -- which the
    // *original* does not, because its noise comes off a clock-seeded
    // Math.random(). It is the property that makes this testable at all.
    texture::FlameAnimation a;
    texture::FlameAnimation b;
    a.tick(7);
    b.tick(3);
    b.tick(4);
    const usize bytes = usize(texture::kFlameWidth) * texture::kFlameVisibleRows * 4;
    CHECK(std::vector<u8>(a.texels(0), a.texels(0) + bytes)
          == std::vector<u8>(b.texels(0), b.texels(0) + bytes));
}

