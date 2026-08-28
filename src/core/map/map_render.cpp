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
    *minChunkX = floorDiv(window.originBlockX, kChunkPixels);
    *minChunkZ = floorDiv(window.originBlockZ, kChunkPixels);
    *maxChunkX = floorDiv(window.originBlockX + window.width - 1, kChunkPixels);
    *maxChunkZ = floorDiv(window.originBlockZ + window.height - 1, kChunkPixels);
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

void renderMapWindow(const MapStore& store, const MapWindow& window, const MapStyle& style,
                     const MapSurface& surface)
{
    if (surface.pixels == nullptr || window.width <= 0 || window.height <= 0) {
        return;
    }

    for (int px = 0; px < window.width; ++px) {
        const i32 blockX = window.originBlockX + px;
        const i32 chunkX = floorDiv(blockX, kChunkPixels);
        const int localX = int(blockX - chunkX * kChunkPixels);

        i32 chunkZ = floorDiv(window.originBlockZ, kChunkPixels);
        int localZ = int(window.originBlockZ - chunkZ * kChunkPixels);
        const MapPixel* patch = store.patch(chunkX, chunkZ);

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
                // **The fast path, and the reason a patch is stored z-reversed.**
                // Going south steps back through the framebuffer and forward
                // through the patch, so the run is contiguous and ascending in
                // both: one move of `run` halfwords rather than `run` loads,
                // stores and two pointer bumps. This is the case the console
                // takes on every redraw.
                std::memcpy(out - (run - 1), patch + localX * kChunkPixels + kChunkPixels - localZ - run,
                            usize(run) * sizeof(MapPixel));
                out -= run;
            } else {
                const MapPixel* column = patch + localX * kChunkPixels;
                for (int k = 0; k < run; ++k) {
                    *out = column[kChunkPixels - 1 - (localZ + k)];
                    out += surface.strideZ;
                }
            }

            pz += run;
            localZ = 0;
            patch = store.patch(chunkX, ++chunkZ);
        }
    }
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

    // Where the player is inside the window, in pixels, sub-block part and all:
    // one pixel is one block, so the fraction of a block they have walked into
    // is the fraction of a pixel the arrow moves.
    const float centreX = float(blockX - double(window.originBlockX));
    const float centreZ = float(blockZ - double(window.originBlockZ));

    gui::drawArrow(canvas, centreX, centreZ, angle, shape, fill, outline);
}

}  // namespace mc::map
