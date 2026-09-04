#include "platform/ctr/map_screen.hpp"

#include "core/map/map_sample.hpp"
#include "core/util/math.hpp"

#include <3ds.h>

#include <algorithm>
#include <cmath>

namespace mc::ctr {

namespace {

constexpr float kPi = 3.14159265358979f;

// Unexplored ground. Dark, and blue rather than black, so the edge of what has
// been seen reads as "not yet" rather than as a hole in the screen.
const map::MapPixel kUnexplored = map::rgb565(20, 22, 34);

// The 128-block grid later versions centre their maps on. Deliberately a colour
// no terrain is: it is a reference line, not scenery.
const map::MapPixel kTileGrid = map::rgb565(150, 60, 60);

const map::MapPixel kMarkerFill = map::rgb565(255, 255, 255);
const map::MapPixel kMarkerOutline = map::rgb565(0, 0, 0);

// **Where a scrolled map is looking**, in the same amber the focus cursor uses
// on the hotbar and the palette -- it is the same idea in a different place:
// this is where you are pointing, as against where you are. The cross is drawn
// only while the window is panned, because unpanned the marker is already at
// the centre and a second mark on top of it would say nothing.
const map::MapPixel kPanCross = map::rgb565(255, 210, 74);
constexpr u32 kPannedText = 0xFFD24A;
constexpr int kPanCrossArm = 5;
constexpr int kPanCrossGap = 2;

// The arrow, centre to tip -- so about eleven pixels of marker on a map that is
// 192 across. Bigger than that and it stops being an indicator and starts being
// terrain the player cannot see under; smaller and the raster has too little to
// work with and the shape changes as it turns.
constexpr float kMarkerLength = 6.5f;

// The panel beside the map, and the three readouts in it. It runs the full
// height of the map's frame, flush to the left edge of the screen, because
// every pixel it does not use is one the map does.
constexpr int kPanelX = 0;
constexpr int kPanelY = kMapTop - 2;
constexpr int kPanelW = 96;
constexpr int kPanelH = kMapHeight + 4;

constexpr int kFieldX = kPanelX + 4;
constexpr int kFieldW = kPanelW - 8;
constexpr int kFieldH = 24;
// Evenly spaced down the panel with the wordmark under them, which is what
// keeps three numbers from reading as a column that ran out of things to say.
//
// **The pitch went from 56 to 40 as the screen grew its two bands.** The panel
// is as tall as the map's frame and the map lost 42 pixels between the hotbar
// and the focus banner, so everything in this column moved rather than the
// wordmark being dropped -- see hud.hpp. The boxes are unchanged at 24 tall;
// only the gaps between them are smaller.
constexpr int kFieldY[3] = {48, 88, 128};
// The console row each one's text sits on: the middle of its box, which is
// `y / 8 + 2` for a box that starts on a cell boundary.
constexpr int kFieldRow[3] = {8, 13, 18};
// The two d-pad settings, directly under the z box (which ends at pixel 152, so
// row 20). Plain text on the panel rather than boxed readouts: they are what
// the map is showing itself as, not measurements of the world, and a bevelled
// slot around each would have made the column read as six numbers instead of
// three.
constexpr int kZoomRow = 20;
constexpr int kGridRow = 21;
// Row 22 is left blank, which is what separates what the map is showing from
// what the program is.
constexpr int kWordmarkRow = 23;

// **Everything in the panel has to stop inside it**, and the panel is now the
// map's height rather than the screen's. Checked here because the wordmark used
// to sit at row 27 -- pixel 208 -- which is where the hotbar starts.
static_assert(kFieldY[2] + kFieldH <= kPanelY + kPanelH, "the z box must fit the panel");
static_assert((kWordmarkRow + 1) * hud::kCell <= kPanelY + kPanelH,
              "the wordmark must fit the panel");

// The axis letter, as pixels. **It is five wide where a character cell is
// eight**, and that is the whole reason the column fits in 96 pixels: a Far
// Lands coordinate is nine characters -- `-12550824` -- and nine cells is 72 of
// the 88 the box has inside it.
constexpr hud::Letter kFieldLetter[3] = {hud::Letter::X, hud::Letter::Y, hud::Letter::Z};
constexpr int kLetterX = kFieldX + 4;
// The first text column of the number, and how many it gets. Columns are
// 1-based, so column 3 is the cell starting at pixel 16 -- clear of the letter,
// which ends at 12.
constexpr int kValueColumn = 3;
constexpr int kValueColumns = 9;

// ...and the two d-pad rows under them: column 2 to column 11, pixels 8 to 88.
constexpr int kSettingColumn = 2;
constexpr int kSettingColumns = 10;

}  // namespace

void MapScreen::configure(bool isNew3DS)
{
    store_.setCapacity(isNew3DS ? 1536 : 768);
    sampleBudget_ = isNew3DS ? 16 : 8;
    // A new world, so the store knows nothing about it: burst until the window
    // has caught up with what the streamer can give. See kPrimeBudget.
    primed_ = false;
    shown_ = Signature{};
    refreshed_ = Refreshed{};
}

void MapScreen::cycleGrid(int delta)
{
    constexpr int kCount = int(Grid::Count);
    // The step for "back" is kCount - 1 rather than -1, so the arithmetic stays
    // unsigned-safe if a fourth state is ever added.
    const int step = delta > 0 ? 1 : kCount - 1;
    grid_ = Grid((int(grid_) + step) % kCount);
    // Every patch has the old grid drawn into it.
    ++stamp_;
}

const char* MapScreen::gridName() const
{
    return grid_ == Grid::None ? "off" : (grid_ == Grid::Tiles ? "128" : "16/128");
}

void MapScreen::cycleZoom(int delta)
{
    const int wanted = zoom_ + (delta > 0 ? 1 : -1);
    if (wanted < map::kZoomMin || wanted > map::kZoomMax) {
        return;
    }
    zoom_ = wanted;
    // **No stamp bump, unlike the grid.** A patch is drawn at one pixel per
    // block whatever the zoom is, so every one of them is still current; what
    // changed is which of their pixels the window asks for. The signature
    // carries the zoom, so the next draw is one ordinary redraw.
}

const char* MapScreen::zoomName() const
{
    switch (zoom_) {
    case -1:
        return "1/2";
    case 1:
        return "x2";
    case 2:
        return "x4";
    default:
        break;
    }
    return "1:1";
}

void MapScreen::reset()
{
    store_.clear();
    shown_ = Signature{};
    refreshed_ = Refreshed{};
    clearPan();
}

void MapScreen::pan(double blocksEast, double blocksSouth)
{
    // See the header: an arithmetic guard on the i32 floor in windowFor, not a
    // limit on where the map may look.
    constexpr double kLimit = 1048576.0;
    panX_ += blocksEast;
    panZ_ += blocksSouth;
    panX_ = panX_ < -kLimit ? -kLimit : (panX_ > kLimit ? kLimit : panX_);
    panZ_ = panZ_ < -kLimit ? -kLimit : (panZ_ > kLimit ? kLimit : panZ_);
}

void MapScreen::clearPan()
{
    panX_ = 0.0;
    panZ_ = 0.0;
}

void MapScreen::setPalette(const texture::AtlasImage& atlas)
{
    map::buildMapPalette(atlas, &palette_);
    // Every patch in the store is in the old pack's colours, and nothing about
    // the player has moved to say so.
    ++stamp_;
    shown_ = Signature{};
    refreshed_ = Refreshed{};
}

map::MapWindow MapScreen::windowFor(const Camera& camera) const
{
    map::MapWindow window;
    window.width = kMapWidth;
    window.height = kMapHeight;
    window.zoom = zoom_;

    // How much ground the picture covers, which is what the centring is in
    // terms of -- 208 by 200 blocks at 1:1, a quarter of that magnified twice,
    // four times it shrunk once.
    const i32 blocksWide = map::mapWindowBlocks(kMapWidth, zoom_);
    const i32 blocksHigh = map::mapWindowBlocks(kMapHeight, zoom_);

    // Centred on the block the player is standing in, so the marker sits in the
    // middle and the ground scrolls under it. The origin is a whole block,
    // which is what keeps the pixel grid on the block grid however far the
    // player is from the origin.
    //
    // **Shrunk, it is snapped to the sampling step as well.** At two blocks per
    // pixel an unsnapped origin would flip between the even and the odd blocks
    // as the player walked, so every pixel of the map would change colour on
    // alternate steps -- the picture would shimmer rather than scroll. Snapping
    // costs half a pixel of centring and buys a lattice that never moves.
    //
    // **The pan is added here and nowhere else.** Everything that asks where the
    // map is looking goes through this function -- the sampling rings, the
    // redraw signature, the picture and the panel's numbers -- so scrolling the
    // window is one addition rather than a flag threaded through five callers.
    const i32 step = map::mapBlocksPerPixel(zoom_);
    const double centreX = camera.x + panX_;
    const double centreZ = camera.z + panZ_;
    window.originBlockX = floorDiv(i32(std::floor(centreX)) - blocksWide / 2, step) * step;
    window.originBlockZ = floorDiv(i32(std::floor(centreZ)) - blocksHigh / 2, step) * step;
    return window;
}

void MapScreen::update(const render::WorldStreamer& world, const Camera& camera)
{
    const map::MapWindow window = windowFor(camera);

    i32 minChunkX = 0;
    i32 minChunkZ = 0;
    i32 maxChunkX = 0;
    i32 maxChunkZ = 0;
    map::windowChunkRange(window, &minChunkX, &minChunkZ, &maxChunkX, &maxChunkZ);

    // Keeping what is on screen alive costs a lookup per visible chunk, so it
    // happens when the view has moved rather than every frame -- the answer
    // cannot change while the player stands still, and the counter it advances
    // is what eviction sorts on.
    const bool moved = window.originBlockX != touchedOriginX_
                       || window.originBlockZ != touchedOriginZ_ || !touched_;
    touchedOriginX_ = window.originBlockX;
    touchedOriginZ_ = window.originBlockZ;
    touched_ = true;

    // Hoisted: it is a kilobyte, and constructing one per chunk would zero it
    // before sampleChunk overwrote every byte anyway.
    map::MapChunkSample sample;

    // **Nearest the player first, in square rings**, and the order is the whole
    // difference between a map that fills in and a map that looks broken. The
    // scan used to run in raster order from the north-west corner, so with a
    // budget smaller than the window the ground under the marker -- the only
    // part the player is looking at -- was sampled about halfway through, and
    // everything before that was picture arriving in a corner. Rings put it
    // under the marker first and grow outward, which is also the order the
    // streamer brings the columns in, so the budget is rarely spent on a chunk
    // that is not there yet.
    //
    // The centre is derived from the window rather than from the camera a
    // second time, so the rings are centred on exactly the block the marker is
    // drawn on however far from the origin the player is. See windowFor.
    const i32 centreChunkX = floorDiv(
        window.originBlockX + map::mapWindowBlocks(kMapWidth, window.zoom) / 2, map::kChunkPixels);
    const i32 centreChunkZ = floorDiv(
        window.originBlockZ + map::mapWindowBlocks(kMapHeight, window.zoom) / 2, map::kChunkPixels);
    const i32 reach = std::max(std::max(centreChunkX - minChunkX, maxChunkX - centreChunkX),
                               std::max(centreChunkZ - minChunkZ, maxChunkZ - centreChunkZ));

    int budget = primed_ ? sampleBudget_ : kPrimeBudget;
    bool tookAny = false;

    for (i32 ring = 0; ring <= reach; ++ring) {
        for (i32 chunkZ = centreChunkZ - ring; chunkZ <= centreChunkZ + ring; ++chunkZ) {
            if (chunkZ < minChunkZ || chunkZ > maxChunkZ) {
                continue;
            }
            const bool edgeRow = chunkZ == centreChunkZ - ring || chunkZ == centreChunkZ + ring;
            for (i32 chunkX = centreChunkX - ring; chunkX <= centreChunkX + ring; ++chunkX) {
                // Only the ring itself: every chunk inside it was visited by a
                // smaller `ring`, and revisiting them would make this quartic.
                if (!edgeRow && chunkX != centreChunkX - ring && chunkX != centreChunkX + ring) {
                    continue;
                }
                if (chunkX < minChunkX || chunkX > maxChunkX) {
                    continue;
                }
                if (store_.find(chunkX, chunkZ) != nullptr) {
                    if (moved) {
                        store_.touch(chunkX, chunkZ);
                    }
                    continue;
                }
                if (budget <= 0) {
                    continue;
                }
                // **Only what the game already has in memory.** The map never
                // asks the card for anything: a chunk that is not resident is
                // one the player has not been near yet, and it is sampled the
                // moment the streamer brings it in.
                const world::ChunkColumn* column = world.residentColumn(chunkX, chunkZ);
                if (column == nullptr) {
                    continue;
                }
                map::sampleChunk(*column, &sample);
                store_.store(chunkX, chunkZ, sample);
                tookAny = true;
                --budget;
            }
        }
    }

    // **The cold start ends when a whole pass finds nothing to take.** Not when
    // the window is full: the map reaches further than the render distance can
    // at the smaller settings, so waiting for every chunk would leave the burst
    // budget on for ever. A pass that takes nothing has caught up with whatever
    // the streamer is able to give it, which is the same thing.
    if (!tookAny) {
        primed_ = true;
    }
}

bool MapScreen::draw(const gui::Surface& surface, const Camera& camera, bool force)
{
    const map::MapWindow window = windowFor(camera);
    const float yawDegrees = camera.yaw * 180.0f / kPi;

    Signature now;
    now.originX = window.originBlockX;
    now.originZ = window.originBlockZ;
    now.yawStep = map::yawStep(yawDegrees);
    now.stored = store_.stats().stored;
    now.grid = grid_;
    now.zoom = zoom_;
    now.panned = panned();
    now.valid = true;

    if (!force && now == shown_) {
        return false;
    }

    // The panel, the boxes and the frame do not move, so they are drawn when
    // the screen was cleared and never again. The numbers and the picture are
    // drawn whenever the signature says they changed.
    if (force) {
        drawFurniture(surface);
    }
    drawText(camera, window);
    // **The marker is drawn at the quantised angle, not the real one**, so the
    // signature can never claim the screen holds a picture it does not.
    drawPixels(surface, camera, window, map::yawFromStep(now.yawStep));
    shown_ = now;
    return true;
}

void MapScreen::drawFurniture(const gui::Surface& surface)
{
    hud::panel(surface, kPanelX, kPanelY, kPanelW, kPanelH);
    for (int i = 0; i < 3; ++i) {
        hud::readout(surface, kFieldX, kFieldY[i], kFieldW, kFieldH);
        // The letter is furniture, not text: it never changes, so it is drawn
        // with the box rather than reprinted every time the number moves. One
        // pixel down, which centres seven rows of glyph in an eight-pixel
        // character row.
        hud::drawLetter(surface, kLetterX, kFieldY[i] + hud::kCell + 1, kFieldLetter[i],
                        hud::kReadoutLabel);
    }
    // The frame, two pixels of it, cut into the panel colour the way a slot is
    // -- so the map reads as something set into the screen rather than pasted
    // onto it.
    hud::readout(surface, kMapLeft - 2, kMapTop - 2, kMapWidth + 4, kMapHeight + 4);

    hud::textCentred(kWordmarkRow, kPanelX, kPanelW, hud::kPanelText, hud::kPanelFace, "3DAlpha");
    hud::textCentred(kWordmarkRow + 1, kPanelX, kPanelW, hud::kPanelDark, hud::kPanelFace,
                     mcver::kDisplay);
}

void MapScreen::drawText(const Camera& camera, const map::MapWindow& window)
{
    // Floored rather than truncated, so a coordinate names the block the player
    // is standing in on the negative side of the origin too -- int(-3.7) is -3,
    // which is the block next door. The same rule the teleport row uses.
    //
    // **Panned, x and z name the middle of the picture instead**, because that
    // is the question a scrolled map is being asked: not "where am I" -- the
    // marker still says that, or says you are off the edge -- but "what am I
    // looking at". They are derived from the window rather than from the pan
    // offset, so the number and the pixel under the cross can never disagree.
    // `y` stays the player's: a point on a map has no height.
    const bool scrolled = panned();
    const long centreX =
        (long)(window.originBlockX + map::mapWindowBlocks(kMapWidth, window.zoom) / 2);
    const long centreZ =
        (long)(window.originBlockZ + map::mapWindowBlocks(kMapHeight, window.zoom) / 2);
    const long values[3] = {scrolled ? centreX : (long)i32(std::floor(camera.x)),
                            (long)i32(std::floor(camera.y)),
                            scrolled ? centreZ : (long)i32(std::floor(camera.z))};

    for (int i = 0; i < 3; ++i) {
        // Amber for a coordinate that is the map's rather than the player's --
        // the same colour the focus cursor is, and for the same reason: it is
        // where you are pointing, not where you are.
        const bool mapsOwn = scrolled && i != 1;
        hud::text(kFieldRow[i], kValueColumn, kValueColumns,
                  mapsOwn ? kPannedText : hud::kReadoutText, hud::kReadoutFace, "%*ld",
                  kValueColumns, values[i]);
    }

    // What the d-pad is set to. Printed over the panel's own face rather than a
    // readout's, so they read as labels on the panel and not as two more
    // instruments. `hud::text` pads to the columns it is given, which is what
    // stops "16/128" leaving a tail behind when it cycles back to "off".
    //
    // **Ten columns from column 2**, which is pixels 8 to 88: the same span the
    // coordinate numbers use, and clear of the panel's own bevel at either
    // edge. It is also the whole reason the widest state prints as `grid16/128`
    // with no gap -- four cells of label and six of value is exactly what there
    // is, and losing the space is better than losing a digit.
    hud::text(kZoomRow, kSettingColumn, kSettingColumns, hud::kPanelText, hud::kPanelFace,
              "%-4s%6s", "zoom", zoomName());
    hud::text(kGridRow, kSettingColumn, kSettingColumns, hud::kPanelText, hud::kPanelFace,
              "%-4s%6s", "grid", gridName());
}

void MapScreen::drawPixels(const gui::Surface& surface, const Camera& camera,
                           const map::MapWindow& window, float yawDegrees)
{
    // The framebuffer runs bottom-to-top inside each column, so a step south is
    // a step *back* through memory. That is the whole reason MapSurface carries
    // signed strides instead of a width, and it is what keeps the copy below on
    // its `memcpy` path.
    map::MapSurface target;
    target.pixels = surface.pixels + kMapLeft * surface.strideX + kMapTop * surface.strideY;
    target.strideX = surface.strideX;
    target.strideZ = surface.strideY;

    map::MapStyle style;
    style.unexplored = kUnexplored;
    style.chunkGrid = grid_ == Grid::ChunksAndTiles;
    style.tileGrid = grid_ != Grid::None;
    style.tileGridColour = kTileGrid;

    // **Two steps, and most redraws take only the second.** Refreshing draws
    // the patches that went stale -- the chunk sampled this frame, the one
    // south of it, or every patch in the window after a pack change -- and the
    // copy is what a redraw costs the rest of the time.
    //
    // The refresh is skipped outright when nothing can have gone stale and the
    // window has not moved onto anything that already was; see `Refreshed`. On
    // a redraw driven by the player turning, which is the common one, that is
    // the whole of the first step gone rather than a scan that finds nothing.
    const u64 before = svcGetSystemTick();
    const u32 stored = store_.stats().stored;
    if (!refreshed_.valid || refreshed_.originX != window.originBlockX
        || refreshed_.originZ != window.originBlockZ || refreshed_.zoom != window.zoom
        || refreshed_.stored != stored || refreshed_.stamp != stamp_) {
        map::refreshMapWindow(store_, palette_, window, style, stamp_);
        refreshed_.originX = window.originBlockX;
        refreshed_.originZ = window.originBlockZ;
        refreshed_.zoom = window.zoom;
        refreshed_.stored = stored;
        refreshed_.stamp = stamp_;
        refreshed_.valid = true;
    }
    map::renderMapWindow(store_, window, style, target);

    // The player. **Every other player in a multiplayer session is this call
    // again** with their own position, yaw and colour -- which is why the marker
    // takes a position rather than assuming the centre, even though the window
    // is centred on this one.
    map::drawMarker(target, window, camera.x, camera.z, yawDegrees, kMarkerLength, kMarkerFill,
                    kMarkerOutline);

    // The centre cross, on a scrolled map only. Four arms with a gap in the
    // middle rather than a filled plus, so the pixel the coordinates name is
    // the one pixel the cross does *not* cover -- a mark that hides what it
    // marks is not a mark.
    if (panned()) {
        gui::Surface canvas;
        canvas.pixels = target.pixels;
        canvas.strideX = target.strideX;
        canvas.strideY = target.strideZ;
        canvas.width = window.width;
        canvas.height = window.height;
        const int cx = window.width / 2;
        const int cy = window.height / 2;
        gui::hLine(canvas, cx - kPanCrossGap - kPanCrossArm, cy, kPanCrossArm, kPanCross);
        gui::hLine(canvas, cx + kPanCrossGap + 1, cy, kPanCrossArm, kPanCross);
        gui::vLine(canvas, cx, cy - kPanCrossGap - kPanCrossArm, kPanCrossArm, kPanCross);
        gui::vLine(canvas, cx, cy + kPanCrossGap + 1, kPanCrossArm, kPanCross);
    }

    // Timed before the flush rather than after: the flush is a cache
    // maintenance call whose cost belongs to the screen being software-drawn at
    // all, and mixing the two would hide which of them was the problem.
    lastDrawMicros_ = u32(millisFromTicks(svcGetSystemTick() - before) * 1000.0f);

    // The CPU has just written a buffer the LCD reads by DMA.
    gfxFlushBuffers();
}

}  // namespace mc::ctr
