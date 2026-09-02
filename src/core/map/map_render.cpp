#include "core/map/map_render.hpp"

#include "core/util/math.hpp"

#include <cmath>
#include <cstring>

namespace mc::map {

namespace {

// Which of the three brightnesses a water pixel gets.
//
// The later arithmetic is `d2 = depth * 0.1 + ((x + z) & 1) * 0.2`, bright
// below 0.5 and dark above 0.9. Everything in it is a tenth, so it is done in
// tenths and the floating point goes away without the answer moving.
int waterShade(int depth, i32 blockX, i32 blockZ)
{
    const int tenths = depth + (((blockX + blockZ) & 1) != 0 ? 2 : 0);
    if (tenths < 5) {
        return 2;
    }
    if (tenths > 9) {
        return 0;
    }
    return 1;
}

// ...and a land pixel: the step to the block one to the north.
//
// The later expression is `(d1 - d0) * 4 / (depth + 4) + ((x & 1) - 0.5) * 0.4`,
// bright above 0.6 and dark below -0.6. At scale 0 `depth` is zero for anything
// that reaches this branch and the heights are whole numbers, so the first term
// is the height step itself and the dither is +-0.2 -- which cannot carry a
// step of 0 past 0.6 nor hold a step of 1 below it. So it is a comparison.
int landShade(int height, int previousHeight)
{
    if (height > previousHeight) {
        return 2;
    }
    return height < previousHeight ? 0 : 1;
}

}  // namespace

const MapStep kFacingStep[8] = {
    {0, 1},    // south, which is where yaw 0 looks
    {-1, 1},   // south-west
    {-1, 0},   // west
    {-1, -1},  // north-west
    {0, -1},   // north
    {1, -1},   // north-east
    {1, 0},    // east
    {1, 1},    // south-east
};

const char* const kCompass[8] = {"S", "SW", "W", "NW", "N", "NE", "E", "SE"};

int facingFromYaw(float yawDegrees)
{
    float turns = yawDegrees / 45.0f;
    turns = std::floor(turns + 0.5f);
    int index = int(std::fmod(turns, 8.0f));
    return index < 0 ? index + 8 : index;
}

void windowChunkRange(const MapWindow& window, i32* minChunkX, i32* minChunkZ, i32* maxChunkX,
                      i32* maxChunkZ)
{
    // In blocks, not pixels: a shrunk window is a small picture of a lot of
    // ground, and it is the ground that decides which chunks are wanted.
    //
    // **Rounded up, unlike `mapWindowBlocks`.** A magnified window whose pixel
    // width is not a whole number of blocks still draws that last partial
    // block, so the chunk it is in is one the caller has to know about --
    // reporting the floor would leave a strip of the picture sampled by nobody.
    // The console's own 208 by 200 divides exactly at every level; this is for
    // every other caller.
    const int pixelsPerBlock = mapPixelsPerBlock(window.zoom);
    const int blocksPerPixel = mapBlocksPerPixel(window.zoom);
    const i32 blocksWide = (i32(window.width) * blocksPerPixel + pixelsPerBlock - 1) / pixelsPerBlock;
    const i32 blocksHigh =
        (i32(window.height) * blocksPerPixel + pixelsPerBlock - 1) / pixelsPerBlock;

    *minChunkX = floorDiv(window.originBlockX, kChunkPixels);
    *minChunkZ = floorDiv(window.originBlockZ, kChunkPixels);
    *maxChunkX = floorDiv(window.originBlockX + blocksWide - 1, kChunkPixels);
    *maxChunkZ = floorDiv(window.originBlockZ + blocksHigh - 1, kChunkPixels);
}

void renderMapChunk(const MapChunkSample& sample, const MapChunkSample* north, i32 chunkX,
                    i32 chunkZ, const MapPalette& palette, const MapStyle& style, MapPixel* patch)
{
    const i32 originBlockX = chunkX * kChunkPixels;
    const i32 originBlockZ = chunkZ * kChunkPixels;

    // Where this chunk sits inside a later-version map tile. A tile is 128
    // blocks and a chunk is 16, so a chunk is either entirely inside a tile or
    // starts one -- the tile line, when there is one, is always this chunk's
    // own first row or column.
    const bool tileEdgeColumn = isTileEdgeBlock(originBlockX);
    const bool tileEdgeRow = isTileEdgeBlock(originBlockZ);

    for (int localX = 0; localX < kChunkPixels; ++localX) {
        const i32 blockX = originBlockX + localX;
        const int columnBase = localX * kChunkPixels;

        // The row immediately north, which is the chunk above's last row. See
        // the note on `north` in the header for why absent is not zero.
        int previousHeight = 0;
        bool previousKnown = false;
        if (north != nullptr) {
            previousHeight = int(north->height[columnBase + kChunkPixels - 1]);
            previousKnown = true;
        }

        for (int localZ = 0; localZ < kChunkPixels; ++localZ) {
            const int i = columnBase + localZ;
            const block::BlockId id = sample.surface[i];
            const int height = int(sample.height[i]);

            MapPixel colour = style.unexplored;
            if (id < kPaletteSize && palette.known[id]) {
                const int shade = palette.water[id]
                                      ? waterShade(int(sample.depth[i]), blockX,
                                                   originBlockZ + localZ)
                                      : (previousKnown ? landShade(height, previousHeight) : 1);
                colour = palette.shaded[shade][id];
                if (style.chunkGrid && (localX == 0 || localZ == 0)) {
                    colour = palette.shaded[0][id];
                }
            }
            if (style.tileGrid && ((tileEdgeColumn && localX == 0) || (tileEdgeRow && localZ == 0))) {
                colour = style.tileGridColour;
            }

            patch[patchIndex(localX, localZ)] = colour;
            previousHeight = height;
            previousKnown = true;
        }
    }
}

int refreshMapWindow(MapStore& store, const MapPalette& palette, const MapWindow& window,
                     const MapStyle& style, u32 stamp)
{
    i32 minChunkX = 0;
    i32 minChunkZ = 0;
    i32 maxChunkX = 0;
    i32 maxChunkZ = 0;
    windowChunkRange(window, &minChunkX, &minChunkZ, &maxChunkX, &maxChunkZ);

    int drawn = 0;
    for (i32 chunkZ = minChunkZ; chunkZ <= maxChunkZ; ++chunkZ) {
        for (i32 chunkX = minChunkX; chunkX <= maxChunkX; ++chunkX) {
            if (!store.patchStale(chunkX, chunkZ, stamp)) {
                continue;
            }
            const MapChunkSample* sample = store.find(chunkX, chunkZ);
            if (sample == nullptr) {
                continue;  // patchStale already said no, but be explicit
            }
            // `claimPatch` only stamps an entry and hands back a pointer into
            // it -- it never inserts and never evicts -- so these two stay
            // valid across it. That is the whole reason the patch lives in the
            // same entry as the sample rather than in a table of its own.
            const MapChunkSample* north = store.find(chunkX, chunkZ - 1);
            MapPixel* patch = store.claimPatch(chunkX, chunkZ, stamp);
            if (patch == nullptr) {
                continue;
            }
            renderMapChunk(*sample, north, chunkX, chunkZ, palette, style, patch);
            ++drawn;
        }
    }
    return drawn;
}

namespace {

// The patches under one chunk column of the window, gathered once.
//
// **This is a cache-miss budget rather than a tidiness argument.** Every walk
// below asks the store for a patch each time it crosses a chunk edge going
// south, and the answer depends only on the chunk -- so at 1:1 a redraw was
// doing 208 x 13 = 2,704 hash lookups to obtain 182 distinct answers, and
// shrunk it was 5,408 to obtain 702. Each one is a probe into an index in front
// of 2.25 MB of entries, which is 1.5 times an old 3DS's whole L2 and has no
// chance of staying resident. Sixteen output columns share a chunk column at
// 1:1 -- eight when shrunk, 32 or 64 magnified -- so gathering per chunk column
// takes it to exactly one lookup per chunk in the window.
//
// 64 rows is 256 bytes of stack against the 3DS build's 8 KB per-frame ceiling.
// The console's window is 200 pixels, which is 26 chunk rows at the widest zoom
// and 13 at 1:1, so this is four times what `kZoomMin` can ask for.
constexpr int kMaxWindowChunkRows = 64;

struct PatchColumn {
    const MapPixel* rows[kMaxWindowChunkRows];
    i32 firstChunkZ = 0;
    int count = 0;

    // Out of range reads as "never sampled", which is what a window taller than
    // the array would otherwise have to be refused for. It cannot happen at any
    // zoom that exists; it is here so that widening `kZoomMin` later is a map
    // with an unexplored strip and not a read off the end of a stack array.
    const MapPixel* at(i32 chunkZ) const
    {
        const i32 index = chunkZ - firstChunkZ;
        return index >= 0 && index < count ? rows[index] : nullptr;
    }
};

void gatherPatchColumn(const MapStore& store, i32 chunkX, i32 firstChunkZ, int count,
                       PatchColumn* out)
{
    out->firstChunkZ = firstChunkZ;
    out->count = count < kMaxWindowChunkRows ? count : kMaxWindowChunkRows;
    for (int i = 0; i < out->count; ++i) {
        out->rows[i] = store.patch(chunkX, firstChunkZ + i);
    }
}

// How many chunk rows one column of this window spans.
int windowChunkRows(const MapWindow& window, i32* firstChunkZ)
{
    const int pixelsPerBlock = mapPixelsPerBlock(window.zoom);
    const int blocksPerPixel = mapBlocksPerPixel(window.zoom);
    const i32 blocksHigh =
        (i32(window.height) * blocksPerPixel + pixelsPerBlock - 1) / pixelsPerBlock;
    *firstChunkZ = floorDiv(window.originBlockZ, kChunkPixels);
    const i32 lastChunkZ = floorDiv(window.originBlockZ + blocksHigh - 1, kChunkPixels);
    return int(lastChunkZ - *firstChunkZ) + 1;
}

// **The 1:1 path, and the reason a patch is stored z-reversed.** One `memcpy`
// per chunk column, which is what the 700-microsecond redraw was measured on.
// Everything zoomed goes through one of the two below; this is the case the
// console is in whenever the player has not asked for anything else.
void renderUnscaled(const MapStore& store, const MapWindow& window, const MapStyle& style,
                    const MapSurface& surface)
{
    i32 firstChunkZ = 0;
    const int chunkRows = windowChunkRows(window, &firstChunkZ);

    PatchColumn column;
    i32 gatheredChunkX = 0;
    bool gathered = false;

    for (int px = 0; px < window.width; ++px) {
        const i32 blockX = window.originBlockX + px;
        const i32 chunkX = floorDiv(blockX, kChunkPixels);
        const int localX = int(blockX - chunkX * kChunkPixels);
        if (!gathered || chunkX != gatheredChunkX) {
            gatherPatchColumn(store, chunkX, firstChunkZ, chunkRows, &column);
            gatheredChunkX = chunkX;
            gathered = true;
        }

        i32 chunkZ = floorDiv(window.originBlockZ, kChunkPixels);
        int localZ = int(window.originBlockZ - chunkZ * kChunkPixels);
        const MapPixel* patch = column.at(chunkZ);

        MapPixel* out = surface.pixels + px * surface.strideX;
        int pz = 0;
        while (pz < window.height) {
            // As far as this chunk goes, or as far as the window does.
            int run = kChunkPixels - localZ;
            if (run > window.height - pz) {
                run = window.height - pz;
            }

            if (patch == nullptr) {
                for (int k = 0; k < run; ++k) {
                    *out = style.unexplored;
                    out += surface.strideZ;
                }
            } else if (surface.strideZ == -1) {
                // Going south steps back through the framebuffer and forward
                // through the patch, so the run is contiguous and ascending in
                // both: one move of `run` halfwords rather than `run` loads,
                // stores and two pointer bumps.
                std::memcpy(out - (run - 1), patch + localX * kChunkPixels + kChunkPixels - localZ - run,
                            usize(run) * sizeof(MapPixel));
                out -= run;
            } else {
                const MapPixel* source = patch + localX * kChunkPixels + kChunkPixels - 1 - localZ;
                for (int k = 0; k < run; ++k) {
                    *out = *source--;
                    out += surface.strideZ;
                }
            }

            pz += run;
            localZ = 0;
            patch = column.at(++chunkZ);
        }
    }
}

// Magnified: one block becomes `pixelsPerBlock` pixels on each axis.
//
// **One output column in `pixelsPerBlock` is built; the rest are copies of it.**
// The columns beside it are the same block repeated, so they are a `memcpy` of
// the one just built rather than a second walk down the patches -- which is why
// +2 measures *cheaper* than 1:1 rather than four times dearer: it reads a
// quarter of the source columns 1:1 reads, and three quarters of what it writes
// is a straight block move.
void renderMagnified(const MapStore& store, const MapWindow& window, const MapStyle& style,
                     const MapSurface& surface)
{
    const int pixelsPerBlock = mapPixelsPerBlock(window.zoom);

    i32 firstChunkZ = 0;
    const int chunkRows = windowChunkRows(window, &firstChunkZ);

    PatchColumn column;
    i32 gatheredChunkX = 0;
    bool gathered = false;

    for (int px = 0; px < window.width; px += pixelsPerBlock) {
        const i32 blockX = window.originBlockX + i32(px / pixelsPerBlock);
        const i32 chunkX = floorDiv(blockX, kChunkPixels);
        const int localX = int(blockX - chunkX * kChunkPixels);
        if (!gathered || chunkX != gatheredChunkX) {
            gatherPatchColumn(store, chunkX, firstChunkZ, chunkRows, &column);
            gatheredChunkX = chunkX;
            gathered = true;
        }

        i32 chunkZ = floorDiv(window.originBlockZ, kChunkPixels);
        int localZ = int(window.originBlockZ - chunkZ * kChunkPixels);
        const MapPixel* patch = column.at(chunkZ);

        MapPixel* const columnStart = surface.pixels + px * surface.strideX;
        MapPixel* out = columnStart;

        int pz = 0;
        while (pz < window.height) {
            // The blocks this chunk still owes, and the pixels they become.
            const int blocks = kChunkPixels - localZ;
            // Descending through the patch is going south; see map_store.hpp.
            const MapPixel* source =
                patch != nullptr ? patch + localX * kChunkPixels + kChunkPixels - 1 - localZ
                                 : nullptr;
            for (int b = 0; b < blocks && pz < window.height; ++b) {
                const MapPixel colour = source != nullptr ? *source-- : style.unexplored;
                for (int j = 0; j < pixelsPerBlock && pz < window.height; ++j) {
                    *out = colour;
                    out += surface.strideZ;
                    ++pz;
                }
            }
            localZ = 0;
            patch = column.at(++chunkZ);
        }

        for (int k = 1; k < pixelsPerBlock && px + k < window.width; ++k) {
            MapPixel* copy = columnStart + k * surface.strideX;
            if (surface.strideZ == -1) {
                std::memcpy(copy - (window.height - 1), columnStart - (window.height - 1),
                            usize(window.height) * sizeof(MapPixel));
                continue;
            }
            const MapPixel* source = columnStart;
            for (int pz2 = 0; pz2 < window.height; ++pz2) {
                *copy = *source;
                copy += surface.strideZ;
                source += surface.strideZ;
            }
        }
    }
}

// Shrunk: one pixel is `blocksPerPixel` blocks, point-sampled.
//
// **The one path with no block move in it anywhere**, and therefore the
// expensive one -- the source is strided and the destination is not, so every
// pixel is a load and a store however it is written. What it can avoid is doing
// arithmetic per pixel: within a chunk the source walks backwards by a fixed
// step, so the run below is a pointer decrement rather than a `patchIndex`
// multiply, and the chunk it is walking is an array index rather than a hash
// lookup.
void renderShrunk(const MapStore& store, const MapWindow& window, const MapStyle& style,
                  const MapSurface& surface)
{
    const int blocksPerPixel = mapBlocksPerPixel(window.zoom);

    i32 firstChunkZ = 0;
    const int chunkRows = windowChunkRows(window, &firstChunkZ);

    PatchColumn column;
    i32 gatheredChunkX = 0;
    bool gathered = false;

    for (int px = 0; px < window.width; ++px) {
        const i32 blockX = window.originBlockX + i32(px) * blocksPerPixel;
        const i32 chunkX = floorDiv(blockX, kChunkPixels);
        const int localX = int(blockX - chunkX * kChunkPixels);
        if (!gathered || chunkX != gatheredChunkX) {
            gatherPatchColumn(store, chunkX, firstChunkZ, chunkRows, &column);
            gatheredChunkX = chunkX;
            gathered = true;
        }

        i32 chunkZ = floorDiv(window.originBlockZ, kChunkPixels);
        int localZ = int(window.originBlockZ - chunkZ * kChunkPixels);
        const MapPixel* patch = column.at(chunkZ);

        MapPixel* out = surface.pixels + px * surface.strideX;
        int pz = 0;
        while (pz < window.height) {
            // Samples this chunk still has for this column, at this step.
            int run = (kChunkPixels - localZ + blocksPerPixel - 1) / blocksPerPixel;
            if (run > window.height - pz) {
                run = window.height - pz;
            }

            if (patch == nullptr) {
                for (int k = 0; k < run; ++k) {
                    *out = style.unexplored;
                    out += surface.strideZ;
                }
            } else {
                const MapPixel* source = patch + localX * kChunkPixels + kChunkPixels - 1 - localZ;
                for (int k = 0; k < run; ++k) {
                    *out = *source;
                    source -= blocksPerPixel;
                    out += surface.strideZ;
                }
            }

            pz += run;
            // Where the next chunk's first sample lands. The lattice is
            // absolute -- the window origin is a multiple of the step -- so
            // this is always 0, and it is computed rather than assumed so that
            // a caller which does not snap its origin still samples evenly
            // instead of shifting the phase at every chunk edge.
            localZ = (localZ + run * blocksPerPixel) - kChunkPixels;
            patch = column.at(++chunkZ);
        }
    }
}

}  // namespace

void renderMapWindow(const MapStore& store, const MapWindow& window, const MapStyle& style,
                     const MapSurface& surface)
{
    if (surface.pixels == nullptr || window.width <= 0 || window.height <= 0) {
        return;
    }

    // **Three functions rather than one general walk, and the split is where
    // the inner loop genuinely differs.** 1:1 is a block move; magnified is a
    // block move plus a source read every `pixelsPerBlock` pixels; shrunk is a
    // strided gather with no move available to it at all. One loop covering all
    // three would carry two counters and a branch per pixel to serve a case
    // that never needs them, on the redraw path of a 268 MHz console.
    if (window.zoom == 0) {
        renderUnscaled(store, window, style, surface);
        return;
    }
    if (window.zoom > 0) {
        renderMagnified(store, window, style, surface);
        return;
    }
    renderShrunk(store, window, style, surface);
}

int yawStep(float yawDegrees)
{
    const float perStep = 360.0f / float(kYawSteps);
    float steps = std::floor(yawDegrees / perStep + 0.5f);
    int index = int(std::fmod(steps, float(kYawSteps)));
    return index < 0 ? index + kYawSteps : index;
}

float yawFromStep(int step)
{
    return float(step) * (360.0f / float(kYawSteps));
}

void drawMarker(const MapSurface& surface, const MapWindow& window, double blockX, double blockZ,
                float yawDegrees, float length, MapPixel fill, MapPixel outline)
{
    if (surface.pixels == nullptr || window.width <= 0 || window.height <= 0) {
        return;
    }

    // The window's own pixels, as a thing that can be drawn on. The map's
    // surface carries strides and no bounds, because the window is what says
    // how big it is; `gui::Surface` wants both, and clips against them.
    gui::Surface canvas;
    canvas.pixels = surface.pixels;
    canvas.strideX = surface.strideX;
    canvas.strideY = surface.strideZ;
    canvas.width = window.width;
    canvas.height = window.height;

    // The arrow's own proportions, at whatever size the caller asked for. Every
    // part scales together, so a marker drawn at half the size is the same
    // shape and not a different one.
    gui::ArrowShape shape;
    const float scale = length / shape.length;
    shape.length = length;
    shape.halfWidth *= scale;
    shape.tail *= scale;
    shape.notch *= scale;

    // **Minecraft's yaw against the surface's angle.** Yaw 0 looks along +Z,
    // which is south and therefore *down* the map, and it increases towards -X,
    // which is west and therefore left. `gui::drawArrow` measures clockwise
    // from straight up, so south is half a turn away and the two rotate the
    // same way -- which makes the conversion an offset and not a reflection.
    constexpr float kPi = 3.14159265358979f;
    const float angle = (yawDegrees + 180.0f) * kPi / 180.0f;

    // Where the player is inside the window, in pixels, sub-block part and all.
    // At 1:1 one pixel is one block, so the fraction of a block they have walked
    // into is the fraction of a pixel the arrow moves; zoomed, the same fraction
    // is scaled with everything else, which is what makes a magnified map move
    // smoothly rather than in whole-block steps.
    const float scaleToPixels =
        float(mapPixelsPerBlock(window.zoom)) / float(mapBlocksPerPixel(window.zoom));
    const float centreX = float(blockX - double(window.originBlockX)) * scaleToPixels;
    const float centreZ = float(blockZ - double(window.originBlockZ)) * scaleToPixels;

    gui::drawArrow(canvas, centreX, centreZ, angle, shape, fill, outline);
}

}  // namespace mc::map
