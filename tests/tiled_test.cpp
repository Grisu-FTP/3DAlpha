#include "framework.hpp"

#include "core/texture/atlas_image.hpp"
#include "core/texture/tiled.hpp"

#include <vector>

using namespace mc;
using texture::kAtlasEdge;
using texture::mortonInterleave;
using texture::tiledOffset;
using texture::kTileRunWords;
using texture::kTileWords;
using texture::TileRuns;
using texture::tiledOffsetFlipped;
using texture::tileRunIndex;
using texture::tileRunsFlipped;

namespace {

// The Morton order of one 8x8 tile, written out rather than computed, so the
// test states the layout instead of restating the function under test.
//
// x supplies the even bits and y the odd ones: (1,0) is one word along, (0,1)
// is two, (2,0) is four. This is the whole reason a texture cannot be memcpy'd
// into a C3D_Tex.
constexpr u32 kTileOrder[8][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
    {32, 33, 36, 37, 48, 49, 52, 53},
    {34, 35, 38, 39, 50, 51, 54, 55},
    {40, 41, 44, 45, 56, 57, 60, 61},
    {42, 43, 46, 47, 58, 59, 62, 63},
};

}  // namespace

TEST(morton_matches_the_written_out_tile)
{
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            CHECK_EQ(mortonInterleave(x, y), kTileOrder[y][x]);
        }
    }
}

TEST(morton_ignores_everything_above_the_tile)
{
    // The tile index is added by tiledOffset, not by the interleave, so the
    // interleave has to stay inside 0..63 for any coordinate at all.
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            CHECK_EQ(mortonInterleave(x, y), mortonInterleave(x & 7, y & 7));
        }
    }
}

TEST(tiles_run_across_then_down)
{
    // 64 words to a tile across, and width*8 to a row of tiles.
    CHECK_EQ(tiledOffset(8, 0, 256), 64u);
    CHECK_EQ(tiledOffset(16, 0, 256), 128u);
    CHECK_EQ(tiledOffset(0, 8, 256), 256u * 8);
    CHECK_EQ(tiledOffset(9, 9, 256), 256u * 8 + 64 + kTileOrder[1][1]);
}

TEST(flip_sends_row_zero_to_the_last_row_of_memory)
{
    // **This is the rule the atlas upload used to get wrong.** Source row 0 is
    // terrain.png's top row and it has to land in the highest addresses in the
    // buffer, because v = 0 samples the last row in memory. The last row is the
    // one a short or off-by-one write loses first, and losing it shows up on
    // exactly the tiles in atlas row 0 -- grass, dirt, stone, the flowers -- and
    // on nothing else.
    for (u32 x = 0; x < 256; ++x) {
        CHECK_EQ(tiledOffsetFlipped(x, 0, 256, 256), tiledOffset(x, 255, 256));
        CHECK_EQ(tiledOffsetFlipped(x, 255, 256, 256), tiledOffset(x, 0, 256));
    }
}

TEST(the_atlas_map_covers_every_word_exactly_once)
{
    // The covering check, and the one that would have caught the missing row
    // without a console: 65536 source texels, 65536 distinct offsets, none of
    // them past the end of the buffer. Any map that drops a row, doubles one or
    // walks off the end fails here.
    const usize count = usize(kAtlasEdge) * kAtlasEdge;
    std::vector<u8> seen(count, 0);

    for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
        for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
            const u32 offset = tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge);
            CHECK(offset < count);
            CHECK_EQ(seen[offset], u8(0));
            seen[offset] = 1;
        }
    }

    for (usize i = 0; i < count; ++i) {
        CHECK_EQ(seen[i], u8(1));
    }
}

TEST(the_lightmap_map_covers_every_word_exactly_once)
{
    // The same check at the lightmap's 16x16, which is the other caller and the
    // one where a wrong offset would be read as the world being lit wrongly
    // rather than as a texture being wrong.
    u8 seen[256] = {};
    for (u32 sky = 0; sky < 16; ++sky) {
        for (u32 block = 0; block < 16; ++block) {
            const u32 offset = tiledOffsetFlipped(block, sky, 16, 16);
            CHECK(offset < 256u);
            CHECK_EQ(seen[offset], u8(0));
            seen[offset] = 1;
        }
    }
    for (u32 i = 0; i < 256; ++i) {
        CHECK_EQ(seen[i], u8(1));
    }
}

TEST(tiling_an_image_round_trips)
{
    // Write a whole atlas through the map and read it back through the same
    // map: what comes out has to be what went in, texel for texel, including
    // the first row and the last.
    const usize count = usize(kAtlasEdge) * kAtlasEdge;
    std::vector<u32> tiled(count, 0xDEADBEEF);

    const auto value = [](u32 x, u32 y) { return (y << 16) | x; };

    for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
        for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
            tiled[tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge)] = value(x, y);
        }
    }

    for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
        for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
            CHECK_EQ(tiled[tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge)], value(x, y));
        }
    }
}

// ---------------------------------------------------------------------------
// One tile on its own
// ---------------------------------------------------------------------------
//
// **The claim these pin is a performance one, and it is only true because of
// the layout.** Fire animates by replacing two atlas tiles twenty times a
// second, and the atlas is 256 KB in VRAM that the CPU cannot store into -- so
// the update has to be a DMA of exactly those tiles or it is a re-upload of
// the whole texture. `tileRunsFlipped` says it is two runs of 512 bytes; if
// that is wrong the failure on hardware is not a slow frame but a corrupted
// atlas, which is why the arithmetic lives in core where it can be checked.

TEST(a_tile_is_exactly_two_runs_of_a_hundred_and_twenty_eight_words)
{
    // Every texel of the tile lands somewhere in the two runs, no texel lands
    // twice, and nothing outside the tile lands in them.
    for (u32 tile = 0; tile < 256; tile += 37) {
        const u32 column = tile % 16;
        const u32 row = tile / 16;
        const TileRuns runs = tileRunsFlipped(column, row, u32(kAtlasEdge));

        bool filled[kTileWords] = {};
        for (u32 y = 0; y < 16; ++y) {
            for (u32 x = 0; x < 16; ++x) {
                const u32 at = tileRunIndex(runs, column, row, x, y, u32(kAtlasEdge));
                CHECK(at < kTileWords);
                CHECK(!filled[at]);
                filled[at] = true;
            }
        }
        for (u32 i = 0; i < kTileWords; ++i) {
            CHECK(filled[i]);
        }
    }
}

TEST(a_tiles_runs_hold_only_that_tile)
{
    // The other direction, and the one that matters: a run that reached past
    // its tile would overwrite a neighbouring texture every time fire ticked.
    for (u32 tile : {0u, 15u, 31u, 47u, 240u, 255u}) {
        const u32 column = tile % 16;
        const u32 row = tile / 16;
        const TileRuns runs = tileRunsFlipped(column, row, u32(kAtlasEdge));

        for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
            for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
                const u32 offset = tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge);
                const bool inRuns =
                    (offset >= runs.first && offset < runs.first + kTileRunWords)
                    || (offset >= runs.second && offset < runs.second + kTileRunWords);
                const bool inTile = x / 16 == column && y / 16 == row;
                CHECK_EQ(inRuns, inTile);
            }
        }
    }
}

TEST(both_runs_are_aligned_for_the_consoles_copy_engine)
{
    // GX_TextureCopy wants 16-byte alignment at both ends and a size that is a
    // multiple of 16. A run is 512 bytes and both offsets are multiples of 128
    // words, so all three hold -- but only because a tile column advances by
    // 128 words and a tile row by a multiple of that.
    for (u32 tile = 0; tile < 256; ++tile) {
        const TileRuns runs = tileRunsFlipped(tile % 16, tile / 16, u32(kAtlasEdge));
        CHECK_EQ(runs.first % kTileRunWords, 0u);
        CHECK_EQ(runs.second % kTileRunWords, 0u);
        CHECK_EQ(runs.second - runs.first, 8u * u32(kAtlasEdge));
        CHECK(runs.second + kTileRunWords <= u32(kAtlasEdge) * u32(kAtlasEdge));
    }
}

TEST(writing_a_tile_through_its_runs_matches_writing_it_texel_by_texel)
{
    // The whole update path, in the small: build the two runs the way
    // platform/ctr/textures.cpp does, splice them into an atlas, and check the
    // result against the same tile written through the general map.
    const usize count = usize(kAtlasEdge) * kAtlasEdge;
    std::vector<u32> byTexel(count, 0u);
    std::vector<u32> byRun(count, 0u);

    const u32 tile = 31;  // fire's own, in this version
    const u32 column = tile % 16;
    const u32 row = tile / 16;
    const auto value = [](u32 x, u32 y) { return (y << 16) | x | 0x80000000u; };

    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            byTexel[tiledOffsetFlipped(column * 16 + x, row * 16 + y, kAtlasEdge, kAtlasEdge)] =
                value(x, y);
        }
    }

    const TileRuns runs = tileRunsFlipped(column, row, u32(kAtlasEdge));
    u32 staging[kTileWords] = {};
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            staging[tileRunIndex(runs, column, row, x, y, u32(kAtlasEdge))] = value(x, y);
        }
    }
    for (u32 i = 0; i < kTileRunWords; ++i) {
        byRun[runs.first + i] = staging[i];
        byRun[runs.second + i] = staging[kTileRunWords + i];
    }

    CHECK(byRun == byTexel);
}

