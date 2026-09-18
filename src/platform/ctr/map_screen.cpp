#include "platform/ctr/map_screen.hpp"
#include "platform/ctr/bottom_screen.hpp"

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
//
// **All of it hangs off the map's own top**, which moves with the hotbar band:
// the three boxes are pitched 40 apart from the top of the panel and the
// wordmark is anchored to its bottom, so a taller panel grows in the middle
// rather than pushing the wordmark off the end. The panel's bottom edge is the
// same pixel in both shapes of the screen -- the tab strip does not move -- so
// the wordmark does not move either.
constexpr int kPanelX = 0;
constexpr int kPanelW = 96;
constexpr int kFieldX = kPanelX + 4;
constexpr int kFieldW = kPanelW - 8;
constexpr int kFieldH = 24;
constexpr int kFieldPitch = 40;

struct Column {
    int panelY, panelH;
    int fieldY[3];
    int fieldRow[3];
    // The two d-pad settings, directly under the z box. Plain text on the panel
    // rather than boxed readouts: they are what the map is showing itself as,
    // not measurements of the world, and a bevelled slot around each would have
    // made the column read as six numbers instead of three.
    int zoomRow, gridRow;
    int wordmarkRow;
};

// The console row a box of `kFieldH` starting at `y` has its text on: the
// middle of the box, and rows are 1-based.
constexpr int rowCentredIn(int y) { return (y + (kFieldH - hud::kCell) / 2) / hud::kCell + 1; }

constexpr Column columnFor(int pageTop)
{
    const int panelY = mapTopFor(pageTop) - 2;
    const int panelH = mapHeightFor(pageTop) + 4;
    const int z = panelY + 2 * kFieldPitch;
    // Two rows of wordmark against the bottom edge, and a blank row above them
    // separating what the map is showing from what the program is.
    const int wordmark = (panelY + panelH) / hud::kCell - 1;
    return Column{panelY,
                  panelH,
                  {panelY, panelY + kFieldPitch, z},
                  {rowCentredIn(panelY), rowCentredIn(panelY + kFieldPitch), rowCentredIn(z)},
                  (z + kFieldH + 2) / hud::kCell + 1,
                  (z + kFieldH + 2) / hud::kCell + 2,
                  wordmark};
}

// **Everything in the panel has to stop inside it**, at both of the screen's
// two shapes, and the settings rows have to stay clear of the wordmark.
constexpr bool columnFits(int pageTop)
{
    const Column c = columnFor(pageTop);
    return c.fieldY[2] + kFieldH <= c.panelY + c.panelH && c.gridRow < c.wordmarkRow
           && (c.wordmarkRow + 1) * hud::kCell <= c.panelY + c.panelH;
}
static_assert(columnFits(hud::kBandedPageTop), "the map's column must fit under a hotbar");
static_assert(columnFits(hud::kBarePageTop), "the map's column must fit without one");

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
    // **Sized for the widest window in the tallest shape of the screen**, which
    // is Spectator's: 212 by 202 pixels at zoom -1 is 424 by 404 blocks, and a
    // patch is a chunk, so the window touches 28 by 27 of them -- 756. Sizing
    // for the banded window instead (28 by 21, 588) would have meant a store
    // that thrashes the moment a player switches mode, and thrashing is the one
    // failure this store degrades into rather than out of: the ring scan
    // touches the centre first, so the least-recently-used entry is the ground
    // under the player.
    //
    // Both numbers therefore went up with the window, keeping the headroom they
    // had: 1,024 on an old 3DS (1.5 MB) and 2,048 on a New one (3 MB), against
    // the 36 MB and 75 MB of newlib heap those consoles get -- 4% of each.
    store_.setCapacity(isNew3DS ? 2048 : 1024);
    sampleMicros_ = isNew3DS ? 800 : 400;

    // **As long as the widest window is wide**, which is the 756 above, with
    // room left for the changed columns arriving beside them. A queue that
    // could not hold one cold window would overflow on the first frame of every
    // world and send the map straight to the fallback pass it is there to
    // avoid.
    pending_.setCapacity(1024);
    // **The same window again, for the band the grid cannot fill.** At the
    // widest zoom and the shortest render distance that band is most of the
    // window, so it is sized as though it were all of it. `asked_` holds the
    // ground already offered and is twice as long, because it also remembers
    // chunks the world does not have -- which is most of a window in a world
    // nobody has explored.
    fill_.setCapacity(1024);
    asked_.setCapacity(2048);
    resync_ = false;
    offloadCount_ = 0;

    // A new world, so the store knows nothing about it: burst until the map has
    // caught up with what the streamer can give. See kPrimeMicros.
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
    pending_.clear();
    fill_.clear();
    asked_.clear();
    resync_ = false;
    offloadCount_ = 0;
    // **The world that was surveyed is closed and has forgotten the visitor**
    // -- `WorldStreamer::close` drops it, with the card's thread already joined
    // -- so the slots are free and the next world registers again.
    for (Survey& survey : surveys_) {
        survey.state.store(u8(SurveyState::Free), std::memory_order_relaxed);
    }
    surveysOut_ = 0;
    surveyorSet_ = false;
    filled_ = 0;
    fillEmpty_ = 0;
    primed_ = false;
    shown_ = Signature{};
    refreshed_ = Refreshed{};
    // The screen is a process-lifetime object and the session is not: a
    // pointer kept here would outlive the world it belonged to.
    players_ = nullptr;
    selfEntityId_ = 0;
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
    window.height = mapHeight();
    window.zoom = zoom_;

    // How much ground the picture covers, which is what the centring is in
    // terms of -- 212 by 162 blocks at 1:1 under a hotbar and 212 by 202
    // without one, a quarter of that magnified twice, four times it shrunk
    // once.
    const i32 blocksWide = map::mapWindowBlocks(kMapWidth, zoom_);
    const i32 blocksHigh = map::mapWindowBlocks(window.height, zoom_);

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

namespace {

// Microseconds since a system tick was taken. The same conversion the redraw
// timer uses; see `lastDrawMicros`.
u32 microsSince(u64 began)
{
    return u32(millisFromTicks(svcGetSystemTick() - began) * 1000.0f);
}

}  // namespace

void MapScreen::sampleOnWorker(void* ctx, int index, const world::ChunkColumn& column)
{
    // **The generation worker's thread, and the only line of this class that
    // runs there.** It reads the column and writes one slot of `offloadSample_`
    // and touches nothing else -- in particular not the store, which is the
    // main thread's. See the note on `offloadSample_`.
    auto* self = static_cast<MapScreen*>(ctx);
    map::sampleChunk(column, &self->offloadSample_[index]);
}

MapScreen::Sampled MapScreen::sampleOne(const render::WorldStreamer& world, i32 chunkX,
                                       i32 chunkZ, map::MapChunkSample* scratch)
{
    // **The column's serial, which is what "this sample is out of date" means.**
    // Zero is a column the streamer does not hold, and a held sample of ground
    // that is no longer resident stays exactly as it was -- it is the last true
    // thing anyone knew about that chunk, and there is nothing to replace it
    // with.
    //
    // **Not resident is not the end of it any more**, which is the one thing
    // that changed here: the caller offers what this refuses to the card. See
    // `postSurveys`.
    const u32 serial = world.columnBlockSerial(chunkX, chunkZ);
    if (serial == 0) {
        return Sampled::NotResident;
    }
    if (store_.find(chunkX, chunkZ) != nullptr && serial == store_.sampleSerial(chunkX, chunkZ)) {
        return Sampled::Current;
    }
    const world::ChunkColumn* column = world.residentColumn(chunkX, chunkZ);
    if (column == nullptr) {
        return Sampled::NotResident;
    }
    map::sampleChunk(*column, scratch);
    store_.store(chunkX, chunkZ, *scratch, serial);
    return Sampled::Took;
}

void MapScreen::surveyOnIo(void* ctx, i32 chunkX, i32 chunkZ, const world::ChunkColumn* column)
{
    // **The I/O thread, and the only lines of this class that run there.** The
    // column is the cache's for the length of this call and nothing but the
    // sample is kept; the slot is this thread's alone from the moment it was
    // published `Posted`.
    auto* self = static_cast<MapScreen*>(ctx);
    for (Survey& survey : self->surveys_) {
        if (survey.state.load(std::memory_order_acquire) != u8(SurveyState::Posted)) {
            continue;
        }
        if (survey.chunkX != chunkX || survey.chunkZ != chunkZ) {
            continue;
        }
        survey.found = column != nullptr;
        if (column != nullptr) {
            map::sampleChunk(*column, &survey.sample);
        }
        survey.state.store(u8(SurveyState::Done), std::memory_order_release);
        return;
    }
    // Nothing is waiting for this one: the world closed and the slots were
    // freed between the request being queued and the card answering it.
}

void MapScreen::collectSurveys()
{
    for (Survey& survey : surveys_) {
        if (survey.state.load(std::memory_order_acquire) != u8(SurveyState::Done)) {
            continue;
        }
        if (survey.found) {
            // **Stored with a serial of zero**, which is what the streamer says
            // about a column it does not hold -- and it is the right answer
            // rather than a placeholder. The moment the grid does adopt this
            // chunk, `adoptColumn` puts it on the change list with a fresh
            // serial, `sampleOne` finds zero against it and samples the live
            // column. Ground read off the card is therefore replaced by ground
            // in memory as soon as there is any, without either side knowing
            // about the other.
            store_.store(survey.chunkX, survey.chunkZ, survey.sample, 0);
            ++filled_;
        } else {
            ++fillEmpty_;
        }
        survey.state.store(u8(SurveyState::Free), std::memory_order_release);
        --surveysOut_;
    }
}

void MapScreen::postSurveys(render::WorldStreamer& world)
{
    if (fill_.empty() || !world.surveyAvailable()) {
        return;
    }
    if (!surveyorSet_) {
        world.setColumnSurveyor(&MapScreen::surveyOnIo, this);
        surveyorSet_ = true;
    }
    for (Survey& survey : surveys_) {
        if (surveysOut_ >= kSurveys) {
            return;
        }
        if (survey.state.load(std::memory_order_acquire) != u8(SurveyState::Free)) {
            continue;
        }
        i32 chunkX = 0;
        i32 chunkZ = 0;
        if (!fill_.pop(&chunkX, &chunkZ)) {
            return;
        }
        survey.chunkX = chunkX;
        survey.chunkZ = chunkZ;
        survey.found = false;
        // Published before the request, so the visitor cannot arrive at a slot
        // that does not yet say which chunk it is for. The I/O thread reads the
        // coordinates only after seeing this.
        survey.state.store(u8(SurveyState::Posted), std::memory_order_release);
        if (!world.surveyColumn(chunkX, chunkZ)) {
            // Refused -- the world shut under us, or this coordinate is still
            // queued down there from a request whose slot has already been
            // collected. Nothing will answer for this slot, so take it back,
            // and put the coordinate at the head of the queue rather than
            // dropping it: `asked_` would never offer it again, and that is a
            // chunk of the map blank for the rest of the session. Posting stops
            // for this frame, which is what keeps a refusal from spinning.
            survey.state.store(u8(SurveyState::Free), std::memory_order_release);
            fill_.push(chunkX, chunkZ);
            return;
        }
        ++surveysOut_;
    }
}

void MapScreen::collectOffload(render::WorldStreamer& world)
{
    int done = 0;
    if (!world.takeColumnWork(&done)) {
        // Either nothing was offered or it is still out there, which on the
        // console cannot happen -- `WorldStreamer::update` runs earlier in the
        // frame and withdraws it. Whatever was offered stays recorded and
        // `postOffload` will not offer again until it comes back.
        return;
    }
    for (int i = 0; i < done && i < offloadCount_; ++i) {
        store_.store(offloadX_[i], offloadZ_[i], offloadSample_[i], offloadSerial_[i]);
    }
    // **What the worker did not reach goes back on the queue**, at the end
    // rather than the front: it is a chunk the world was too busy to look at,
    // and the ones queued behind it have been waiting just as long.
    for (int i = done; i < offloadCount_; ++i) {
        pending_.push(offloadX_[i], offloadZ_[i]);
    }
    offloadCount_ = 0;
}

void MapScreen::postOffload(render::WorldStreamer& world)
{
    if (offloadCount_ != 0 || pending_.empty()) {
        return;
    }
    if (!world.columnWorkAvailable()) {
        return;
    }

    int wanted = 0;
    while (wanted < kOffload && pending_.pop(&offloadX_[wanted], &offloadZ_[wanted])) {
        ++wanted;
    }
    // Coordinates whose column is not resident are dropped by the offer, and
    // the rest are compacted to the front -- the same answer `sampleOne` gives
    // them, reached without a second lookup here.
    const int taken =
        world.offerColumnWork(offloadX_, offloadZ_, wanted, &MapScreen::sampleOnWorker, this);
    for (int i = 0; i < taken; ++i) {
        offloadSerial_[i] = world.columnBlockSerial(offloadX_[i], offloadZ_[i]);
    }
    offloadCount_ = taken;
}

void MapScreen::update(render::WorldStreamer& world, const Camera& camera)
{
    const u64 began = svcGetSystemTick();

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

    // **First, whatever core 2 sampled while this thread was drawing.** Before
    // anything else touches the queue, so a chunk it finished is not also
    // sitting on the queue to be done again.
    collectOffload(world);

    // ...and whatever the card answered, on the same terms and for the same
    // reason. See `postSurveys`.
    collectSurveys();

    // **Then the world's own list of what it wrote into.** This is the whole of
    // "the map follows the world", and it is a pop per *changed chunk* rather
    // than a lookup per *visible chunk*: a lake draining for a minute puts two
    // coordinates here, where the pass this replaced re-asked six hundred
    // chunks on every frame of that minute to find the same two.
    //
    // A chunk that is neither held nor in the window is dropped: the map draws
    // what it remembers and what is on screen, and ground that is neither is
    // ground it has never had a reason to sample.
    resync_ = resync_ || world.changedColumnsOverflowed();
    i32 changedX = 0;
    i32 changedZ = 0;
    while (world.takeChangedColumn(&changedX, &changedZ)) {
        const bool inWindow = changedX >= minChunkX && changedX <= maxChunkX
                              && changedZ >= minChunkZ && changedZ <= maxChunkZ;
        if (inWindow || store_.find(changedX, changedZ) != nullptr) {
            pending_.push(changedX, changedZ);
        }
    }

    // **And the ground the map has never had**, which the change list cannot
    // say anything about after the fact -- a column adopted before this screen
    // existed, or one whose coordinate was lost to an overflow.
    //
    // **Nearest the player first, in square rings**, and the order is the whole
    // difference between a map that fills in and a map that looks broken. The
    // scan used to run in raster order from the north-west corner, so with a
    // budget smaller than the window the ground under the marker -- the only
    // part the player is looking at -- was sampled about halfway through, and
    // everything before that was picture arriving in a corner. Rings put it
    // under the marker first and grow outward, and the queue is FIFO, so that
    // order survives all the way to the sampling.
    //
    // The centre is derived from the window rather than from the camera a
    // second time, so the rings are centred on exactly the block the marker is
    // drawn on however far from the origin the player is. See windowFor.
    if (moved || resync_) {
        const i32 centreChunkX =
            floorDiv(window.originBlockX + map::mapWindowBlocks(window.width, window.zoom) / 2,
                     map::kChunkPixels);
        const i32 centreChunkZ =
            floorDiv(window.originBlockZ + map::mapWindowBlocks(window.height, window.zoom) / 2,
                     map::kChunkPixels);
        const i32 reach = std::max(std::max(centreChunkX - minChunkX, maxChunkX - centreChunkX),
                                   std::max(centreChunkZ - minChunkZ, maxChunkZ - centreChunkZ));

        for (i32 ring = 0; ring <= reach; ++ring) {
            for (i32 chunkZ = centreChunkZ - ring; chunkZ <= centreChunkZ + ring; ++chunkZ) {
                if (chunkZ < minChunkZ || chunkZ > maxChunkZ) {
                    continue;
                }
                const bool edgeRow =
                    chunkZ == centreChunkZ - ring || chunkZ == centreChunkZ + ring;
                for (i32 chunkX = centreChunkX - ring; chunkX <= centreChunkX + ring; ++chunkX) {
                    // Only the ring itself: every chunk inside it was visited by
                    // a smaller `ring`, and revisiting them would make this
                    // quartic.
                    if (!edgeRow && chunkX != centreChunkX - ring
                        && chunkX != centreChunkX + ring) {
                        continue;
                    }
                    if (chunkX < minChunkX || chunkX > maxChunkX) {
                        continue;
                    }
                    const bool held = store_.find(chunkX, chunkZ) != nullptr;
                    if (!held) {
                        pending_.push(chunkX, chunkZ);
                        continue;
                    }
                    if (moved) {
                        store_.touch(chunkX, chunkZ);
                    }
                    // **The fallback, and only ever the fallback.** Asking the
                    // streamer about a chunk the map already holds is the pass
                    // the change list exists to make unnecessary; it runs when
                    // that list overflowed and there is no other way to find out
                    // what was on it.
                    if (resync_ && world.columnBlockSerial(chunkX, chunkZ)
                                       != store_.sampleSerial(chunkX, chunkZ)) {
                        pending_.push(chunkX, chunkZ);
                    }
                }
            }
        }
    }

    // **Cleared by the walk it asked for, and asked for again if the queue
    // itself lost something.** The queue holds a whole cold window with room to
    // spare, so the second half of this is a guard rather than a path -- but a
    // dropped coordinate is a chunk of the map that would otherwise never be
    // redrawn, and the recovery costs one pass.
    resync_ = pending_.overflowed();

    // **The batch for core 2, offered before this thread starts its own**, so
    // the worker has the whole of the rest of the frame to do it in. Refused
    // outright on a console with no spare core and on a frame where a batch is
    // already out; generation is never waiting on it either way -- see
    // `WorldStreamer::offerColumnWork`.
    postOffload(world);

    // **And whatever is left, on this thread, for as long as the frame can
    // spare.** The clock is read after each chunk rather than before, so a
    // frame always samples at least one however little of its allowance is
    // left: the queue can fall behind, but it cannot stall.
    // **A guest has no card**, so there is nothing to offer and no point
    // collecting the offers. Asked once here rather than per chunk below.
    const bool fillFromCard = world.surveyAvailable();

    const u32 allowance = primed_ ? sampleMicros_ : kPrimeMicros;
    map::MapChunkSample scratch;
    bool tookAny = false;
    i32 chunkX = 0;
    i32 chunkZ = 0;
    while (pending_.pop(&chunkX, &chunkZ)) {
        const Sampled took = sampleOne(world, chunkX, chunkZ, &scratch);
        tookAny = took == Sampled::Took || tookAny;
        if (took == Sampled::NotResident && fillFromCard && asked_.push(chunkX, chunkZ)) {
            // **The card's half of the window**, offered only by a chunk the
            // grid has just refused -- so the resident ground is always
            // sampled first and this is exactly the band that is left.
            // `asked_` is what keeps a chunk that is not there at all from
            // being offered again on every step the player takes.
            fill_.push(chunkX, chunkZ);
        }
        if (microsSince(began) >= allowance) {
            break;
        }
    }
    // **Both are read, not short-circuited**: `overflowed` clears the flag it
    // answers with, so a test that skipped the second one would leave it set to
    // fire again on a frame where nothing had overflowed.
    const bool lostAsked = asked_.overflowed();
    const bool lostFill = fill_.overflowed();
    if (lostAsked || lostFill) {
        // Both are bounded and both recover the same way -- by forgetting what
        // has been offered, so the walk offers it again the next time the
        // window moves. A refused `fill_` push is the case that needs it: the
        // coordinate is in `asked_` and would otherwise never be offered again,
        // which is a hole in the map for the rest of the session.
        asked_.clear();
    }

    // **And the requests themselves, last**: the queue they draw on is what
    // this frame's sampling has just added to, and the I/O thread has the whole
    // of the rest of the frame -- and of the next one -- to answer them in.
    postSurveys(world);

    // **The cold start ends when a frame finds nothing to take.** Not when the
    // window is full: the map reaches further than the render distance can at
    // the smaller settings, so waiting for every chunk would leave the burst
    // allowance on for ever. A frame that samples nothing has caught up with
    // whatever the streamer is able to give it, which is the same thing.
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
    now.players = playersDigest(window);
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

u32 MapScreen::playersDigest(const map::MapWindow& window) const
{
    if (players_ == nullptr) {
        return 0;
    }
    // Quantised to the pixel the marker would land on and the angle it would
    // be drawn at, so the digest changes exactly when the picture would --
    // never for a step too small to see, always for one that is not.
    const double perBlock = double(map::mapPixelsPerBlock(window.zoom))
                            / double(map::mapBlocksPerPixel(window.zoom));
    u32 digest = 0;
    for (int i = 0; i < players_->playerCount(); ++i) {
        const net::RemotePlayer& other = players_->player(i);
        if (!other.used) {
            continue;
        }
        const i32 px = i32((other.x - double(window.originBlockX)) * perBlock);
        const i32 pz = i32((other.z - double(window.originBlockZ)) * perBlock);
        // One round of a 32-bit FNV-1a over the three values that move the
        // arrow. Order matters and is the pool's, which is stable between
        // redraws.
        const u32 parts[3] = {u32(px), u32(pz), u32(map::yawStep(other.yaw))};
        for (const u32 part : parts) {
            digest = (digest ^ part) * 16777619u;
        }
    }
    return digest;
}

void MapScreen::drawFurniture(const gui::Surface& surface)
{
    const Column col = columnFor(hud::pageTop());
    hud::panel(surface, kPanelX, col.panelY, kPanelW, col.panelH);
    for (int i = 0; i < 3; ++i) {
        hud::readout(surface, kFieldX, col.fieldY[i], kFieldW, kFieldH);
        // The letter is furniture, not text: it never changes, so it is drawn
        // with the box rather than reprinted every time the number moves. One
        // pixel down, which centres seven rows of glyph in an eight-pixel
        // character row.
        hud::drawLetter(surface, kLetterX, col.fieldY[i] + hud::kCell + 1, kFieldLetter[i],
                        hud::kReadoutLabel);
    }
    // The frame, two pixels of it, cut into the panel colour the way a slot is
    // -- so the map reads as something set into the screen rather than pasted
    // onto it.
    hud::readout(surface, kMapLeft - 2, mapTop() - 2, kMapWidth + 4, mapHeight() + 4);

    hud::textCentred(col.wordmarkRow, kPanelX, kPanelW, hud::kPanelText, hud::kPanelFace,
                     "3DAlpha");
    hud::textCentred(col.wordmarkRow + 1, kPanelX, kPanelW, hud::kPanelDark, hud::kPanelFace,
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
    const Column col = columnFor(hud::pageTop());
    const bool scrolled = panned();
    const long centreX =
        (long)(window.originBlockX + map::mapWindowBlocks(window.width, window.zoom) / 2);
    const long centreZ =
        (long)(window.originBlockZ + map::mapWindowBlocks(window.height, window.zoom) / 2);
    const long values[3] = {scrolled ? centreX : (long)i32(std::floor(camera.x)),
                            (long)i32(std::floor(camera.y)),
                            scrolled ? centreZ : (long)i32(std::floor(camera.z))};

    for (int i = 0; i < 3; ++i) {
        // Amber for a coordinate that is the map's rather than the player's --
        // the same colour the focus cursor is, and for the same reason: it is
        // where you are pointing, not where you are.
        const bool mapsOwn = scrolled && i != 1;
        hud::text(col.fieldRow[i], kValueColumn, kValueColumns,
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
    hud::text(col.zoomRow, kSettingColumn, kSettingColumns, hud::kPanelText, hud::kPanelFace,
              "%-4s%6s", "zoom", zoomName());
    hud::text(col.gridRow, kSettingColumn, kSettingColumns, hud::kPanelText, hud::kPanelFace,
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
    target.pixels = surface.pixels + kMapLeft * surface.strideX + mapTop() * surface.strideY;
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

    // **Everybody else first, so this player's marker is the one on top.** It
    // is the same call with their position, their yaw and their colour -- which
    // is why `drawMarker` takes a position rather than assuming the centre,
    // even though the window is centred on this one.
    //
    // A marker outside the window is clipped by `drawMarker` and costs a few
    // arithmetic operations, so there is nothing here to skip: the pool is four
    // players and three of them are somebody else.
    if (players_ != nullptr) {
        for (int i = 0; i < players_->playerCount(); ++i) {
            const net::RemotePlayer& other = players_->player(i);
            if (!other.used) {
                continue;
            }
            u8 r = 255;
            u8 g = 255;
            u8 b = 255;
            net::playerColour(other.id, &r, &g, &b);
            map::drawMarker(target, window, other.x, other.z, other.yaw, kMarkerLength,
                            map::rgb565(r, g, b), kMarkerOutline);
        }
    }

    // This player, in the colour everybody else is drawing them in. White in
    // single player, which is `playerColour`'s answer for an id nobody gave
    // out.
    u8 selfR = 255;
    u8 selfG = 255;
    u8 selfB = 255;
    net::playerColour(selfEntityId_, &selfR, &selfG, &selfB);
    map::drawMarker(target, window, camera.x, camera.z, yawDegrees, kMarkerLength,
                    selfEntityId_ != 0 ? map::rgb565(selfR, selfG, selfB) : kMarkerFill,
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

    // Copied by the overlay's flush once the band is drawn too, so the frame
    // goes across once rather than twice.
    bottom::changed();
}

}  // namespace mc::ctr
