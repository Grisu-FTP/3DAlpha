#include "platform/ctr/overlay.hpp"

#include "core/util/console_text.hpp"
#include "core/util/coord_text.hpp"

#include <3ds.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mc::ctr {

namespace {

// devkitARM's newlib links a printf with no float support unless -u _printf_float
// is forced, so a "%f" here prints nothing at all and costs a trip to hardware
// to discover. Everything is formatted as integers on purpose.
int tenths(float value)
{
    return int(value * 10.0f + 0.5f);
}

constexpr int kSamplesPerUpdate = 20;

// swkbd's filter hook. Runs on the applet's side each time the player presses
// OK, and gets to say "no, and here is why" without closing the keyboard.
//
// `ppMessage` is shown as-is and not freed, so every message written to it has
// to outlive the call. parseCoordinateTriple only ever returns string
// literals, which is the reason it returns `const char*` instead of filling a
// buffer.
SwkbdCallbackResult validateCoordinates(void* user, const char** ppMessage, const char* text,
                                        size_t textlen)
{
    (void)user;

    // The applet hands over a length, so do not assume a terminator. 64 is
    // comfortably above the 47 characters the keyboard is configured to accept;
    // anything longer means the assumption broke rather than that the player
    // typed a lot.
    char buffer[64];
    if (textlen >= sizeof(buffer)) {
        *ppMessage = "too long";
        return SWKBD_CALLBACK_CONTINUE;
    }
    std::memcpy(buffer, text, textlen);
    buffer[textlen] = '\0';

    mc::CoordTriple parsed;
    const char* error = mc::parseCoordinateTriple(buffer, &parsed);
    if (error != nullptr) {
        *ppMessage = error;
        return SWKBD_CALLBACK_CONTINUE;
    }
    return SWKBD_CALLBACK_OK;
}

// libctru's bottom-screen console, from `console.c`: 40 columns, 30 rows,
// cursor addressed from 1.
constexpr int kConsoleWidth = 40;

// Row the pages start on. Rows 1 and 2 are the header, which only a page change
// reprints. The footer is pinned at 28, so a page's body is rows 4..27 -- 24
// rows, and the settings page spends every one of them.
constexpr int kBodyRow = 4;
constexpr int kFooterRow = 28;

void clearScreen()
{
    std::printf("\x1b[2J\x1b[1;1H");
}

// **One row of a page, placed absolutely, clipped, and with no newline in it.**
//
// The overlay used to end every line with `\n` and trust that to mean "next
// row". It does not. libctru wraps at 40 columns, and six of the Info page's
// lines were 41 to 44 characters long before any number grew a digit -- so each
// of them quietly took two rows. The body then ran past row 30, `newRow()`
// scrolled the whole window, and what the player saw was the frame-time line
// several times over, each one sample block older than the one below it. The
// page's own history, marching up the screen, and worse the busier the console
// got because that is when the numbers are widest.
//
// Nothing here can do that. The row is addressed, the text is clipped to the
// console's width, and no newline is ever written -- so the cursor cannot
// advance a row on its own and `newRow()` cannot be reached. A line that is too
// long now loses its tail where you can see it, and a page that asks for a row
// outside its body simply does not get one.
void row(int line, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// A row with nothing on it. Its own function because `row(r, "")` is a
// zero-length printf format, which GCC warns about and is right to.
void blank(int line)
{
    if (line < 1 || line >= kFooterRow + 2) {
        return;
    }
    std::printf("\x1b[%d;1H\x1b[2K", line);
}

void row(int line, const char* fmt, ...)
{
    if (line < 1 || line >= kFooterRow + 2) {
        return;
    }

    char text[160];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    char clipped[sizeof(text) + 8];
    usize written = clipToColumns(text, clipped, sizeof(clipped), kConsoleWidth);
    // Always reset, so a clip that fell inside a colour sequence cannot leak it
    // into the rest of the page.
    std::memcpy(clipped + written, "\x1b[0m", 4);
    clipped[written + 4] = '\0';

    std::printf("\x1b[%d;1H\x1b[2K%s", line, clipped);
}

}  // namespace

void Overlay::begin(const char* worldName, const char* model)
{
    worldName_ = worldName;
    model_ = model;
    dirty_ = true;
}

bool Overlay::handleInput(u32 down, u32 held, DebugSettings* settings, Camera* camera)
{
    // SELECT is the modifier rather than a page key of its own, so the page
    // cycle cannot be hit by accident while flying: X is sprint and Y is the
    // stereo tuner, and both of those read the same buttons.
    if (held & KEY_SELECT) {
        if (down & (KEY_Y | KEY_X)) {
            // +1 forward, -1 back, modulo however many pages there are. The
            // "back" step is kPageCount - 1 rather than -1 so the arithmetic
            // stays unsigned-safe when a page is added.
            const int step = (down & KEY_Y) != 0 ? 1 : kPageCount - 1;
            page_ = Page(((int(page_) + step) % kPageCount));
            cursor_ = 0;
            dirty_ = true;
        }
        return false;
    }

    if (page_ != Page::Settings) {
        return false;
    }

    if (down & (KEY_DUP | KEY_DDOWN)) {
        cursor_ += (down & KEY_DDOWN) != 0 ? 1 : kSettingCount - 1;
        cursor_ %= kSettingCount;
        dirty_ = true;
    }

    // One step per press, never a repeat: a held d-pad on a render distance
    // would rebuild the pool every frame.
    const int delta = (down & KEY_DRIGHT) != 0 ? 1 : ((down & KEY_DLEFT) != 0 ? -1 : 0);
    if (delta == 0) {
        return false;
    }

    dirty_ = true;
    if (cursor_ == 0) {
        const int wanted = settings->renderDistance + delta;
        if (wanted < settings->minDistance || wanted > settings->maxDistance) {
            return false;
        }
        settings->renderDistance = wanted;
        return true;
    }

    if (cursor_ == 1) {
        settings->geometryQuads = !settings->geometryQuads;
        return true;
    }

    if (cursor_ == 2) {
        settings->wireframe = !settings->wireframe;
        return true;
    }

    // Teleport. Either direction opens it, because the row has no value to step
    // through and refusing left would only be a way to be unhelpful.
    //
    // Returns false whatever happens: nothing in DebugSettings changed, so the
    // caller has no pool to rebuild. The camera moved or it did not, and the
    // next frame reads it either way.
    teleportViaKeyboard(camera);
    return false;
}

// The applet. Both screens belong to it while it runs and this thread is
// suspended, which has two consequences the caller depends on:
//
//   * The bottom-screen console is gone when it returns, so `dirty_` is set by
//     the caller to force a full reprint rather than the usual in-place update.
//   * The frame that contains this call takes as long as the player took to
//     type. main.cpp already clamps dt to 0.25 s for exactly this reason -- it
//     was written for the console being suspended, and a keyboard is the same
//     shape of pause -- so nobody flies 400 blocks on the next frame.
//
// It must not be called between C3D_FrameBegin and C3D_FrameEnd. Input is
// handled at the top of the loop, well before the frame opens, so it is not.
bool Overlay::teleportViaKeyboard(Camera* camera)
{
    // Long enough for three signed coordinates at full length with decimals,
    // and short enough that the whole thing sits on one keyboard line.
    constexpr int kMaxText = 48;

    // Prefilled with where the camera is, so the common case is editing one
    // number rather than typing three. Floored rather than truncated, so it
    // names the block the camera is standing in on the negative side too --
    // int(-3.7) is -3, which is the block next door.
    char initial[kMaxText];
    std::snprintf(initial, sizeof(initial), "%d %d %d", int(std::floor(camera->x)),
                  int(std::floor(camera->y)), int(std::floor(camera->z)));

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "x y z");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);

    // **Validated inside the keyboard rather than after it.** SWKBD_FILTER_CALLBACK
    // lets the parser reject bad input with its own message while the keyboard
    // stays open, so a mistyped coordinate is fixed in place instead of
    // dismissing the applet, printing an error onto a console the player is not
    // looking at, and making them navigate back to reopen it.
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    swkbdSetFilterCallback(&swkbd, validateCoordinates, nullptr);

    char text[kMaxText];
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    // **Re-init the console before anything else.** libctru's console caches
    // the framebuffer address at consoleInit and never looks it up again --
    // objdump on console.o shows the only call to gfxGetFramebuffer is inside
    // consoleInit -- while gfx.o reallocates framebuffers through
    // linearAlloc/vramAlloc when the screen format changes and flips which
    // buffer is current. An applet does both, so after one returns, the
    // cached pointer is at best pointing at the wrong buffer.
    //
    // consoleInit is safe to repeat: it calls no allocator at all (memcpy,
    // gfxSetScreenFormat, gfxSetDoubleBuffering, gfxSwapBuffersGpu,
    // gspWaitForEvent, gfxGetFramebuffer, a clear, and setvbuf), so this
    // leaks nothing and costs one VBlank wait on a frame where the player has
    // just spent seconds typing. The caller has already set `dirty_`, which is
    // what reprints the header and the page over the cleared screen.
    consoleInit(GFX_BOTTOM, nullptr);

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }

    // Parsed a second time rather than smuggled out of the callback. The filter
    // has already guaranteed this succeeds; doing it again is a few microseconds
    // against a global, and the callback runs on text that the player may still
    // have edited afterwards in ways the filter is not called for.
    CoordTriple target;
    if (parseCoordinateTriple(text, &target) != nullptr) {
        return false;
    }

    camera->x = target.x;
    camera->y = target.y;
    camera->z = target.z;
    return true;
}

void Overlay::draw(const Renderer& renderer, const render::WorldStreamer& world,
                   const Camera& camera, const FrameTiming& timing, float frameMs,
                   float timeOfDay, const DebugSettings& settings)
{
    accum_.frame += frameMs;
    accum_.draw += renderer.gpuDrawMs();
    accum_.process += renderer.gpuProcessMs();
    accum_.blocked += renderer.blockedMs();
    accum_.submit += renderer.submitMs();
    accum_.walk += timing.walkMs;
    accum_.stream += timing.streamMs;

    const bool tick = ++samples_ >= kSamplesPerUpdate;
    if (tick) {
        const float n = float(samples_);
        shown_ = {accum_.frame / n,  accum_.draw / n, accum_.process / n, accum_.blocked / n,
                  accum_.submit / n, accum_.walk / n, accum_.stream / n};
        accum_ = Accum{};
        samples_ = 0;

        // What the cache did over the block that just ended, against what it
        // had done at the start of it. See the note on ioPrevious_.
        const world::ChunkCache::Stats& io = world.stats().io;
        ioDelta_.mainThreadMicros = io.mainThreadMicros - ioPrevious_.mainThreadMicros;
        ioDelta_.stats = io.stats - ioPrevious_.stats;
        ioDelta_.reads = io.reads - ioPrevious_.reads;
        ioDelta_.writes = io.writes - ioPrevious_.writes;
        ioDelta_.listings = io.listings - ioPrevious_.listings;
        ioDelta_.hits = io.hits - ioPrevious_.hits;
        ioDelta_.misses = io.misses - ioPrevious_.misses;
        ioDelta_.prefetchHits = io.prefetchHits - ioPrevious_.prefetchHits;
        ioDelta_.evicted = io.evicted - ioPrevious_.evicted;
        ioPrevious_ = io;
    }

    // Printing is the expensive part -- libctru's console renders every glyph
    // into the bottom framebuffer on the CPU -- so it happens once per sample
    // block, not once per frame. An overlay that costs several milliseconds
    // would be measuring itself. A page change or a settings edit is the
    // exception: waiting a third of a second to see a button press is worse.
    if (!tick && !dirty_) {
        return;
    }

    // The Normal page has nothing that changes, so once drawn it stays drawn.
    if (page_ == Page::Normal && !dirty_) {
        return;
    }

    if (dirty_) {
        clearScreen();
        // Clipped like everything else: a world name is a path off the SD card
        // and there is nothing stopping it being longer than the screen.
        row(1, "\x1b[32m3DAlpha %s\x1b[0m  %s", mcver::kDisplay, model_);
        row(2, "%s", worldName_);
        dirty_ = false;
    }

    int next = kBodyRow;
    switch (page_) {
    case Page::Normal:
        next = drawNormal();
        break;
    case Page::Info:
        next = drawInfo(renderer, world, camera, timeOfDay);
        break;
    case Page::Storage:
        next = drawStorage(world);
        break;
    case Page::Settings:
        next = drawSettings(renderer, settings, camera);
        break;
    }

    // **Blank what the page did not use.** Pages are different lengths and the
    // Info page's own length moves -- the generation rows come and go with the
    // world -- so a shorter draw over a longer one would leave the tail of the
    // longer one on screen, still being read as current.
    while (next < kFooterRow) {
        blank(next++);
    }

    row(kFooterRow, "SELECT+Y / SELECT+X  change page");
    row(kFooterRow + 1, "START pause");
}

int Overlay::drawNormal()
{
    int r = kBodyRow;
    blank(r++);
    row(r++, "  The hotbar and inventory live here");
    row(r++, "  from M3. Until then:");
    blank(r++);
    row(r++, "  circle pad   move");
    row(r++, "  C-stick      look  (New 3DS)");
    row(r++, "  touch drag   look");
    row(r++, "  L / R        down / up");
    row(r++, "  X            sprint");
    row(r++, "  Y + d-pad    tune the 3D");
    row(r++, "  SELECT+Y     debug pages, and");
    row(r++, "               teleport");
    return r;
}

int Overlay::drawInfo(const Renderer& renderer, const render::WorldStreamer& world,
                      const Camera& camera, float timeOfDay)
{
    const render::ChunkRenderer::FrameStats& frame = renderer.chunks().frameStats();
    const render::VboPool::Stats& pool = renderer.chunks().pool().stats();
    const Renderer::FrameStats& gpu = renderer.frameStats();
    const render::WorldStreamer::Stats& streaming = world.stats();

    constexpr int kKb = 1024;
    constexpr int kMb = 1024 * 1024;

    // **Every line here is measured against 40 columns at its widest values,
    // not its typical ones.** Six of them used to be 41 to 44 characters wide
    // with ordinary numbers in them, which cost a row each and scrolled the
    // page; see row(). The field widths below are the widest each number can
    // actually get -- a Far Lands coordinate is eight digits, a count of cells
    // in the grid is four -- so a busy console does not silently need more room
    // than a quiet one.
    int r = kBodyRow;
    row(r++, "frame %2d.%d ms   fps %2d      %s",
        shown_.frame < 0.0f ? 0 : int(shown_.frame), tenths(shown_.frame) % 10,
        shown_.frame > 0.0f ? int(1000.0f / shown_.frame) : 0, gpu.stereo ? "3D on " : "3D off");
    row(r++, "  GPU draw %2d.%d ms  proc %2d.%d ms", int(shown_.draw), tenths(shown_.draw) % 10,
        int(shown_.process), tenths(shown_.process) % 10);
    // The frame minus the vsync wait. If `busy` is well under 16.7 the console
    // is at its refresh rate and there is nothing to fix; if `vsync` is near
    // zero the CPU is the thing missing frames, and the three numbers under it
    // say which part.
    const float busy = shown_.walk + shown_.stream + shown_.submit;
    row(r++, "  CPU busy %2d.%d ms  vsync %2d.%d ms", int(busy), tenths(busy) % 10,
        int(shown_.blocked), tenths(shown_.blocked) % 10);
    row(r++, "  walk %d.%d  stream %2d.%d  submit %d.%d", int(shown_.walk),
        tenths(shown_.walk) % 10, int(shown_.stream), tenths(shown_.stream) % 10,
        int(shown_.submit), tenths(shown_.submit) % 10);

    blank(r++);
    row(r++, "drawn   %4d sections  %6lu quads", gpu.drawCalls,
        static_cast<unsigned long>(gpu.quads));
    // Splits should read 0. Anything else is a frame that outgrew the GPU
    // command buffer -- harmless now, and the thing to raise
    // ctr::kCommandBufferBytes against if it is happening every frame.
    row(r++, "splits  %4d", gpu.commandSplits);
    // Y + d-pad moves these. The disparity is what a point at infinity gets at
    // full slider, in pixels of the 400 across the top screen; the focal
    // distance is what sits at the screen plane.
    row(r++, "3D      %2d.%d px inf   focus %3d blocks", int(renderer.stereoDisparityPixels()),
        tenths(renderer.stereoDisparityPixels()) % 10, int(renderer.stereoFocalBlocks()));
    row(r++, "walked  %4d seen   %4d frustum-cut", frame.sectionsVisited,
        frame.rejectedByFrustum);
    row(r++, "queued  %4d mesh   %2d done, %2d empty", frame.queued, streaming.meshedThisFrame,
        streaming.emptyThisFrame);

    blank(r++);
    row(r++, "pool %3lu.%lu MB  %4d slots  %5lu KB free",
        static_cast<unsigned long>(pool.resident / kMb),
        static_cast<unsigned long>((pool.resident % kMb) * 10 / kMb), pool.residents,
        static_cast<unsigned long>((pool.reserved - pool.resident) / kKb));
    row(r++, "  %6lu up  %6lu reused  %6lu evict", static_cast<unsigned long>(pool.uploads),
        static_cast<unsigned long>(pool.reused), static_cast<unsigned long>(pool.evictions));
    row(r++, "  %6lu alloc  %6lu refused", static_cast<unsigned long>(pool.allocations),
        static_cast<unsigned long>(pool.failures));

    blank(r++);
    row(r++, "cols  %4d in  %4d pending  %4d absent", streaming.columnsResident,
        streaming.pendingColumns, streaming.columnsMissing);

    // **Only while ground is being made**, and quiet the moment the player is
    // walking over chunks that already exist.
    //
    // `owed` and `ask` answer different questions and the gap between them is
    // the diagnosis: `owed` is columns in range that do not exist yet, `ask` is
    // cells whose group listing has not arrived, so the streamer does not yet
    // know whether the world already has them. Owed high with `ask` at zero is
    // a worker that cannot keep up, and the core named on the left is the first
    // thing to look at. `ask` high with `GATE` is the streamer waiting on the
    // card rather than on the generator -- ordinary for a moment after a
    // chunk-boundary crossing, a fault if it stays.
    //
    // `ms` should read 0: it is main-thread time, so anything above zero means
    // the worker did not start and generation is happening inside the frame,
    // which a player sees as the game stopping. `lost` must read 0 too -- a
    // non-zero one means the generator's cache was too small and columns came
    // back without their neighbours' population. See
    // ChunkGenerator::cacheColumnsFor.
    if (streaming.pendingGeneration > 0 || streaming.generatedThisFrame > 0) {
        row(r++, "gen %-5s owed %4d  ask %4d  done %2d",
            streaming.workerRunning ? workerCore_ : "MAIN", streaming.pendingGeneration,
            streaming.unclassified, streaming.generatedThisFrame);
        // The tail of the row is whichever of the two things is wrong, and
        // `fail` wins because it is the worse one: a sweep that cannot finish
        // means the frontier never advances again, where a gate clears itself
        // as soon as a directory listing lands.
        char tail[16] = {};
        if (streaming.generationFailures != 0) {
            std::snprintf(tail, sizeof(tail), " fail %2lu",
                          static_cast<unsigned long>(streaming.generationFailures));
        } else if (streaming.generationGated) {
            std::snprintf(tail, sizeof(tail), " GATE");
        }
        row(r++, "    %2d.%d ms live %3lu lost %2lu%s",
            int(streaming.generateMicros / 1000), int((streaming.generateMicros % 1000) / 100),
            static_cast<unsigned long>(streaming.generatorPeakLive),
            static_cast<unsigned long>(streaming.generatorEvictedLive), tail);
    }
    row(r++, "  blocks %3lu.%lu MB in the heap",
        static_cast<unsigned long>(streaming.blockBytes / kMb),
        static_cast<unsigned long>((streaming.blockBytes % kMb) * 10 / kMb));
    row(r++, "free  linear %5lu KB  VRAM %5lu KB",
        static_cast<unsigned long>(renderer.freeLinearBytes() / kKb),
        static_cast<unsigned long>(renderer.freeVramBytes() / kKb));

    blank(r++);
    // **Two rows, because the Far Lands are the point.** x reaches 12,550,824
    // and its chunk 784,426, and a single row wide enough for both plus labels
    // is 50 characters. The one place these numbers matter most is the one
    // place they would have been clipped.
    row(r++, "xyz %8d %4d %8d", int(camera.x), int(camera.y), int(camera.z));
    row(r++, "chunk %6d %6d   time %2d:%02d", int(camera.chunkX()), int(camera.chunkZ()),
        int(timeOfDay * 24.0f) % 24, int(timeOfDay * 1440.0f) % 60);
    return r;
}

// What the card is doing, and who is waiting for it.
//
// The page exists because the fix it reports on is invisible from every other
// one: a chunk read costs the same microseconds wherever it happens, and the
// only thing that changed is which thread pays them. So the first row is the
// answer -- `main` is main-thread time inside a storage call, and **on this
// build it is expected to be 0.0 always**, not merely usually.
//
// It used to climb whenever the player moved, which is the report this row
// earned its place on: a cell whose directory listing had not arrived fell back
// to a `stat`, that `stat` waits for the storage lock, and the I/O thread holds
// the lock for a whole chunk write. A row of freshly exposed cells behind a
// flush is a second or two of frozen game. Nothing on the render thread asks
// storage anything now -- an unlisted cell waits for its listing instead -- so
// any number above zero here is a fault to chase, not a busy moment.
//
// `hit` is the second thing to read. It counts columns served without an SD
// operation at all -- retained after leaving the grid, or read ahead of the
// player -- and `pre` is how many of those the read-ahead band earned rather
// than retention. A low `pre` with plenty of `hit` means the band is memory
// spent for nothing and can go to zero; both low means the cap is too small for
// the render distance.
int Overlay::drawStorage(const render::WorldStreamer& world)
{
    const render::WorldStreamer::Stats& streaming = world.stats();
    const world::ChunkCache::Stats& io = streaming.io;

    constexpr int kKb = 1024;
    constexpr int kMb = 1024 * 1024;

    int r = kBodyRow;
    // **The first rows are `now`, not `ever`.** `d` is the delta over the last
    // sample block -- two thirds of a second at 30 fps -- and it is the one
    // that answers whether the card is on the render thread *at the moment*.
    // The totals beside them are history: opening a world stats a few hundred
    // chunks before its directory listings land, and that cost then sits in the
    // total for the rest of the session whether or not anything is still wrong.
    // Read as a total this line said 4000 ms on a session that felt perfectly
    // smooth, which is exactly the misreading the split fixes.
    const world::ChunkCache::Stats& d = ioDelta_;

    row(r++, "main %2d.%d ms now          %s",
        int(d.mainThreadMicros / 1000), int((d.mainThreadMicros % 1000) / 100),
        io.workerRunning ? "io thread" : "[31mNO THREAD[0m");
    row(r++, "  %4lu stats now", static_cast<unsigned long>(d.stats));
    row(r++, "  %6lu ms, %6lu stats all session",
        static_cast<unsigned long>(io.mainThreadMicros / 1000),
        static_cast<unsigned long>(io.stats));

    blank(r++);
    row(r++, "ops  %4lu rd %4lu wr %4lu ls  now",
        static_cast<unsigned long>(d.reads), static_cast<unsigned long>(d.writes),
        static_cast<unsigned long>(d.listings));
    row(r++, "     %5lu read %5lu write all", static_cast<unsigned long>(io.reads),
        static_cast<unsigned long>(io.writes));
    row(r++, "queue %4lu rd %4lu wr %4lu ls",
        static_cast<unsigned long>(io.readsQueued), static_cast<unsigned long>(io.writesQueued),
        static_cast<unsigned long>(io.groupsQueued));
    row(r++, "  %4d columns still being read", streaming.pendingReads);

    blank(r++);
    const unsigned long asked = static_cast<unsigned long>(io.hits) + io.misses;
    const unsigned long askedNow = static_cast<unsigned long>(d.hits) + d.misses;
    row(r++, "hit  %4lu miss %4lu  %3lu%%  now", static_cast<unsigned long>(d.hits),
        static_cast<unsigned long>(d.misses),
        askedNow != 0 ? static_cast<unsigned long>(d.hits) * 100 / askedNow : 0);
    row(r++, "     %5lu     %5lu  %3lu%%  all", static_cast<unsigned long>(io.hits),
        static_cast<unsigned long>(io.misses),
        asked != 0 ? static_cast<unsigned long>(io.hits) * 100 / asked : 0);
    row(r++, "  %5lu read ahead and used",
        static_cast<unsigned long>(io.prefetchHits));

    blank(r++);
    row(r++, "cache %3lu.%lu MB  %4lu cols  %5lu evict",
        static_cast<unsigned long>(io.cleanBytes / kMb),
        static_cast<unsigned long>((io.cleanBytes % kMb) * 10 / kMb),
        static_cast<unsigned long>(io.cleanColumns), static_cast<unsigned long>(io.evicted));
    // Owed to the card. It is bounded by the cache's own dirty cap rather than
    // by the autosave timer, so a number that sits near the cap means the I/O
    // thread is being outrun and the generation worker is paying for writes
    // itself -- which is the back-pressure working, not a fault.
    //
    // **The cap is printed because it moves.** It follows the free heap between
    // a floor and a ceiling, so the same dirty figure can be comfortable on one
    // frame and about to cost the worker a write on another; without the
    // denominator the two look identical. See ChunkCache::dirtyCapLocked.
    row(r++, "dirty %4lu KB of %4lu  %4lu cols",
        static_cast<unsigned long>(io.dirtyBytes / kKb),
        static_cast<unsigned long>(io.dirtyCapBytes / kKb),
        static_cast<unsigned long>(io.dirtyColumns));

    blank(r++);
    row(r++, "autosave %s", world.autosaveSeconds() > 0 ? "on" : "off");
    return r;
}

int Overlay::drawSettings(const Renderer& renderer, const DebugSettings& settings,
                          const Camera& camera)
{
    const char* cursor[kSettingCount] = {"  ", "  ", "  ", "  "};
    cursor[cursor_] = "\x1b[33m> \x1b[0m";

    int r = kBodyRow;
    row(r++, "debug settings");
    blank(r++);
    row(r++, "%srender distance   %2d chunks", cursor[0], settings.renderDistance);
    row(r++, "%scube format       %s", cursor[1],
        renderer.cubeFormat() == mesh::CubeFormat::Quads ? "geoshader" : "4-vertex ");
    row(r++, "%swireframe         %s", cursor[2], renderer.wireframe() ? "on " : "off");
    // The row doubles as the readout: after a teleport it shows where you
    // landed, which is the only confirmation the player needs and costs no
    // extra state to keep. Integers because newlib's printf here has no float
    // support -- see the note on tenths().
    row(r++, "%steleport          %d %d %d", cursor[3], int(std::floor(camera.x)),
        int(std::floor(camera.y)), int(std::floor(camera.z)));
    blank(r++);
    row(r++, "d-pad up/down choose, l/r change");
    blank(r++);
    // Every setting here is honest about what it costs, because all three are
    // easy to leave switched on and then wonder at the numbers on the Info
    // page. The page is 30 rows and the footer is pinned at 28, so this text
    // has to earn its lines -- rows 4..27 is the whole budget, and the setting
    // whose question is already answered gives up its share to the one whose
    // is not.
    row(r++, "range %d-%d, the hardware's limit,", settings.minDistance, settings.maxDistance);
    row(r++, "not the player's: menu offers %d/%d.", kPlayMaxDistanceOld3DS,
        kPlayMaxDistanceNew3DS);
    row(r++, "Past ~20 the heap runs out. Changing");
    row(r++, "it rebuilds the pool; columns stay.");
    blank(r++);
    // The one live question on this page, so it gets the room. Says what to
    // look at, because the number that decides it is not on this screen.
    row(r++, "Cube format is the M2 gate's open");
    row(r++, "question. Geoshader sends 8 bytes a");
    row(r++, "quad, not 60. Both re-mesh, so wait");
    row(r++, "then compare GPU draw on Info.");
    // The number the comparison is against, on the screen where the comparison
    // is made. 30 fps is 33.3 ms a frame, less the ~0.8 ms fixed cost.
    row(r++, "Gate: 30 fps at %d, 3D on, so GPU", kGateDistanceNew3DS);
    row(r++, "draw under 32 ms both eyes.");
    blank(r++);
    row(r++, "Wireframe is a texture, not lines.");
    // **Wireframe gave up a line of explanation for these.** The
    // budget is rows 4..27 and it was already exactly full, so the teleport row
    // and its hint had to come from somewhere. Wireframe was the cheapest to
    // shorten: you switch it on and the screen tells you what it does, which is
    // not true of either of the other two settings or of this one -- nothing on
    // the console would ever hint that the interesting coordinate is 12,550,824.
    row(r++, "Teleport: type x y z. The Far Lands");
    row(r++, "are at 12550824 70 0.");
    return r;
}

}  // namespace mc::ctr
