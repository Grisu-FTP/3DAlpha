#include "framework.hpp"

#include "core/map/map_palette.hpp"
#include "core/map/map_render.hpp"
#include "core/map/map_sample.hpp"
#include "core/map/map_store.hpp"

#include <cmath>
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

// **A chunk is re-sampled when the world writes into it, and the map has to be
// able to tell "written into" from "looks different".**
//
// The first is the streamer's block-change serial and the second is this: a
// re-sample whose surface is identical to the one held changes nothing anyone
// can see, and must not advance the counter `MapScreen`'s redraw signature
// hangs off, or a player mining a tunnel would re-shade the whole window on
// every frame for a picture that never changes. The serial is still taken, so
// the chunk is not re-read again until the next write into it.
TEST(a_resample_that_changes_nothing_costs_nothing_but_still_takes_the_serial)
{
    MapStore store;
    store.setCapacity(4);

    MapChunkSample sample;
    sample.surface[0] = kStone;
    sample.height[0] = 64;

    CHECK(store.store(3, -5, sample, 11u));
    CHECK_EQ(store.stats().stored, 1u);
    CHECK_EQ(store.sampleSerial(3, -5), 11u);

    // Drawn, so there is a patch to lose.
    CHECK(store.claimPatch(3, -5, 7u) != nullptr);
    CHECK(!store.patchStale(3, -5, 7u));

    // The same ground, re-read after a block changed under the surface.
    CHECK(!store.store(3, -5, sample, 12u));
    CHECK_EQ(store.stats().stored, 1u);
    CHECK_EQ(store.sampleSerial(3, -5), 12u);
    CHECK(!store.patchStale(3, -5, 7u));

    // ...and now something the map can see. The patch goes, and so does the one
    // to the south, whose first row shades against this chunk's last.
    MapChunkSample south;
    south.surface[0] = kStone;
    south.height[0] = 64;
    CHECK(store.store(3, -4, south, 13u));
    CHECK(store.claimPatch(3, -4, 7u) != nullptr);

    sample.height[0] = 70;
    CHECK(store.store(3, -5, sample, 14u));
    CHECK_EQ(store.stats().stored, 3u);
    CHECK_EQ(store.sampleSerial(3, -5), 14u);
    CHECK(store.patchStale(3, -5, 7u));
    CHECK(store.patchStale(3, -4, 7u));

    // A chunk nobody ever stored has no serial to give.
    CHECK_EQ(store.sampleSerial(40, 40), 0u);
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

// Everything the marker is, as numbers: how many pixels it filled, how far the
// furthest of them is from the centre, and where that one is. **The tip and not
// the centroid**, because an arrowhead with a bite out of its back has most of
// its area beside the centre rather than in front of it -- its centroid is less
// than half a pixel forward, which is no signal at all.
struct MarkerShape {
    int count = 0;
    double reach = 0.0;  // the furthest filled pixel, in pixels
    double tipX = 0.0;   // ...and where it is, relative to the centre
    double tipZ = 0.0;
};

MarkerShape measureMarker(const Canvas& canvas, double centreX, double centreZ, MapPixel fill)
{
    MarkerShape shape;
    for (int z = 0; z < canvas.height; ++z) {
        for (int x = 0; x < canvas.width; ++x) {
            if (canvas.at(x, z) != fill) {
                continue;
            }
            const double dx = double(x) + 0.5 - centreX;
            const double dz = double(z) + 0.5 - centreZ;
            const double distance = std::sqrt(dx * dx + dz * dz);
            if (distance > shape.reach) {
                shape.reach = distance;
                shape.tipX = dx;
                shape.tipZ = dz;
            }
            ++shape.count;
        }
    }
    return shape;
}

TEST(a_marker_points_where_the_player_is_looking)
{
    const MapPixel fill = rgb565(255, 255, 255);
    const MapPixel outline = rgb565(0, 0, 0);
    const MapWindow window{0, 0, 32, 32};

    // Minecraft's yaw: 0 looks along +Z, which is south and therefore down the
    // map, and it increases towards -X, which is west and therefore left. The
    // furthest pixel from the centre is the tip, so where the tip is is which
    // way the arrow points.
    struct Case {
        float yaw;
        double x;
        double z;
    };
    const Case cases[4] = {
        {0.0f, 0.0, 1.0},    // south
        {90.0f, -1.0, 0.0},  // west
        {180.0f, 0.0, -1.0}, // north
        {270.0f, 1.0, 0.0},  // east
    };

    for (const Case& one : cases) {
        Canvas canvas(32, 32);
        drawMarker(canvas.surface(), window, 16.5, 16.5, one.yaw, 6.5f, fill, outline);
        const MarkerShape shape = measureMarker(canvas, 16.5, 16.5, fill);
        CHECK(shape.count > 8);
        // The tip is a whole marker's length away along the axis it points
        // down, and on that axis: the arrow is symmetric about it, so anything
        // across it is the raster's rounding and nothing else.
        CHECK(shape.tipX * one.x + shape.tipZ * one.z > 4.5);
        CHECK(std::fabs(shape.tipX * one.z - shape.tipZ * one.x) < 1.5);
    }
}

TEST(a_marker_is_the_same_length_whichever_way_it_points)
{
    // **The fault this replaced.** The old marker put its tick on a whole-block
    // step, so a diagonal tick was the square root of two longer than a
    // straight one and the marker grew as the player turned through it. A
    // rotated shape cannot do that: every angle is the same arrowhead.
    const MapPixel fill = rgb565(255, 255, 255);
    const MapPixel outline = rgb565(0, 0, 0);
    const MapWindow window{0, 0, 32, 32};

    double shortest = 1e9;
    double longest = 0.0;
    for (int step = 0; step < 16; ++step) {
        Canvas canvas(32, 32);
        const float yaw = float(step) * 22.5f;
        drawMarker(canvas.surface(), window, 16.5, 16.5, yaw, 6.5f, fill, outline);
        const MarkerShape shape = measureMarker(canvas, 16.5, 16.5, fill);
        shortest = shape.reach < shortest ? shape.reach : shortest;
        longest = shape.reach > longest ? shape.reach : longest;
    }
    // Within a pixel of each other, which is as close as a raster can be. The
    // old shape differed by a factor of 1.41.
    CHECK(longest - shortest < 1.0);
}

TEST(a_marker_turns_by_less_than_an_eighth_of_a_turn)
{
    // The other half of the same fault: eight directions meant every yaw inside
    // a 45-degree bucket drew exactly the same picture.
    const MapPixel fill = rgb565(255, 255, 255);
    const MapPixel outline = rgb565(0, 0, 0);
    const MapWindow window{0, 0, 32, 32};

    Canvas first(32, 32);
    Canvas second(32, 32);
    drawMarker(first.surface(), window, 16.5, 16.5, 0.0f, 6.5f, fill, outline);
    drawMarker(second.surface(), window, 16.5, 16.5, 20.0f, 6.5f, fill, outline);
    CHECK(first.pixels != second.pixels);

    // ...and the step the screen rounds to is finer still, so the rounding is
    // below what the arrow can draw.
    CHECK(kYawSteps >= 32);
    CHECK_EQ(yawStep(0.0f), 0);
    CHECK_EQ(yawStep(360.0f), 0);
    CHECK_EQ(yawStep(-360.0f), 0);
    CHECK_EQ(yawStep(yawFromStep(9)), 9);
    // Half a step either side still rounds home.
    const float half = 360.0f / float(kYawSteps) * 0.4f;
    CHECK_EQ(yawStep(yawFromStep(9) + half), 9);
    CHECK_EQ(yawStep(yawFromStep(9) - half), 9);
}

TEST(a_marker_outside_the_window_writes_nothing)
{
    const MapPixel fill = rgb565(255, 255, 255);
    const MapPixel outline = rgb565(0, 0, 0);

    Canvas edge(8, 8);
    drawMarker(edge.surface(), MapWindow{0, 0, 8, 8}, -40.0, -40.0, 0.0f, 6.5f, fill, outline);
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
    constexpr int kMapWidth = 208;
    constexpr int kMapHeight = 158;  // 200 before the hotbar and banner bands
    constexpr int kMapLeft = 104;
    constexpr int kMapTop = 42;

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

    const MapWindow window{-96, -96, kMapWidth, kMapHeight};
    MapStyle style;
    style.unexplored = rgb565(20, 22, 34);

    // Row-major, which every other test in this file reads.
    Canvas expected(kMapWidth, kMapHeight);
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
            const bool insideMap = x >= kMapLeft && x < kMapLeft + kMapWidth && y >= kMapTop
                                   && y < kMapTop + kMapHeight;
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

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

namespace {

// The console's own framebuffer layout: stored in columns, bottom-up, so a step
// south is a step *back* through memory. Every `memcpy` path in
// `renderMapWindow` -- the 1:1 one and the column-repeat one magnifying adds --
// is only ever taken when `strideZ == -1`, which the row-major Canvas above can
// never be. So the fast paths were untested by construction, and the way to
// test them is to draw the same window into both layouts and demand the same
// picture rather than to re-derive what the picture should be.
struct ConsoleCanvas {
    int width;
    int height;
    std::vector<MapPixel> pixels;

    ConsoleCanvas(int w, int h) : width(w), height(h), pixels(usize(w * h), 0) {}

    // The origin pixel is the *last* one of the first column, because z runs
    // backwards through memory from there.
    MapSurface surface()
    {
        return MapSurface{pixels.data() + (height - 1), height, -1};
    }
    MapPixel at(int x, int z) const { return pixels[usize(x * height + (height - 1 - z))]; }
};

// A chunk in which no two pixels are the same, so a sample taken from the wrong
// block cannot pass by luck: water shaded by a depth that varies with both
// axes, over a palette whose every tile is a different colour.
MapChunkSample speckledChunk(int salt)
{
    MapChunkSample sample;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            sample.surface[x * 16 + z] = kWater;
            sample.height[x * 16 + z] = 62;
            sample.depth[x * 16 + z] = u8(1 + ((x + 3 * z + salt) % 12));
        }
    }
    return sample;
}

}  // namespace

TEST(zoom_scales_are_powers_of_two_in_one_direction_only)
{
    CHECK_EQ(mapPixelsPerBlock(0), 1);
    CHECK_EQ(mapBlocksPerPixel(0), 1);

    CHECK_EQ(mapPixelsPerBlock(2), 4);
    CHECK_EQ(mapBlocksPerPixel(2), 1);

    CHECK_EQ(mapPixelsPerBlock(-1), 1);
    CHECK_EQ(mapBlocksPerPixel(-1), 2);

    // A chunk is sixteen blocks, and at every level in the range it is a whole
    // number of pixels -- which is the whole of "the grids still land on the
    // lattice". 208 and 200 are the console's own window.
    for (int zoom = kZoomMin; zoom <= kZoomMax; ++zoom) {
        const int chunkPixels = 16 * mapPixelsPerBlock(zoom) / mapBlocksPerPixel(zoom);
        CHECK(chunkPixels >= 1);
        CHECK_EQ(mapWindowBlocks(208, zoom) * mapPixelsPerBlock(zoom) / mapBlocksPerPixel(zoom),
                 208);
        CHECK_EQ(mapWindowBlocks(200, zoom) * mapPixelsPerBlock(zoom) / mapBlocksPerPixel(zoom),
                 200);
    }
}

TEST(a_magnified_window_repeats_each_block_over_a_square_of_pixels)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(4);
    store.store(0, 0, speckledChunk(0));

    // Eight blocks of the chunk, four pixels each.
    Canvas canvas(32, 32);
    drawWindow(store, palette, MapWindow{0, 0, 32, 32, 2}, MapStyle{}, canvas.surface());

    const MapPixel* patch = store.patch(0, 0);
    CHECK(patch != nullptr);
    for (int x = 0; x < 32; ++x) {
        for (int z = 0; z < 32; ++z) {
            CHECK_EQ(canvas.at(x, z), patch[patchIndex(x / 4, z / 4)]);
        }
    }
}

TEST(a_shrunk_window_point_samples_every_nth_block)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(16);
    // Two chunks by two, so the walk has to cross a chunk edge in both axes
    // rather than staying inside one patch.
    for (i32 cz = 0; cz < 2; ++cz) {
        for (i32 cx = 0; cx < 2; ++cx) {
            store.store(cx, cz, speckledChunk(int(cx * 5 + cz * 11)));
        }
    }

    Canvas canvas(16, 16);
    drawWindow(store, palette, MapWindow{0, 0, 16, 16, -1}, MapStyle{}, canvas.surface());

    for (int px = 0; px < 16; ++px) {
        for (int pz = 0; pz < 16; ++pz) {
            const int blockX = px * 2;
            const int blockZ = pz * 2;
            const MapPixel* patch = store.patch(blockX / 16, blockZ / 16);
            CHECK(patch != nullptr);
            CHECK_EQ(canvas.at(px, pz), patch[patchIndex(blockX % 16, blockZ % 16)]);
        }
    }
}

TEST(zoom_widens_the_chunks_a_window_touches)
{
    // 208 pixels at 1:1 is fourteen chunks; shrunk once it is 416 blocks and
    // twenty-seven; magnified twice it is 52 blocks and four.
    i32 minX = 0, minZ = 0, maxX = 0, maxZ = 0;

    windowChunkRange(MapWindow{0, 0, 208, 200, 0}, &minX, &minZ, &maxX, &maxZ);
    CHECK_EQ(maxX - minX + 1, 13);

    windowChunkRange(MapWindow{0, 0, 208, 200, -1}, &minX, &minZ, &maxX, &maxZ);
    CHECK_EQ(maxX - minX + 1, 26);

    windowChunkRange(MapWindow{0, 0, 208, 200, 2}, &minX, &minZ, &maxX, &maxZ);
    CHECK_EQ(maxX - minX + 1, 4);

    // ...and it still lands on the right chunks west of the origin.
    windowChunkRange(MapWindow{-17, -1, 16, 16, -1}, &minX, &minZ, &maxX, &maxZ);
    CHECK_EQ(minX, -2);
    CHECK_EQ(maxX, 0);
    CHECK_EQ(minZ, -1);
    CHECK_EQ(maxZ, 1);
}

TEST(the_consoles_framebuffer_layout_draws_the_same_picture_at_every_zoom)
{
    MapPalette palette;
    buildMapPalette(solidAtlas(), &palette);

    MapStore store;
    store.setCapacity(64);
    for (i32 cz = -1; cz <= 2; ++cz) {
        for (i32 cx = -1; cx <= 2; ++cx) {
            // One chunk of the block deliberately left unsampled, so the
            // unexplored fill is on both paths as well.
            if (cx == 1 && cz == 1) {
                continue;
            }
            store.store(cx, cz, speckledChunk(int(cx * 7 + cz * 13)));
        }
    }

    MapStyle style;
    style.unexplored = rgb565(255, 0, 255);
    style.chunkGrid = true;

    for (int zoom = kZoomMin; zoom <= kZoomMax; ++zoom) {
        const MapWindow window{-9, -5, 24, 24, zoom};

        Canvas rowMajor(24, 24);
        ConsoleCanvas console(24, 24);
        drawWindow(store, palette, window, style, rowMajor.surface(), u32(2 + zoom - kZoomMin));
        renderMapWindow(store, window, style, console.surface());

        for (int x = 0; x < 24; ++x) {
            for (int z = 0; z < 24; ++z) {
                CHECK_EQ(console.at(x, z), rowMajor.at(x, z));
            }
        }
    }
}

TEST(the_marker_is_placed_in_pixels_and_so_moves_with_the_zoom)
{
    // Sixteen blocks of ground; the player stands four blocks into it. At 1:1
    // that is pixel 4, magnified four times it is pixel 16, and shrunk it is
    // pixel 2 -- and the arrow is the same size at all three, because `length`
    // is pixels.
    MapStyle style;
    style.unexplored = 0;

    struct Case {
        int zoom;
        int expectX;
    };
    const Case cases[] = {{0, 4}, {2, 16}, {-1, 2}};

    int width = -1;
    int height = -1;
    for (const Case& c : cases) {
        Canvas canvas(32, 32);
        const MapWindow window{0, 0, 32, 32, c.zoom};
        drawMarker(canvas.surface(), window, 4.5, 4.5, 0.0f, 4.0f, rgb565(255, 255, 255),
                   rgb565(255, 255, 255));

        // The middle of what was drawn. The arrow is a rotated shape rather
        // than a dot, so its extent is what says where it was placed -- and
        // because fill and outline are the same colour here, the extent is the
        // shape's own bounding box and not a raster detail.
        int minX = 32, maxX = -1, minZ = 32, maxZ = -1;
        for (int z = 0; z < 32; ++z) {
            for (int x = 0; x < 32; ++x) {
                if (canvas.at(x, z) == 0) {
                    continue;
                }
                minX = x < minX ? x : minX;
                maxX = x > maxX ? x : maxX;
                minZ = z < minZ ? z : minZ;
                maxZ = z > maxZ ? z : maxZ;
            }
        }
        CHECK(maxX >= 0);
        CHECK(std::abs((minX + maxX) / 2 - c.expectX) <= 1);
        CHECK(std::abs((minZ + maxZ) / 2 - c.expectX) <= 1);

        // ...and the marker is the same size at every zoom, because `length` is
        // pixels rather than blocks. The first case sets the size and the rest
        // have to match it, **to within a pixel**: each zoom puts the centre on
        // a different sub-pixel offset -- 4.5, 18.0 and 2.25 -- and the
        // rasteriser's coverage of the same shape can differ by a pixel across
        // those. What is being ruled out is the marker scaling with the map,
        // which would be four times the size at +2 and not one pixel more.
        if (width < 0) {
            width = maxX - minX;
            height = maxZ - minZ;
            CHECK(width > 0);
            CHECK(height > 0);
        }
        CHECK(std::abs((maxX - minX) - width) <= 1);
        CHECK(std::abs((maxZ - minZ) - height) <= 1);
    }
}
