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

void drawMarker(const MapSurface& surface, const MapWindow& window, double blockX, double blockZ,
                int facing, int radius, MapPixel fill, MapPixel outline)
{
    // Small on purpose: at one pixel per block a marker any larger stops being
    // an indicator and starts being terrain the player cannot see under.
    constexpr int kMaxRadius = 4;
    if (surface.pixels == nullptr) {
        return;
    }
    if (radius < 1) {
        radius = 1;
    }
    if (radius > kMaxRadius) {
        radius = kMaxRadius;
    }

    // The tick reaches two blocks past the body, and the outline one past
    // whatever that is.
    const int reach = radius + 2;
    const int edge = reach + 1;
    const int span = edge * 2 + 1;
    constexpr int kMaxSpan = (kMaxRadius + 3) * 2 + 1;
    bool body[kMaxSpan * kMaxSpan] = {};

    for (int dz = -reach; dz <= reach; ++dz) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const int absSum = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
            if (absSum <= radius) {
                body[(dz + edge) * span + (dx + edge)] = true;
            }
        }
    }
    if (facing >= 0 && facing < 8) {
        const MapStep step = kFacingStep[facing];
        for (int i = radius; i <= radius + 2; ++i) {
            const int dx = step.x * i;
            const int dz = step.z * i;
            if (dx >= -reach && dx <= reach && dz >= -reach && dz <= reach) {
                body[(dz + edge) * span + (dx + edge)] = true;
            }
        }
    }

    const i32 centreX = i32(std::floor(blockX));
    const i32 centreZ = i32(std::floor(blockZ));

    for (int dz = -edge; dz <= edge; ++dz) {
        for (int dx = -edge; dx <= edge; ++dx) {
            const bool inside = body[(dz + edge) * span + (dx + edge)];

            // The outline is every empty cell touching a filled one, worked out
            // here rather than drawn by hand, so a marker stays legible over
            // any terrain colour without a second sprite per shape.
            bool ring = false;
            if (!inside) {
                for (int ez = -1; ez <= 1 && !ring; ++ez) {
                    for (int ex = -1; ex <= 1 && !ring; ++ex) {
                        const int nz = dz + ez + edge;
                        const int nx = dx + ex + edge;
                        if (nz >= 0 && nz < span && nx >= 0 && nx < span) {
                            ring = body[nz * span + nx];
                        }
                    }
                }
            }
            if (!inside && !ring) {
                continue;
            }

            const int px = int(centreX + dx - window.originBlockX);
            const int pz = int(centreZ + dz - window.originBlockZ);
            if (px < 0 || px >= window.width || pz < 0 || pz >= window.height) {
                continue;
            }
            surface.pixels[px * surface.strideX + pz * surface.strideZ] = inside ? fill : outline;
        }
    }
}

}  // namespace mc::map
