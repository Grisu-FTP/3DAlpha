#pragma once

// The map page: a picture of the world the player is standing in, with their
// coordinates beside it.
//
// **It is pixels on the console's own text screen, and that is deliberate.**
// The bottom screen belongs to libctru's console -- every message this shell
// says to the player goes through it, from "Saving level.." to a failed texture
// pack -- and taking it away to make a citro3d target would mean rewriting all
// of that and giving up the debug pages. `consoleInit` leaves the screen as a
// plain RGB565 framebuffer with double buffering off, so the map is written
// straight into it beside the text, and the text is given the panel's own
// background colour so the two do not fight. See hud.hpp.
//
//     +----------------------------------------+
//     | [ Map ]   [ Items ]   [ Look ]         |  the tab strip, hud.hpp's
//     |+-------+ +---------------------------+ |
//     ||       | |                           | |
//     ||x  -12 | |                           | |
//     ||       | |                           | |
//     ||       | |                           | |
//     ||y   71 | |          the map          | |
//     ||       | |                           | |
//     ||       | |                           | |
//     ||z  456 | |                           | |
//     ||zoom x2| |                           | |
//     ||grid 16| |                           | |
//     ||3DAlpha| |                           | |
//     |+-------+ +---------------------------+ |
//     +----------------------------------------+
//
// **The d-pad belongs to this page.** Left and right cycle the grid overlay,
// up and down zoom in and out; the two rows above the wordmark say which state
// each is in, because a setting with no readout is one the player has to press
// a button to discover. Nothing else in the game reads the d-pad while a
// player's page is up, so this costs no binding -- see overlay.hpp.
//
// Both used to be elsewhere. The grid was a row on the debug settings page,
// which is a page for the maintainer, and zoom did not exist: the map was one
// pixel per block and that was all it could ever be.
//
// **The strip of button hints along the bottom is gone**, and so are 24 pixels
// of the coordinate column: the map is 208 by 200 where it was 192 by 176,
// which is a quarter more picture. The axis letters are what paid for the
// column -- they are drawn as five-pixel glyphs rather than printed, because a
// character cell is eight wide and a Far Lands coordinate needs all nine of the
// columns that leaves.
//
// **Coordinates and nothing else.** The chunk, the later-version map tile, the
// count of chunks remembered and the redraw time all used to be on here, and
// all four are the maintainer's questions rather than the player's -- a page
// that answers them is a debug page with a picture on it. The redraw time in
// particular is still measured and still worth having, and it is on the Info
// page now, where every other number that exists to be watched already lives.
//
// **What it costs.** A redraw happens only when something moved: the player
// crossed into a new block, turned far enough to move the marker, or a chunk
// was sampled -- or the grid or the zoom changed under the d-pad. Standing
// still costs nothing at all. Sampling is budgeted to a chunk or two a frame,
// which is what keeps a newly opened world from spending a frame scanning a
// hundred columns.
//
// **The first hardware run measured a redraw at 5,000 microseconds**, which is
// a third of a frame on every block the player crosses, and it was spent
// shading pixels that had all been shaded before. A chunk's sixteen by sixteen
// patch is now drawn once and kept, so a redraw is a `memcpy` per chunk column;
// see map_store.hpp and map_render.hpp.
//
// **A second hardware run, after zoom landed, measured 3,283 at zoom -1.** That
// was four times what the 1:1 blit was believed to cost, and the cause was not
// the pixels: the walk was doing 2,704 hash lookups into the store to obtain 182
// distinct patch pointers, and the redraw was also re-scanning every chunk in
// the window for staleness on a frame where the player had only turned. Both
// are fixed -- the pointers are gathered once per chunk column
// (`map_render.hpp`), and the scan is skipped outright when nothing can have
// gone stale (`Refreshed`, below). The host numbers are in those two comments;
// **the console figure that replaces 3,283 has not been taken yet.**

#include "core/gui/paint.hpp"
#include "core/map/map_palette.hpp"
#include "core/map/map_render.hpp"
#include "core/map/map_store.hpp"
#include "core/render/world_streamer.hpp"
#include "core/texture/atlas_image.hpp"
#include "platform/ctr/hud.hpp"
#include "platform/ctr/renderer.hpp"

namespace mc::ctr {

// The map's rectangle on the 320x240 bottom screen, and the column of text
// beside it.
//
// **208 by 200, and it costs what a bigger picture costs.** 41,600 pixels
// against the 36,864 that were measured at about 700 microseconds a redraw on a
// New 3DS, so the copy is an estimated 790 -- a fifth of a millisecond more, on
// a redraw that happens when the player crosses a block or turns far enough to
// move the marker. Not free, and the thing to shrink first if a frame budget
// ever needs it back.
inline constexpr int kMapWidth = 208;
inline constexpr int kMapHeight = 200;
inline constexpr int kMapLeft = 104;
inline constexpr int kMapTop = 32;

// How many characters wide the column beside it is. The frame around the map
// starts at pixel 102, so twelve columns -- 96 pixels -- is the most that can
// be printed without a glyph landing on it.
inline constexpr int kMapTextColumns = 12;

static_assert(kMapTextColumns * hud::kCell + 6 <= kMapLeft,
              "the text column must stop before the map's frame");
static_assert(kMapTop + kMapHeight + 8 <= hud::kScreenHeight, "the map must fit on the screen");

class MapScreen {
public:
    // **What is drawn over the terrain.** The 128-block grid says which of
    // later versions' maps this ground would be on and the 16-block one says
    // where the chunks are.
    //
    // It used to be a debug setting, on the reasoning that both are claims this
    // project makes rather than scenery a player wants -- and the reasoning was
    // half right. They *are* claims, and they are still worth checking on the
    // hardware. But a chunk grid on a map is also the single most useful
    // overlay a Minecraft player has ever been given, and burying it behind a
    // SELECT chord on a maintainer's page was answering the wrong question. It
    // is under the d-pad now, on the map's own page, off by default, and the
    // player is told which of the three states they are in.
    enum class Grid {
        None,
        Tiles,
        ChunksAndTiles,
        Count,
    };

    // Sizes the store. A chunk costs 1,536 bytes now that its drawn patch is
    // kept beside its sample.
    //
    // **These numbers are set by the widest zoom, not by the default one.** At
    // 1:1 the window touches about 196 chunks and 512 would have been generous;
    // zoomed one level out it covers four times the ground -- 27 by 26 chunks,
    // 702 of them -- and a store that cannot hold the window at all does not
    // degrade gracefully. It thrashes: the ring scan touches the centre first,
    // so the least-recently-used entry is the ground under the player, and
    // eviction would take exactly the chunks being looked at. So 768 on an old
    // 3DS (1.13 MB) and 1,536 on a New one (2.25 MB), against the 36 MB and
    // 75 MB of newlib heap those consoles now get -- 3% and 3%, for a store
    // that covers the widest window with room left to remember ground that has
    // scrolled off it.
    //
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

    // The d-pad, on the map's own page: left and right through the grids, up
    // and down through the zoom levels.
    //
    // **The grid wraps and the zoom clamps**, because they are different kinds
    // of thing. Three named states with no order between them are a cycle; a
    // zoom is a line with two ends, and coming out of the far end of a magnified
    // map into the widest one is the sort of jump a player has to undo rather
    // than one they meant.
    void cycleGrid(int delta);
    Grid grid() const { return grid_; }
    const char* gridName() const;

    // `delta > 0` magnifies. Clamped to map::kZoomMin..kZoomMax; a step that
    // would leave the range does nothing at all.
    void cycleZoom(int delta);
    int zoom() const { return zoom_; }
    const char* zoomName() const;

    // Once a frame, wherever the streamer's columns are known to be settled.
    // Samples at most a chunk or two, so a world that has just opened fills the
    // map in over a second or so rather than in one frame.
    void update(const render::WorldStreamer& world, const Camera& camera);

    // Draws the page: the panel and its coordinates, the frame, and the map.
    // `force` is for after anything cleared or overwrote the bottom screen --
    // a page change, an applet, the pause menu -- because the ordinary path
    // draws nothing at all when nothing has moved.
    void draw(const gui::Surface& surface, const Camera& camera, bool force);

    const map::MapStore& store() const { return store_; }

    // **What the last redraw cost, in microseconds. This is the number that
    // found the patch cache**, and it is still measured because it is the only
    // honest way to know: the dev host is thirty to forty times faster here and
    // cannot answer for an ARM11. It is on the Info page rather than on the map
    // itself -- see the note at the top of this file.
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
        // The player's yaw, rounded to `map::kYawSteps`. The marker turns
        // smoothly now rather than snapping to eight compass points, and a
        // redraw is a copy of the whole window, so something has to say when
        // the turn is worth one. See map_render.hpp.
        int yawStep = -1;
        u32 stored = 0;  // the store's counter, so a new chunk forces a redraw
        Grid grid = Grid::None;
        // **Not a stamp bump.** Zoom changes what the window shows and nothing
        // about how a patch was drawn, so a zoom step is one ordinary redraw
        // and the store keeps every patch it had. See map_render.hpp.
        int zoom = 0;
        bool valid = false;

        bool operator==(const Signature& other) const
        {
            return valid && other.valid && originX == other.originX && originZ == other.originZ
                   && yawStep == other.yawStep && stored == other.stored && grid == other.grid
                   && zoom == other.zoom;
        }
    };

    // The window centred on the player. Its origin is a block, so the map's
    // pixel grid is the block grid and a chunk is always sixteen pixels.
    map::MapWindow windowFor(const Camera& camera) const;

    void drawFurniture(const gui::Surface& surface);
    void drawText(const Camera& camera);
    void drawPixels(const gui::Surface& surface, const Camera& camera,
                    const map::MapWindow& window, float yawDegrees);

    map::MapStore store_;
    map::MapPalette palette_;
    Signature shown_;
    Grid grid_ = Grid::None;
    // One pixel per block. The level the redraw has a `memcpy` fast path for,
    // and the one the whole map was measured at.
    int zoom_ = 0;
    u32 lastDrawMicros_ = 0;

    // **What the patches in the store were drawn with.** Bumped by anything a
    // patch depends on that is not the chunk itself -- the palette and the grid
    // -- which makes every one of them stale and redrawn on the next pass.
    // Starts at 1 because `MapStore::store` marks a chunk 0 to mean "never
    // drawn", so a live stamp must never be 0.
    u32 stamp_ = 1;

    // **What the last patch refresh was over**, and why a redraw is allowed to
    // skip the refresh entirely.
    //
    // A patch goes stale exactly two ways -- a chunk is stored, which advances
    // the store's `stored` counter and marks that chunk and the one south of
    // it, or the stamp is bumped by a pack change or a grid change -- and a
    // stale patch can only come *into view* when the window moves or its zoom
    // changes. So a redraw matching all five of these cannot find anything to
    // draw, and the scan over the window's chunks is skipped.
    //
    // It is worth the five fields. The scan is one hash lookup per chunk in the
    // window -- 182 at 1:1, 650 shrunk -- measured on the host at 23.8 and 89.1
    // microseconds against window copies of 88.2 and 399.1, so it is a fifth to
    // a quarter of a redraw. And it is a fifth to a quarter of *the most common*
    // redraw: turning moves the yaw step and nothing else, which is precisely
    // the case this skips.
    struct Refreshed {
        i32 originX = 0;
        i32 originZ = 0;
        int zoom = 0;
        u32 stored = 0;
        u32 stamp = 0;
        bool valid = false;
    };
    Refreshed refreshed_;

    // Where the last "keep these alive" pass was centred, so it runs when the
    // view moves rather than sixty times a second on a player standing still.
    i32 touchedOriginX_ = 0;
    i32 touchedOriginZ_ = 0;
    bool touched_ = false;

    // Chunks sampled per frame once the window is full. Eight on an old 3DS,
    // sixteen on a New one.
    //
    // **This was one and two, and it is why the map came up blank.** Sampling
    // is 1.3 us on the host against the window copy's 16.9, which scales to
    // roughly 50 us on the console -- so one a frame was not a budget, it was
    // an accident: a cold window is up to 196 chunks, which at one a frame is
    // six seconds of a mostly empty picture, and because the scan ran in raster
    // order from the north-west corner, the ground under the marker was not
    // reached until halfway through it. What the player saw was a map that
    // stayed blank until they had walked about for a while. Sixteen a frame is
    // under a millisecond, and the steady case is a handful of chunks arriving
    // from the streamer rather than a full window.
    int sampleBudget_ = 8;

    // **The cold-start budget**, spent until the map has caught up with the
    // streamer for the first time.
    //
    // At world entry the store is empty and the streamer has published almost
    // nothing, so there is nothing to sample yet and this costs a hash lookup
    // per chunk. Columns then arrive over the next second or two and this takes
    // them as fast as they come. 64 chunks is about 3 ms on the console -- a
    // frame's worth of hitch at most, at the one moment the game is already
    // known to be catching up. It ends the first time a whole pass finds
    // nothing left to take.
    static constexpr int kPrimeBudget = 64;
    bool primed_ = false;
};

}  // namespace mc::ctr
