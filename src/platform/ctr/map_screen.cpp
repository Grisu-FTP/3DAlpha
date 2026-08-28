#include "platform/ctr/map_screen.hpp"

#include "core/map/map_sample.hpp"
#include "core/util/math.hpp"

#include <3ds.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace mc::ctr {

namespace {

// The console's cell, and the bottom screen in the orientation the framebuffer
// is actually stored in: 240 pixels down a column, 320 columns across.
constexpr int kCellPixels = 8;
constexpr int kScreenHeight = 240;
constexpr int kScreenWidth = 320;

// The first console row the page owns and the last, matching overlay.cpp's
// body: rows 1-2 are the header and 28-29 the footer, and both are the
// overlay's to draw.
constexpr int kBodyRow = 4;
constexpr int kLastBodyRow = 27;

static_assert(kMapTop == (kBodyRow - 1) * kCellPixels, "the map must start on a character row");
static_assert(kMapTop + kMapPixels == kLastBodyRow * kCellPixels,
              "the map must end on a character row");
static_assert(kMapLeft % kCellPixels == 0, "the map must start on a character column");

// Unexplored ground. Dark, and blue rather than black, so the edge of what has
// been seen reads as "not yet" rather than as a hole in the screen.
const map::MapPixel kUnexplored = map::rgb565(20, 22, 34);

// The 128-block grid later versions centre their maps on. Deliberately a colour
// no terrain is: it is a reference line, not scenery.
const map::MapPixel kTileGrid = map::rgb565(150, 60, 60);

const map::MapPixel kMarkerFill = map::rgb565(255, 255, 255);
const map::MapPixel kMarkerOutline = map::rgb565(0, 0, 0);

// Index order is map::kFacingStep's: 0 is south, because that is where yaw 0
// looks.
const char* const kCompass[8] = {"S", "SW", "W", "NW", "N", "NE", "E", "SE"};

// **One row of the column beside the map, and it must not erase the line.**
//
// overlay.cpp's `row()` writes `\x1b[2K` first, which blanks all forty columns
// -- and forty columns is the whole screen, map included. So this pads to
// exactly sixteen characters instead and writes nothing past them. No colour
// either, for the same reason: an escape sequence would make the padding
// arithmetic depend on what was printed.
void textRow(int line, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// A row with nothing on it. Its own function for the same reason overlay.cpp's
// `blank()` is: `textRow(r, "")` is a zero-length printf format, which GCC
// warns about and is right to.
void textBlank(int line)
{
    if (line < kBodyRow || line > kLastBodyRow) {
        return;
    }
    std::printf("\x1b[%d;1H%*s", line, kMapTextColumns, "");
}

void textRow(int line, const char* fmt, ...)
{
    if (line < kBodyRow || line > kLastBodyRow) {
        return;
    }
    char text[64];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    std::printf("\x1b[%d;1H%-*.*s", line, kMapTextColumns, kMapTextColumns, text);
}

}  // namespace

void MapScreen::configure(bool isNew3DS)
{
    store_.setCapacity(isNew3DS ? 1280 : 512);
    sampleBudget_ = isNew3DS ? 2 : 1;
    shown_ = Signature{};
}

void MapScreen::cycleGrid(int delta)
{
    constexpr int kCount = int(Grid::Count);
    // The step for "back" is kCount - 1 rather than -1, so the arithmetic stays
    // unsigned-safe if a fourth state is ever added -- the same shape the page
    // cycle in overlay.cpp uses.
    const int step = delta > 0 ? 1 : kCount - 1;
    grid_ = Grid((int(grid_) + step) % kCount);
    // Every patch has the old grid drawn into it.
    ++stamp_;
}

void MapScreen::reset()
{
    store_.clear();
    shown_ = Signature{};
}

void MapScreen::setPalette(const texture::AtlasImage& atlas)
{
    map::buildMapPalette(atlas, &palette_);
    // Every patch in the store is in the old pack's colours, and nothing about
    // the player has moved to say so.
    ++stamp_;
    shown_ = Signature{};
}

map::MapWindow MapScreen::windowFor(const Camera& camera) const
{
    map::MapWindow window;
    window.width = kMapPixels;
    window.height = kMapPixels;
    // Centred on the block the player is standing in, so the marker sits in the
    // middle and the ground scrolls under it. The origin is a whole block,
    // which is what keeps the pixel grid on the block grid however far the
    // player is from the origin.
    window.originBlockX = i32(std::floor(camera.x)) - kMapPixels / 2;
    window.originBlockZ = i32(std::floor(camera.z)) - kMapPixels / 2;
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

    int budget = sampleBudget_;
    for (i32 chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ) {
        for (i32 chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX) {
            if (store_.find(chunkX, chunkZ) != nullptr) {
                if (moved) {
                    store_.touch(chunkX, chunkZ);
                }
                continue;
            }
            if (budget <= 0) {
                continue;
            }
            // **Only what the game already has in memory.** The map never asks
            // the card for anything: a chunk that is not resident is one the
            // player has not been near yet, and it is sampled the moment the
            // streamer brings it in.
            const world::ChunkColumn* column = world.residentColumn(chunkX, chunkZ);
            if (column == nullptr) {
                continue;
            }
            map::sampleChunk(*column, &sample);
            store_.store(chunkX, chunkZ, sample);
            --budget;
        }
    }
}

void MapScreen::draw(const Camera& camera, bool force)
{
    const map::MapWindow window = windowFor(camera);

    Signature now;
    now.originX = window.originBlockX;
    now.originZ = window.originBlockZ;
    now.facing = map::facingFromYaw(camera.yaw * 180.0f / 3.14159265358979f);
    now.stored = store_.stats().stored;
    now.grid = grid_;
    now.valid = true;

    if (!force && now == shown_) {
        return;
    }

    drawText(camera, window);
    if (drawPixels(camera, window)) {
        shown_ = now;
    } else {
        // The framebuffer was not what was expected, so nothing was drawn and
        // nothing may be remembered as drawn.
        shown_ = Signature{};
    }
}

void MapScreen::drawText(const Camera& camera, const map::MapWindow& window)
{
    // Floored rather than truncated, so a coordinate names the block the player
    // is standing in on the negative side of the origin too -- int(-3.7) is -3,
    // which is the block next door. The same rule the teleport row uses.
    const i32 blockX = i32(std::floor(camera.x));
    const i32 blockY = i32(std::floor(camera.y));
    const i32 blockZ = i32(std::floor(camera.z));

    int row = kBodyRow;
    textRow(row++, "X %8ld", (long)blockX);
    textRow(row++, "Y %8ld", (long)blockY);
    textRow(row++, "Z %8ld", (long)blockZ);
    textBlank(row++);
    textRow(row++, "facing %4s", kCompass[map::facingFromYaw(camera.yaw * 180.0f / 3.14159265358979f)]);
    textBlank(row++);
    textRow(row++, "chunk");
    textRow(row++, " %5ld %5ld", (long)floorDiv(blockX, 16), (long)floorDiv(blockZ, 16));
    textBlank(row++);
    // Which of later versions' 128-block maps this ground would be on. It is
    // the grid the red lines are, so the number and the lines explain each
    // other.
    textRow(row++, "map tile");
    textRow(row++, " %5ld %5ld", (long)map::tileOfBlock(blockX), (long)map::tileOfBlock(blockZ));
    textBlank(row++);
    textRow(row++, "seen %5d", store_.stats().chunks);
    textRow(row++, "  of %5d", store_.capacity());
    textBlank(row++);
    textRow(row++, "grid %6s", grid_ == Grid::None          ? "off"
                               : grid_ == Grid::Tiles       ? "128"
                                                            : "16/128");
    textRow(row++, " d-pad");
    textBlank(row++);
    // The previous redraw, because this row is printed before this one happens.
    textRow(row++, "draw %5luus", (unsigned long)lastDrawMicros_);

    (void)window;

    while (row <= kLastBodyRow) {
        textBlank(row++);
    }
}

bool MapScreen::drawPixels(const Camera& camera, const map::MapWindow& window)
{
    u16 framebufferWidth = 0;
    u16 framebufferHeight = 0;
    u8* raw = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &framebufferWidth, &framebufferHeight);

    // **Checked rather than assumed.** libctru reports the framebuffer in the
    // orientation it is stored in -- 240 down a column, 320 columns across --
    // and `consoleInit` has already put the screen in RGB565. If either is not
    // what is expected, something has changed the screen under us and writing
    // pixels at computed offsets is the last thing to do about it.
    if (raw == nullptr || framebufferWidth != kScreenHeight || framebufferHeight != kScreenWidth) {
        return false;
    }

    map::MapPixel* pixels = reinterpret_cast<map::MapPixel*>(raw);

    // The framebuffer runs bottom-to-top inside each column, so a step south is
    // a step *back* through memory. That is the whole reason MapSurface carries
    // signed strides instead of a width.
    map::MapSurface surface;
    surface.pixels = pixels + kMapLeft * kScreenHeight + (kScreenHeight - 1 - kMapTop);
    surface.strideX = kScreenHeight;
    surface.strideZ = -1;

    map::MapStyle style;
    style.unexplored = kUnexplored;
    style.chunkGrid = grid_ == Grid::ChunksAndTiles;
    style.tileGrid = grid_ != Grid::None;
    style.tileGridColour = kTileGrid;

    // **Two steps, and only the second one runs most frames.** Refreshing draws
    // the patches that went stale -- the chunk sampled this frame, the one
    // south of it, or every patch in the window after a pack change -- and the
    // copy is what a redraw costs the rest of the time.
    const u64 before = svcGetSystemTick();
    map::refreshMapWindow(store_, palette_, window, style, stamp_);
    map::renderMapWindow(store_, window, style, surface);

    // The player. **Every other player in a multiplayer session is this call
    // again** with their own position, yaw and colour -- which is why the marker
    // takes a position rather than assuming the centre, even though the window
    // is centred on this one.
    map::drawMarker(surface, window, camera.x, camera.z,
                    map::facingFromYaw(camera.yaw * 180.0f / 3.14159265358979f), 2, kMarkerFill,
                    kMarkerOutline);

    // Timed before the flush rather than after: the flush is a cache
    // maintenance call whose cost belongs to the screen being software-drawn at
    // all, and mixing the two would hide which of them was the problem.
    lastDrawMicros_ = u32(millisFromTicks(svcGetSystemTick() - before) * 1000.0f);

    // The CPU has just written a buffer the LCD reads by DMA.
    gfxFlushBuffers();
    return true;
}

}  // namespace mc::ctr
