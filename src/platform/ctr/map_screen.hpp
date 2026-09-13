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
// background colour so the two do not fight. See hud.hpp. (The main menu's
// three preview screens are the one place a render target borrows the bottom
// screen, and only while no world is open -- see menu_preview.hpp.)
//
//     +----------------------------------------+
//     | [ Map ] [ Items ] [ Blocks ] [ Look ]  |  the tab strip, hud.hpp's
//     |  focus banner row, or backdrop           |  hud.hpp's bannerTop()
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
//     | [][][][][][][][][]                     |  the hotbar, hud.hpp's
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
// still costs nothing at all.
//
// **And a chunk is sampled again when the world writes into it**, which is
// what makes this a map of the world rather than of the world as it was the
// first time the player walked past: a house appears as it is built, a lake
// drains as it drains, leaves go when they decay. The map does not watch the
// player -- it watches the block writes, so a fluid spreading two hundred
// blocks away updates on the same rule as a block placed underfoot, and so
// does anything a future entity does to the ground.
//
// **The game never waits for any of it.** That is a rule and not an
// aspiration, and it is what the three pieces below are for.
//
//   * **A queue, and a chunk is on it once.** `WorldStreamer` hands over the
//     columns the world wrote into -- deduped there, so a lake draining for a
//     minute puts two chunks on a list rather than twenty thousand -- and the
//     window walk adds the ground the map has never had. Nothing here scans for
//     work; work arrives, and it arrives once.
//
//   * **A slice of the frame, not a number of chunks.** `update` works the
//     queue down until its microsecond allowance is gone and then stops, in the
//     middle of the queue, and picks it up next frame. A world with a thousand
//     chunks to re-sample costs exactly what a world with three does; it simply
//     takes longer to catch up. The one guarantee is that a frame always does
//     at least one chunk, so the queue cannot stall.
//
//   * **Core 2, when it is free.** On a New 3DS the generation worker has a
//     core to itself, and a console standing still in a world that is already
//     made leaves it idle. A batch of sampling goes there -- see
//     `WorldStreamer::offerColumnWork` -- and generation keeps priority: the
//     worker looks at what the world is owed before it looks at the map, and
//     drops a half-finished batch the moment a column comes up.
//
// The sampling itself is one chunk's 256 downward scans, and the re-shade it
// would normally force is skipped when the new sample is identical to the old
// one -- which is the answer for every block a player mines under a roof. See
// map_store.hpp's `store` and `WorldStreamer::takeChangedColumn`.
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
#include "core/map/map_sample.hpp"
#include "core/map/map_store.hpp"
#include "core/render/world_streamer.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/chunk_queue.hpp"
#include "platform/ctr/hud.hpp"
#include "platform/ctr/renderer.hpp"

namespace mc::ctr {

// The map's rectangle on the 320x240 bottom screen, and the column of text
// beside it.
//
// **It is two rectangles now, because the screen is two shapes.** A gamemode
// with a hotbar starts its pages at 48 and the window is 212 by 162; Spectator
// has no hotbar, starts at 8, and the window is 212 by 202 -- the forty pixels
// the band is not taking, which is the whole of what "use the space" means on
// this page. `mapTop`/`mapHeight` answer for whichever is in force.
//
// **What that costs, carried forward from the one number that was measured.**
// 36,864 pixels redrew in about 700 microseconds on a New 3DS. The banded
// window is 34,344, so roughly 650; the bare one is 42,824, so roughly 815.
// Both are estimates scaled off that single measurement and neither has been
// run on hardware -- docs/3ds-performance.md owes this page a figure either
// way, and the larger one is the one to take it on.
//
// The 4 pixels below are the margin that keeps terrain off the tab strip -- it
// was 8, and half of it was backdrop nobody was using; the frame's two pixels
// above put it flush with the page's top.
inline constexpr int kMapWidth = 212;
inline constexpr int kMapLeft = 104;

constexpr int mapTopFor(int pageTop) { return pageTop + 2; }
constexpr int mapHeightFor(int pageTop) { return hud::kTabTop - mapTopFor(pageTop) - 4; }

inline int mapTop() { return mapTopFor(hud::pageTop()); }
inline int mapHeight() { return mapHeightFor(hud::pageTop()); }

// **The tallest the window can be**, which is what the chunk store has to
// cover: sizing it for the banded window would thrash the moment a player
// switched to Spectator. See `configure`.
inline constexpr int kMapMaxHeight = mapHeightFor(hud::kBarePageTop);   // 202

// How many characters wide the column beside it is. The frame around the map
// starts at pixel 102, so twelve columns -- 96 pixels -- is the most that can
// be printed without a glyph landing on it.
inline constexpr int kMapTextColumns = 12;

constexpr bool mapFits(int pageTop)
{
    return mapTopFor(pageTop) + mapHeightFor(pageTop) <= hud::kTabTop
           && mapTopFor(pageTop) - 2 >= hud::kBannerHeight;
}
static_assert(kMapTextColumns * hud::kCell + 6 <= kMapLeft,
              "the text column must stop before the map's frame");
static_assert(kMapLeft + kMapWidth + 2 <= hud::kScreenWidth,
              "the map's frame must stop before the right edge");
static_assert(mapFits(hud::kBandedPageTop), "the map must fit under a hotbar");
static_assert(mapFits(hud::kBarePageTop), "the map must fit without one");

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

    // **Scrolling the window off the player.** Offsets in blocks, added to the
    // camera's position everywhere the window is derived from it -- so the
    // sampling, the ring order, the redraw signature and the picture all move
    // together and none of them needed to learn about panning.
    //
    // It is the bottom screen's focused circle pad and nothing else drives it;
    // see overlay.hpp. **The marker stays with the player**, which is the point:
    // a panned map is for looking at ground you are not standing on, and it
    // needs to keep showing where you actually are -- `map::drawMarker` clips
    // against the window, so a player scrolled off the edge simply is not drawn.
    //
    // The offsets are clamped to +/- 2^20 blocks. That is not a design limit --
    // the world has none worth naming here -- it is a guard on the `i32` floor
    // in `windowFor`, which a stick held down for an hour would otherwise reach.
    void pan(double blocksEast, double blocksSouth);

    // Back to the player, and back to the player's coordinates in the panel.
    // Called when the focus is let go.
    void clearPan();
    bool panned() const { return panX_ != 0.0 || panZ_ != 0.0; }

    // Once a frame, wherever the streamer's columns are known to be settled --
    // on the console that is after the world tick and before the draw.
    //
    // **Nothing in the game waits for any of this.** It collects what the world
    // changed, puts what that owes onto a queue, and spends a fixed slice of the
    // frame working the queue down; whatever is left waits for the next frame.
    // A world where a lake is draining and a forest is burning does not make
    // this call more expensive, it makes the queue longer.
    //
    // **The streamer is not const**, and that is the whole design: this takes
    // the changed columns off it rather than scanning for them, and hands it a
    // batch of sampling to run on the generation worker when that worker has
    // nothing to generate.
    void update(render::WorldStreamer& world, const Camera& camera);

    // Draws the page: the panel and its coordinates, the frame, and the map.
    // `force` is for after anything cleared or overwrote the bottom screen --
    // a page change, an applet, the pause menu -- because the ordinary path
    // draws nothing at all when nothing has moved.
    //
    // **True when it actually put pixels down**, which most frames it does not.
    // The caller needs that answer for two reasons: the LCD's cache flush, and
    // anything drawn *over* the map -- the focus banner is, and would be
    // silently erased by a redraw it could not see.
    bool draw(const gui::Surface& surface, const Camera& camera, bool force);

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
        // **Not derivable from the origin.** A panned window whose origin
        // happens to land back where the unpanned one was is still a different
        // picture, because the coordinates in the panel say something else and
        // the centre cross is on it.
        bool panned = false;
        bool valid = false;

        bool operator==(const Signature& other) const
        {
            return valid && other.valid && originX == other.originX && originZ == other.originZ
                   && yawStep == other.yawStep && stored == other.stored && grid == other.grid
                   && zoom == other.zoom && panned == other.panned;
        }
    };

    // The window centred on the player. Its origin is a block, so the map's
    // pixel grid is the block grid and a chunk is always sixteen pixels.
    map::MapWindow windowFor(const Camera& camera) const;

    void drawFurniture(const gui::Surface& surface);
    void drawText(const Camera& camera, const map::MapWindow& window);
    void drawPixels(const gui::Surface& surface, const Camera& camera,
                    const map::MapWindow& window, float yawDegrees);

    map::MapStore store_;
    map::MapPalette palette_;
    Signature shown_;
    Grid grid_ = Grid::None;
    // One pixel per block. The level the redraw has a `memcpy` fast path for,
    // and the one the whole map was measured at.
    int zoom_ = 0;

    // How far the window has been scrolled off the player, in blocks. Doubles
    // rather than ints because the stick is analogue and a pan quantised to
    // whole blocks at four pixels a block would step rather than scroll.
    double panX_ = 0.0;
    double panZ_ = 0.0;

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

    // **The chunks this still owes a sample, each one on it once.**
    //
    // Two things put coordinates here and neither of them samples anything: the
    // streamer's change list, drained whole every frame, and the window walk,
    // which finds ground the map has never had. What comes off it is worked
    // through under a clock, so the length of the queue is not a frame cost --
    // it is how far behind the map is, which is a different thing and a
    // recoverable one.
    //
    // **The dedupe is the load-bearing part.** Without it a chunk under a
    // waterfall is queued twenty times a second for as long as the water runs,
    // and the frame's whole allowance goes on re-sampling one chunk that has
    // already been re-sampled. `ChunkQueue::push` answers the second offer of a
    // coordinate in a probe and a compare.
    ChunkQueue pending_;

    // **Look at every chunk in the window and ask whether its sample is still
    // true.** The expensive pass, and the fallback rather than the mechanism:
    // it runs when the streamer says its change list overflowed, which is the
    // one case where "what changed" is not knowable any other way.
    bool resync_ = false;

    // **How long a frame may spend sampling**, in microseconds, checked after
    // each chunk so a frame always does at least one.
    //
    // It was a count of chunks -- eight on an old 3DS, sixteen on a New one --
    // and a count is the wrong unit for a rule that reads "never make the game
    // wait". These are the same work those counts allowed, at the ~50 us a
    // chunk sample is estimated to cost on an ARM11, said in the unit the rule
    // is actually about; if a sample turns out to cost more than that on
    // hardware, this holds and the count would not have.
    u32 sampleMicros_ = 400;

    // **The cold-start allowance**, spent until the map has caught up with the
    // streamer for the first time.
    //
    // At world entry the store is empty and the streamer has published almost
    // nothing, so there is nothing to sample yet and this costs nothing.
    // Columns then arrive over the next second or two and this takes them as
    // fast as they come. 3.2 ms is a frame's worth of hitch at most, at the one
    // moment the game is already known to be catching up. It ends the first
    // time a frame empties the queue.
    static constexpr u32 kPrimeMicros = 3200;
    bool primed_ = false;

    // ------------------------------------------------------------------
    // **What is out on the generation worker.** See
    // `WorldStreamer::offerColumnWork`: the batch is offered at the end of one
    // frame's `update` and collected at the start of the next one's, and the
    // streamer withdraws it before it touches a cell.
    //
    // The samples land here rather than in the store because the store is the
    // main thread's -- `MapStore::store` moves an LRU cursor, invalidates a
    // neighbour's patch and may evict, none of which another thread may do
    // behind this class's back. The worker writes 1 KB into a slot of its own
    // and the main thread puts it away.
    // ------------------------------------------------------------------
    static constexpr int kOffload = render::WorldStreamer::kColumnWorkMax;
    static void sampleOnWorker(void* ctx, int index, const world::ChunkColumn& column);

    i32 offloadX_[kOffload] = {};
    i32 offloadZ_[kOffload] = {};
    // Read on the main thread when the batch is offered, so a block written
    // into one of these columns afterwards leaves the stored serial behind the
    // column's and the chunk is queued again. Conservative in the one direction
    // that is safe.
    u32 offloadSerial_[kOffload] = {};
    map::MapChunkSample offloadSample_[kOffload];
    int offloadCount_ = 0;

    // One chunk of the queue, sampled on this thread. False when there was
    // nothing to do -- the column is not resident, or the sample the store
    // already holds is current.
    bool sampleOne(const render::WorldStreamer& world, i32 chunkX, i32 chunkZ,
                   map::MapChunkSample* scratch);
    void collectOffload(render::WorldStreamer& world);
    void postOffload(render::WorldStreamer& world);
};

}  // namespace mc::ctr
