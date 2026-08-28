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
//     |  [ Map ]   [ Items ]   [ Look ]        |  the tab strip, hud.hpp's
//     |                                        |
//     |  +--------+  +----------------------+  |
//     |  | X  -12 |  |                      |  |
//     |  +--------+  |                      |  |
//     |  +--------+  |        the map       |  |
//     |  | Y   71 |  |                      |  |
//     |  +--------+  |                      |  |
//     |  +--------+  |                      |  |
//     |  | Z  456 |  |                      |  |
//     |  +--------+  +----------------------+  |
//     |  3DAlpha                               |
//     |                                        |
//     |  START pause    SELECT+Y debug         |  the footer, hud.hpp's
//     +----------------------------------------+
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
// was sampled. Standing still costs nothing at all. Sampling is budgeted to a
// chunk or two a frame, which is what keeps a newly opened world from spending
// a frame scanning a hundred columns.
//
// **The first hardware run measured a redraw at 5,000 microseconds**, which is
// a third of a frame on every block the player crosses, and it was spent
// shading pixels that had all been shaded before. A chunk's sixteen by sixteen
// patch is now drawn once and kept, so a redraw is a `memcpy` per chunk column;
// see map_store.hpp and map_render.hpp.

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
// **192 by 176 rather than 192 by 192.** The window lost sixteen rows to the
// tab strip and the frame around it, which is 33,792 pixels against the 36,864
// that were measured at about 700 microseconds a redraw on a New 3DS -- so the
// copy got cheaper rather than dearer, and the number the design rests on still
// bounds it.
inline constexpr int kMapWidth = 192;
inline constexpr int kMapHeight = 176;
inline constexpr int kMapLeft = 120;
inline constexpr int kMapTop = 32;

// How many characters wide the column beside it is. The frame around the map
// starts at pixel 118, so fourteen columns -- 112 pixels -- is the most that
// can be printed without a glyph landing on it.
inline constexpr int kMapTextColumns = 14;

static_assert(kMapTextColumns * hud::kCell + 6 <= kMapLeft,
              "the text column must stop before the map's frame");
static_assert(kMapTop + kMapHeight <= hud::kFooterTop, "the map must fit above the footer");

class MapScreen {
public:
    // **What is drawn over the terrain, and it is a debug setting because it is
    // a debug question.** The 128-block grid says which of later versions' maps
    // this ground would be on and the 16-block one says where the chunks are;
    // both are claims this project makes about the map that are worth being
    // able to check on the hardware rather than on the host. Neither is
    // something a player wants over their world, which is why the map draws
    // none of them by default and the switch is on the debug settings page
    // rather than under the d-pad.
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

    void cycleGrid(int delta);
    Grid grid() const { return grid_; }
    const char* gridName() const;

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
        bool valid = false;

        bool operator==(const Signature& other) const
        {
            return valid && other.valid && originX == other.originX && originZ == other.originZ
                   && yawStep == other.yawStep && stored == other.stored && grid == other.grid;
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
