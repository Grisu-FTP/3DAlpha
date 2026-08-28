#include "framework.hpp"

#include "core/map/map_palette.hpp"
#include "core/map/map_render.hpp"
#include "core/map/map_sample.hpp"
#include "core/map/map_store.hpp"

#include <vector>

using namespace mc;
using namespace mc::map;

namespace {

constexpr block::BlockId kStone = block::BlockId(mcver::Block::Stone);
constexpr block::BlockId kGrass = block::BlockId(mcver::Block::Grass);
constexpr block::BlockId kWater = block::BlockId(mcver::Block::Water);
constexpr block::BlockId kTorch = block::BlockId(mcver::Block::Torch);
constexpr block::BlockId kLava = block::BlockId(mcver::Block::Lava);

// A column filled to `surface` with stone, with the height map the original
// would have written for it: one above the highest block that stops light.
void fillFlat(world::ChunkColumn& column, int surface, block::BlockId top)
{
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y <= surface; ++y) {
                column.setBlock(x, y, z, y == surface ? top : kStone);
            }
            column.heightMap[z * 16 + x] = u8(surface + 1);
        }
    }
}

// An atlas whose every tile is one flat colour, chosen from the tile index so
// two blocks with different textures cannot accidentally compare equal.
texture::AtlasImage solidAtlas()
{
    texture::AtlasImage atlas;
    atlas.rgba.assign(texture::kAtlasBytes, 0);
    for (int tile = 0; tile < 256; ++tile) {
        const int tx = (tile % 16) * texture::kAtlasTilePixels;
        const int ty = (tile / 16) * texture::kAtlasTilePixels;
        for (int y = 0; y < texture::kAtlasTilePixels; ++y) {
            for (int x = 0; x < texture::kAtlasTilePixels; ++x) {
                u8* px = &atlas.rgba[(usize(ty + y) * texture::kAtlasEdge + usize(tx + x)) * 4];
                px[0] = u8(tile);
                px[1] = u8(255 - tile);
                px[2] = u8((tile * 7) & 0xFF);
                px[3] = 255;
            }
        }
    }
    return atlas;
}

// Draw the patches, then copy them out -- the two steps the console takes, in
// the order it takes them. Every test below goes through this rather than
// through `renderMapWindow` alone, because a copy on its own is only ever as
// right as the patches under it.
void drawWindow(MapStore& store, const MapPalette& palette, const MapWindow& window,
                const MapStyle& style, const MapSurface& surface, u32 stamp = 1)
{
    refreshMapWindow(store, palette, window, style, stamp);
    renderMapWindow(store, window, style, surface);
}

// A row-major surface, which is what every test here reads back.
struct Canvas {
    int width;
    int height;
    std::vector<MapPixel> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(usize(w * h), 0) {}

    MapSurface surface() { return MapSurface{pixels.data(), 1, width}; }
    MapPixel at(int x, int z) const { return pixels[usize(z * width + x)]; }
};

}  // namespace

TEST(the_surface_scan_skips_what_a_later_map_calls_air)
{
    world::ChunkColumn column(0, 0);
    fillFlat(column, 64, kGrass);
    // A torch on top: `Material.circuits` on a later map, so the scan walks
    // past it and paints the grass under it.
    column.setBlock(3, 65, 4, kTorch);
    column.heightMap[4 * 16 + 3] = 66;

    MapChunkSample sample;
    sampleChunk(column, &sample);

    CHECK_EQ(sample.surface[3 * 16 + 4], kGrass);
    CHECK_EQ(int(sample.height[3 * 16 + 4]), 64);
    CHECK_EQ(int(sample.depth[3 * 16 + 4]), 0);
}

TEST(water_records_its_depth_and_lava_does_not_count_as_water)
{
    world::ChunkColumn column(0, 0);
    fillFlat(column, 50, kStone);
    for (int y = 51; y <= 55; ++y) {
        column.setBlock(2, y, 2, kWater);
    }
    column.heightMap[2 * 16 + 2] = 56;

    MapChunkSample sample;
    sampleChunk(column, &sample);

    CHECK_EQ(sample.surface[2 * 16 + 2], kWater);
    CHECK_EQ(int(sample.height[2 * 16 + 2]), 55);
    CHECK_EQ(int(sample.depth[2 * 16 + 2]), 5);

    CHECK(isMapWater(kWater));
    CHECK(!isMapWater(kLava));
    CHECK(!isMapWater(kStone));
}

TEST(an_empty_column_is_sky_rather_than_a_missing_answer)
{
    world::ChunkColumn column(0, 0);
    MapChunkSample sample;
    sampleChunk(column, &sample);

    CHECK_EQ(sample.surface[0], block::kAir);
    CHECK_EQ(int(sample.height[0]), 0);
}

TEST(the_palette_takes_a_blocks_top_face_from_the_pack)
{
    const texture::AtlasImage atlas = solidAtlas();
    MapPalette palette;
    buildMapPalette(atlas, &palette);

    // Grass is the case that matters: its top tile is not its side tile, so a
    // palette that took `texture` rather than the top face would paint a lawn
    // the colour of the dirt in its edge.
    const block::BlockDef& grass = block::def(kGrass);
    CHECK(grass.faces[mesh::kFacePosY] != grass.faces[mesh::kFaceNegZ]);
    CHECK_EQ(palette.base[kGrass], averageTileColour(atlas, grass.faces[mesh::kFacePosY]));
    CHECK(palette.known[kGrass]);

    // A torch has no colour at all, because it is never what a map draws.
    CHECK(!palette.known[kTorch]);
    CHECK(!palette.known[block::kAir]);
}

TEST(a_transparent_tile_still_produces_a_colour)
{
    // Every pixel transparent except one, which is the shape a sapling or a
    // cactus has: the answer must be the one pixel that is drawn, not a
    // sixteenth of it.
    texture::AtlasImage atlas;
    atlas.rgba.assign(texture::kAtlasBytes, 0);
    u8* px = &atlas.rgba[0];
    px[0] = 200;
    px[1] = 100;
    px[2] = 50;
    px[3] = 255;

    CHECK_EQ(averageTileColour(atlas, 0), u32(0xC86432));
}

TEST(the_store_keeps_a_chunk_and_evicts_the_oldest_use)
{
    MapStore store;
    store.setCapacity(2);

    MapChunkSample a;
    a.surface[0] = kStone;
    MapChunkSample b;
    b.surface[0] = kGrass;
    MapChunkSample c;
    c.surface[0] = kWater;

    store.store(0, 0, a);
    store.store(1, 0, b);
    CHECK(store.find(0, 0) != nullptr);
    CHECK_EQ(store.find(1, 0)->surface[0], kGrass);

    // Touching (0,0) makes (1,0) the oldest use, so that is what goes.
    store.touch(0, 0);
    store.store(2, 0, c);
    CHECK(store.find(1, 0) == nullptr);
    CHECK_EQ(store.find(0, 0)->surface[0], kStone);
    CHECK_EQ(store.find(2, 0)->surface[0], kWater);
    CHECK_EQ(store.stats().evicted, 1u);
    CHECK_EQ(store.stats().chunks, 2);
}

TEST(storing_a_chunk_twice_replaces_it)
{
    MapStore store;
    store.setCapacity(4);

    MapChunkSample a;
    a.surface[0] = kStone;
    store.store(-3, 7, a);
    a.surface[0] = kGrass;
    store.store(-3, 7, a);

    CHECK_EQ(store.find(-3, 7)->surface[0], kGrass);
    CHECK_EQ(store.stats().chunks, 1);
    CHECK_EQ(store.stats().evicted, 0u);
}

TEST(the_tile_grid_is_the_one_later_versions_centre_maps_on)
{
    // `MathHelper.floor((x + 64) / 128)` and a first block of `tile * 128 - 64`:
    // the tile containing the origin starts at -64, and its edge is a chunk
    // edge, which is what "aligned with the chunks" has to mean for both grids
    // to be true at once.
    CHECK_EQ(tileOfBlock(0), 0);
    CHECK_EQ(tileOriginBlock(0), -64);
    CHECK_EQ(tileOfBlock(-64), 0);
    CHECK_EQ(tileOfBlock(-65), -1);
    CHECK_EQ(tileOriginBlock(-1), -192);
    CHECK_EQ(tileOfBlock(63), 0);
    CHECK_EQ(tileOfBlock(64), 1);
    CHECK_EQ(tileOriginBlock(1), 64);

    CHECK(isTileEdgeBlock(-64));
    CHECK(isTileEdgeBlock(64));
    CHECK(!isTileEdgeBlock(0));

    // Every tile edge is a chunk edge.
    for (i32 tile = -4; tile <= 4; ++tile) {
        CHECK_EQ(tileOriginBlock(tile) % 16, 0);
    }
}

TEST(land_shading_follows_the_step_to_the_north)
{
    const texture::AtlasImage atlas = solidAtlas();
    MapPalette palette;
    buildMapPalette(atlas, &palette);

    MapStore store;
    store.setCapacity(4);

    // One chunk, stone everywhere, stepping up one block halfway down.
    MapChunkSample sample;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            sample.surface[x * 16 + z] = kStone;
            sample.height[x * 16 + z] = u8(z < 8 ? 64 : 65);
            sample.depth[x * 16 + z] = 0;
        }
    }
    store.store(0, 0, sample);

    Canvas canvas(16, 16);
    MapWindow window{0, 0, 16, 16};
    drawWindow(store, palette, window, MapStyle{}, canvas.surface());

    // Row 8 is the step up: brighter. Row 9 is level again: the middle. Row 1
    // is level with row 0 and also the middle.
    CHECK_EQ(canvas.at(4, 8), palette.shaded[2][kStone]);
    CHECK_EQ(canvas.at(4, 9), palette.shaded[1][kStone]);
    CHECK_EQ(canvas.at(4, 1), palette.shaded[1][kStone]);
}

TEST(the_row_above_the_window_is_what_the_first_row_shades_against)
{
    const texture::AtlasImage atlas = solidAtlas();
    MapPalette palette;
    buildMapPalette(atlas, &palette);

    MapStore store;
    store.setCapacity(4);

    MapChunkSample north;
    MapChunkSample here;
    for (int i = 0; i < 256; ++i) {
        north.surface[i] = kStone;
        north.height[i] = 70;
        north.depth[i] = 0;
        here.surface[i] = kStone;
        here.height[i] = 64;
        here.depth[i] = 0;
    }
    store.store(0, -1, north);
    store.store(0, 0, here);

    Canvas canvas(16, 16);
    drawWindow(store, palette, MapWindow{0, 0, 16, 16}, MapStyle{}, canvas.surface());

    // The window's first row is six blocks below the chunk to the north, so it
    // is the dark step -- which is only knowable by looking outside the window.
    CHECK_EQ(canvas.at(0, 0), palette.shaded[0][kStone]);
    CHECK_EQ(canvas.at(0, 1), palette.shaded[1][kStone]);
}

TEST(water_shading_is_depth_on_a_checkerboard)
{
    const texture::AtlasImage atlas = solidAtlas();
    MapPalette palette;
    buildMapPalette(atlas, &palette);

    MapStore store;
    store.setCapacity(4);

    MapChunkSample sample;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            sample.surface[x * 16 + z] = kWater;
            sample.height[x * 16 + z] = 62;
            // Depth grows with x, so one row walks the whole rule.
            sample.depth[x * 16 + z] = u8(x + 1);
        }
    }
    store.store(0, 0, sample);

    Canvas canvas(16, 16);
    drawWindow(store, palette, MapWindow{0, 0, 16, 16}, MapStyle{}, canvas.surface());

    // depth*0.1 + ((x+z)&1)*0.2, bright under 0.5 and dark over 0.9. At z = 0:
    // x = 0 is depth 1 on an even square -> 0.1 -> bright.
    CHECK_EQ(canvas.at(0, 0), palette.shaded[2][kWater]);
    // x = 5 is depth 6 on an odd square -> 0.8 -> the middle.
    CHECK_EQ(canvas.at(5, 0), palette.shaded[1][kWater]);
    // x = 9 is depth 10 on an odd square -> 1.2 -> dark.
    CHECK_EQ(canvas.at(9, 0), palette.shaded[0][kWater]);
    // ...and depth alone does not decide it: x = 8, depth 9, even -> 0.9, which
    // is not over 0.9, so it stays in the middle.
    CHECK_EQ(canvas.at(8, 0), palette.shaded[1][kWater]);
}

TEST(unsampled_ground_draws_as_unexplored)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(4);

    Canvas canvas(8, 8);
    MapStyle style;
    style.unexplored = rgb565(16, 16, 24);
    drawWindow(store, palette, MapWindow{-4, -4, 8, 8}, style, canvas.surface());

    for (int z = 0; z < 8; ++z) {
        for (int x = 0; x < 8; ++x) {
            CHECK_EQ(canvas.at(x, z), style.unexplored);
        }
    }
}

TEST(a_negative_window_lands_on_the_right_chunks)
{
    MapWindow window{-17, -1, 16, 16};
    i32 minX = 0, minZ = 0, maxX = 0, maxZ = 0;
    windowChunkRange(window, &minX, &minZ, &maxX, &maxZ);
    CHECK_EQ(minX, -2);
    CHECK_EQ(maxX, -1);
    CHECK_EQ(minZ, -1);
    CHECK_EQ(maxZ, 0);
}

TEST(a_negative_window_reads_the_chunk_the_blocks_are_in)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(4);

    MapChunkSample sample;
    for (int i = 0; i < 256; ++i) {
        sample.surface[i] = kStone;
        sample.height[i] = 64;
    }
    // The chunk west and north of the origin holds blocks -16..-1.
    store.store(-1, -1, sample);

    Canvas canvas(16, 16);
    MapStyle style;
    style.unexplored = rgb565(255, 0, 255);
    drawWindow(store, palette, MapWindow{-16, -16, 16, 16}, style, canvas.surface());

    // The middle brightness at both ends: the row north of the window has never
    // been sampled, so the first row has nothing to step from and shades level.
    CHECK_EQ(canvas.at(0, 0), palette.shaded[1][kStone]);
    CHECK_EQ(canvas.at(15, 15), palette.shaded[1][kStone]);
}

TEST(the_facing_of_a_yaw_is_minecrafts_own)
{
    // Yaw 0 looks along +Z, which is south and therefore down the map, and it
    // increases towards -X, which is west and therefore left.
    CHECK_EQ(facingFromYaw(0.0f), 0);
    CHECK_EQ(kFacingStep[facingFromYaw(0.0f)].z, 1);
    CHECK_EQ(kFacingStep[facingFromYaw(90.0f)].x, -1);
    CHECK_EQ(kFacingStep[facingFromYaw(90.0f)].z, 0);
    CHECK_EQ(kFacingStep[facingFromYaw(180.0f)].z, -1);
    CHECK_EQ(kFacingStep[facingFromYaw(-90.0f)].x, 1);
    // Rounds to the nearest of the eight, and wraps.
    CHECK_EQ(facingFromYaw(44.0f), 1);
    CHECK_EQ(facingFromYaw(360.0f), 0);
    CHECK_EQ(facingFromYaw(-360.0f), 0);
}

TEST(a_marker_is_drawn_where_the_player_is_and_is_clipped_to_the_window)
{
    Canvas canvas(32, 32);
    MapWindow window{0, 0, 32, 32};
    const MapPixel fill = rgb565(255, 255, 255);
    const MapPixel outline = rgb565(0, 0, 0);

    drawMarker(canvas.surface(), window, 16.5, 20.5, 4 /* north */, 2, fill, outline);

    CHECK_EQ(canvas.at(16, 20), fill);
    // The tick reaches north of the body.
    CHECK_EQ(canvas.at(16, 20 - 4), fill);
    // ...and nothing reaches the same distance south, because that is the tail.
    CHECK(canvas.at(16, 20 + 4) != fill);
    // The body is ringed.
    CHECK_EQ(canvas.at(16 + 3, 20), outline);

    // A marker outside the window writes nothing rather than off the end.
    Canvas edge(8, 8);
    drawMarker(edge.surface(), MapWindow{0, 0, 8, 8}, -40.0, -40.0, 0, 2, fill, outline);
    for (usize i = 0; i < edge.pixels.size(); ++i) {
        CHECK_EQ(edge.pixels[i], MapPixel(0));
    }
}

TEST(the_chunk_grid_only_darkens_and_the_tile_grid_paints)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(64);

    MapChunkSample sample;
    for (int i = 0; i < 256; ++i) {
        sample.surface[i] = kStone;
        sample.height[i] = 64;
    }
    for (i32 cz = -5; cz <= 1; ++cz) {
        for (i32 cx = -5; cx <= 1; ++cx) {
            store.store(cx, cz, sample);
        }
    }

    Canvas canvas(32, 32);
    MapStyle style;
    style.chunkGrid = true;
    style.tileGrid = true;
    style.tileGridColour = rgb565(255, 0, 0);
    // Starts at -64, which is a tile edge, so both grids are in view.
    drawWindow(store, palette, MapWindow{-64, -64, 32, 32}, style, canvas.surface());

    CHECK_EQ(canvas.at(0, 0), style.tileGridColour);
    CHECK_EQ(canvas.at(16, 5), palette.shaded[0][kStone]);
    CHECK_EQ(canvas.at(17, 5), palette.shaded[1][kStone]);
    CHECK_EQ(canvas.at(5, 16), palette.shaded[0][kStone]);
}

TEST(a_negative_stride_surface_lands_exactly_where_the_bottom_screen_is)
{
    // **The console's framebuffer, in the orientation it is really stored in.**
    // The bottom screen is 320 x 240 but the memory runs down each column and
    // bottom-to-top inside it: pixel (x, y) is at `x * 240 + (239 - y)`. So the
    // map is drawn through a positive stride across and a *negative* one down,
    // which is the one piece of MapSurface that cannot be checked by looking at
    // it. This is that check, against the same numbers platform/ctr uses.
    constexpr int kScreenWidth = 320;
    constexpr int kScreenHeight = 240;
    constexpr int kMapPixels = 192;
    constexpr int kMapLeft = kScreenWidth - kMapPixels;
    constexpr int kMapTop = 24;

    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(512);
    MapChunkSample sample;
    for (int i = 0; i < 256; ++i) {
        sample.surface[i] = kStone;
        sample.height[i] = u8(64 + (i % 3));
    }
    for (i32 cz = -8; cz <= 8; ++cz) {
        for (i32 cx = -8; cx <= 8; ++cx) {
            store.store(cx, cz, sample);
        }
    }

    const MapWindow window{-96, -96, kMapPixels, kMapPixels};
    MapStyle style;
    style.unexplored = rgb565(20, 22, 34);

    // Row-major, which every other test in this file reads.
    Canvas expected(kMapPixels, kMapPixels);
    drawWindow(store, palette, window, style, expected.surface());

    // ...and through the framebuffer's own layout.
    const MapPixel kUntouched = 0xABCD;
    std::vector<MapPixel> screen(usize(kScreenWidth * kScreenHeight), kUntouched);
    MapSurface surface;
    surface.pixels = screen.data() + kMapLeft * kScreenHeight + (kScreenHeight - 1 - kMapTop);
    surface.strideX = kScreenHeight;
    surface.strideZ = -1;
    drawWindow(store, palette, window, style, surface);

    for (int y = 0; y < kScreenHeight; ++y) {
        for (int x = 0; x < kScreenWidth; ++x) {
            const MapPixel got = screen[usize(x * kScreenHeight + (kScreenHeight - 1 - y))];
            const bool insideMap = x >= kMapLeft && x < kMapLeft + kMapPixels && y >= kMapTop
                                   && y < kMapTop + kMapPixels;
            if (!insideMap) {
                // Not one pixel outside the rectangle: the console's text is
                // there, and a stride that ran off the end would eat it.
                CHECK_EQ(got, kUntouched);
                continue;
            }
            CHECK_EQ(got, expected.at(x - kMapLeft, y - kMapTop));
        }
    }
}

TEST(a_patch_is_drawn_once_and_reused)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(64);

    MapChunkSample sample;
    for (int i = 0; i < 256; ++i) {
        sample.surface[i] = kStone;
        sample.height[i] = 64;
    }
    for (i32 cz = -1; cz <= 1; ++cz) {
        for (i32 cx = -1; cx <= 1; ++cx) {
            store.store(cx, cz, sample);
        }
    }

    const MapWindow window{0, 0, 16, 16};
    MapStyle style;

    // Nine chunks are in range of a 16-pixel window only because it needs the
    // one to the north as well; what matters is that the second pass draws
    // nothing at all.
    const int first = refreshMapWindow(store, palette, window, style, 1);
    CHECK(first > 0);
    CHECK_EQ(refreshMapWindow(store, palette, window, style, 1), 0);

    // A new stamp -- a texture pack, or the grid being toggled -- makes every
    // one of them stale again.
    CHECK_EQ(refreshMapWindow(store, palette, window, style, 2), first);
}

TEST(a_chunk_arriving_redraws_the_one_south_of_it)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(64);

    MapChunkSample here;
    for (int i = 0; i < 256; ++i) {
        here.surface[i] = kStone;
        here.height[i] = 64;
    }
    store.store(0, 0, here);

    const MapWindow window{0, 0, 16, 16};
    MapStyle style;
    Canvas canvas(16, 16);

    drawWindow(store, palette, window, style, canvas.surface());
    // Nothing north of it yet, so the first row shades level.
    CHECK_EQ(canvas.at(0, 0), palette.shaded[1][kStone]);

    // The chunk to the north arrives, six blocks higher. **The patch already
    // drawn is now wrong**, and the only thing that knows is the store.
    MapChunkSample north;
    for (int i = 0; i < 256; ++i) {
        north.surface[i] = kStone;
        north.height[i] = 70;
    }
    store.store(0, -1, north);

    CHECK(store.patchStale(0, 0, 1));
    drawWindow(store, palette, window, style, canvas.surface());
    CHECK_EQ(canvas.at(0, 0), palette.shaded[0][kStone]);
    CHECK_EQ(canvas.at(0, 1), palette.shaded[1][kStone]);
}

TEST(a_patch_is_stored_with_z_running_backwards)
{
    // The layout the `memcpy` path depends on, stated as a test rather than as
    // a comment: pixel (x, z) of a chunk is at `x * 16 + (15 - z)`. If this
    // ever changes, the fast path in renderMapWindow silently draws the map
    // upside down inside every chunk, which is the kind of wrong that looks
    // like noise rather than like a flip.
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(4);

    MapChunkSample sample;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            // Water, so the shade comes from depth and parity and every pixel
            // of the chunk can be told from every other.
            sample.surface[x * 16 + z] = kWater;
            sample.height[x * 16 + z] = 62;
            sample.depth[x * 16 + z] = u8(1 + ((x + 2 * z) % 12));
        }
    }
    store.store(0, 0, sample);

    MapStyle style;
    refreshMapWindow(store, palette, MapWindow{0, 0, 16, 16}, style, 1);

    Canvas canvas(16, 16);
    drawWindow(store, palette, MapWindow{0, 0, 16, 16}, style, canvas.surface());

    const MapPixel* patch = store.patch(0, 0);
    CHECK(patch != nullptr);
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            CHECK_EQ(patch[patchIndex(x, z)], canvas.at(x, z));
            CHECK_EQ(patch[x * 16 + (15 - z)], canvas.at(x, z));
        }
    }
}
