#pragma once

// The spectator bottom screen: a map of the world, drawn from the blocks the
// game already has in memory, with the player's coordinates beside it.
//
// **It is pixels on the console's own text screen, and that is deliberate.**
// The bottom screen belongs to libctru's console -- every message this shell
// says to the player goes through it, from "Saving level.." to a failed texture
// pack -- and taking it away to make a citro3d target would mean rewriting all
// of that and giving up the debug pages. `consoleInit` leaves the screen as a
// plain RGB565 framebuffer with double buffering off, so the map is written
// straight into it beside the text. The two never fight because the layout
// keeps them apart: the text column is clipped to sixteen characters and the
// map starts at the seventeenth.
//
//     +----------------------------------------+
//     | 3DAlpha a1.1.2   New 3DS               |  rows 1-2, the overlay's
//     | MyWorld                                |
//     |                                        |
//     | X    -1234  |##########################|  row 4 .. row 27
//     | Y       71  |####   the map   #########|  x = 128 .. 319
//     | Z      456  |##########################|
//     | ...         |##########################|
//     |                                        |
//     | SELECT+Y / SELECT+X  change page       |  rows 28-29, the overlay's
//     +----------------------------------------+
//
// **What it costs.** A redraw happens only when something moved: the player
// crossed into a new block, turned far enough to change compass point, or a
// chunk was sampled. Standing still costs nothing at all. Sampling is budgeted
// to a chunk or two a frame, which is what keeps a newly opened world from
// spending a frame scanning a hundred columns.
//
// **The first hardware run measured a redraw at 5,000 microseconds**, which is
// a third of a frame on every block the player crosses, and it was spent
// shading 36,864 pixels that had all been shaded before. A chunk's sixteen by
// sixteen patch is now drawn once and kept, so a redraw is a `memcpy` per chunk
// column; see map_store.hpp and map_render.hpp. The screen still prints what
// the last one took, because that is the number that found this.

#include "core/map/map_palette.hpp"
#include "core/map/map_render.hpp"
#include "core/map/map_store.hpp"
#include "core/render/world_streamer.hpp"
#include "core/texture/atlas_image.hpp"
#include "platform/ctr/renderer.hpp"

namespace mc::ctr {

// The map's square, in pixels, and where it sits on the 320x240 bottom screen.
// The edges are multiples of eight because that is the console's character
// cell: anything else and a text row would clip into the map.
inline constexpr int kMapPixels = 192;
inline constexpr int kMapLeft = 320 - kMapPixels;  // 128
inline constexpr int kMapTop = 24;                 // under the two header rows

// How many characters wide the column beside it is. The map starts at pixel
// 128, which is character 16.
inline constexpr int kMapTextColumns = kMapLeft / 8;

class MapScreen {
public:
    // **What is drawn over the terrain, and it is a setting because neither
    // answer is right for everyone.** The 128-block grid is the one worth
    // having on by default: it is `MapData`'s own, so it says which of later
    // versions' maps this ground would be on, and nothing else on the screen
    // says that. The chunk grid says something the map is already true about --
    // sixteen pixels to a chunk, always -- and at one pixel per block it is a
    // line every sixteen, which is a lot of lines. So it is one d-pad press
    // away rather than always there.
    enum class Grid {
        None,
        Tiles,
        ChunksAndTiles,
        Count,
    };
    // Sizes the store. A chunk costs 1,536 bytes now that its drawn patch is
    // kept beside its sample, so 512 chunks is 768 KB and 360 blocks square of
    // remembered ground, and 1,280 is 1.9 MB and 570 blocks square -- against
    // the ~21 MB an old 3DS's newlib heap has and the ~40 MB a New one does.
    // Neither is a limit on where the player may go: ground beyond it is
    // sampled again when they come back.
    void configure(bool isNew3DS);

    // A different world. Everything remembered belongs to the old one.
    void reset();

    // Rebuilds the colours from a texture pack. Called when a world opens and
    // again whenever the pack changes, which is the whole reason the store
    // holds block ids rather than colours: a pack change recolours ground that
    // was sampled long ago and is no longer loaded.
    //
    // It also bumps the patch stamp, because every patch on the map was drawn
    // in the old pack's colours.
    void setPalette(const texture::AtlasImage& atlas);

    // The d-pad, which this screen owns because nothing else in a world uses it
    // bare -- the stereo tuner wants Y held and the debug settings page wants
    // SELECT. **It costs the other gamemodes nothing**, which is the point of
    // the screens being separate: a hotbar built on the survival screen later
    // can have the whole d-pad.
    void cycleGrid(int delta);
    Grid grid() const { return grid_; }

    // Once a frame, wherever the streamer's columns are known to be settled.
    // Samples at most a chunk or two, so a world that has just opened fills the
    // map in over a second or so rather than in one frame.
    void update(const render::WorldStreamer& world, const Camera& camera);

    // Draws the page: the coordinate column as console text, the map as pixels.
    // `force` is for after anything cleared or overwrote the bottom screen --
    // a page change, an applet, the pause menu -- because the ordinary path
    // draws nothing at all when nothing has moved.
    void draw(const Camera& camera, bool force);

    const map::MapStore& store() const { return store_; }

    // **What the last redraw cost, in microseconds. This is the number that
    // found the patch cache**, and it stays on the screen because it is the
    // only honest way to know: the dev host is thirty to forty times faster
    // here and cannot answer for an ARM11.
    //
    // It covers both halves -- redrawing whatever patches went stale, then
    // copying the window -- so a texture-pack change or a grid toggle shows up
    // as one expensive frame and everything else as the copy.
    u32 lastDrawMicros() const { return lastDrawMicros_; }

private:
    // What the last redraw was of. A redraw happens when this changes, and not
    // otherwise: the map is a picture of a block grid, so a player moving
    // within one block has not changed it.
    struct Signature {
        i32 originX = 0;
        i32 originZ = 0;
        int facing = -1;
        u32 stored = 0;  // the store's counter, so a new chunk forces a redraw
        Grid grid = Grid::Tiles;
        bool valid = false;

        bool operator==(const Signature& other) const
        {
            return valid && other.valid && originX == other.originX && originZ == other.originZ
                   && facing == other.facing && stored == other.stored && grid == other.grid;
        }
    };

    // The window centred on the player. Its origin is a block, so the map's
    // pixel grid is the block grid and a chunk is always sixteen pixels.
    map::MapWindow windowFor(const Camera& camera) const;

    void drawText(const Camera& camera, const map::MapWindow& window);
    bool drawPixels(const Camera& camera, const map::MapWindow& window);

    map::MapStore store_;
    map::MapPalette palette_;
    Signature shown_;
    Grid grid_ = Grid::Tiles;
    u32 lastDrawMicros_ = 0;

    // **What the patches in the store were drawn with.** Bumped by anything a
    // patch depends on that is not the chunk itself -- the palette and the grid
    // -- which makes every one of them stale and redrawn on the next pass.
    // Starts at 1 because `MapStore::store` marks a chunk 0 to mean "never
    // drawn", so a live stamp must never be 0.
    u32 stamp_ = 1;

    // Where the last "keep these alive" pass was centred, so it runs when the
    // view moves rather than sixty times a second on a player standing still.
    i32 touchedOriginX_ = 0;
    i32 touchedOriginZ_ = 0;
    bool touched_ = false;

    // Chunks sampled per frame. One on an old 3DS, two on a New one -- the same
    // split the streamer's own column budget uses, and for the same reason.
    int sampleBudget_ = 1;
};

}  // namespace mc::ctr
