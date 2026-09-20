#pragma once

// The map's memory: every chunk that has been sampled, and the patch of pixels
// it was last drawn as.
//
// **This is what "the map saves its render" means here, and it is now two
// things rather than one, because a console said so.** Sampling a chunk is 256
// downward scans, so the sample is kept and a chunk is scanned once and then
// only when the world under it is written into -- see `store`'s `serial`. That
// was the whole of it, and it left the *shading* to be redone for every pixel
// of every redraw -- which a New 3DS measured at **5,000 microseconds for one
// 192 x 192 window**, a third of a frame, on every block the player crosses.
//
// So the drawn pixels are kept too, sixteen by sixteen per chunk, and a redraw
// is a copy. Everything a patch depends on is either inside the chunk or
// knowable from its coordinates -- its own heights, the row of heights
// immediately north of it, the palette and the grid style -- so a patch can be
// drawn once and reused until one of those changes. What changes them is
// enumerated on `stamp` below, and there is nothing else.
//
// It is also what makes the map remember: ground that has scrolled off the
// screen, and ground the streamer has since given back to the card, are both
// still here and still drawn.
//
// **It is memory and not a file, and that is a decision rather than an
// omission.** Two things stand between this and a map on the SD card, and both
// are real:
//
//   * **The card is not allowed on the render thread** (CONTRIBUTING.md), so
//     tiles would have to be written by the I/O thread or at the two moments a
//     world already blocks -- open and close. That is a design step of its own.
//   * **A packed world would swallow them.** The converter stashes every file
//     it does not recognise into the container, at its own path, so a map
//     directory would vanish from plain view the first time a world was packed
//     unless it were given the same "carried as well as stashed" treatment
//     `alpha.ini` has. Getting that wrong is data loss on a conversion, which
//     is the one class of bug this project does not trade for a feature.
//
// So the map lives for as long as the world is open. A chunk costs 1,536 bytes
// -- a kilobyte of sample and 512 bytes of pixels -- so the sizes the console
// picks are 768 KB and 1.9 MB, which is 360 and 570 blocks square of remembered
// ground.
//
// **The grid is later versions' own.** A map at `scale = 0` covers the 128
// blocks starting at `j * 128 - 64` -- `MapData`'s centre arithmetic -- so a
// tile is exactly 8 by 8 chunks and a tile edge is always a chunk edge. Nothing
// in here is stored per tile; the helpers exist because the screen draws those
// boundaries, and because "aligned with the maps of later versions" is a claim
// that should be checkable rather than asserted.

#include "core/map/map_palette.hpp"
#include "core/map/map_sample.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::map {

// Blocks across one later-version map at scale 0, and the chunks that is.
inline constexpr int kTileBlocks = 128;
inline constexpr int kTileChunks = kTileBlocks / kChunkPixels;

// Which tile a block column falls in, and where that tile starts. The `+64`
// is `MathHelper.floor((x + 64) / 128)` out of `ItemMap.getMapData`, and the
// `-64` puts the tile's first block back where that centre implies.
i32 tileOfBlock(i32 blockX);
i32 tileOriginBlock(i32 tile);

// True when this block column is the first one inside a tile, which is what the
// screen draws a heavier grid line on.
bool isTileEdgeBlock(i32 blockX);

// One chunk's drawn pixels, **stored x-major with z running backwards** --
// `patch[x * 16 + (15 - z)]`.
//
// The reversal is not an accident and it is not a platform detail leaking in:
// it is what turns the one blit that matters into a `memcpy`. The console's
// bottom screen is stored in columns from the bottom up, so a step south is a
// step *back* through memory -- a z stride of -1. Written this way round, a
// chunk's sixteen pixels and the sixteen framebuffer halfwords they land on run
// the same direction, and a chunk column of the map is 32 bytes moved rather
// than sixteen loads, stores and pointer bumps. `renderMapWindow` takes that
// path when the surface's z stride is exactly -1 and walks pixel by pixel
// otherwise, so a host test reading a row-major buffer gets the same picture.
struct MapChunkPatch {
    MapPixel pixels[kChunkSamples] = {};
};

inline int patchIndex(int localX, int localZ)
{
    return localX * kChunkPixels + (kChunkPixels - 1 - localZ);
}

class MapStore {
public:
    struct Stats {
        int chunks = 0;      // held right now
        usize bytes = 0;     // what they cost
        u32 stored = 0;      // stores that changed the picture; see `store`
        u32 evicted = 0;     // ...and chunks pushed out to make room
        u32 drawn = 0;       // patches drawn, which is the cost a redraw avoids
    };

    // How many chunks may be held. Allocates the whole arena once, here, so
    // nothing in the frame path ever asks the heap for anything. Called again
    // with a different number throws the contents away rather than trying to
    // move them: it happens at world open and nowhere else.
    void setCapacity(int chunks);
    int capacity() const { return capacity_; }

    // Null when this chunk has never been sampled. **Does not count as use** --
    // it is const, and the window render calls it for every chunk it touches,
    // so making it move an LRU cursor would mean a redraw could evict what the
    // next redraw is about to ask for. `touch` below is what marks use.
    const MapChunkSample* find(i32 chunkX, i32 chunkZ) const;

    // Stores or replaces one chunk's sample. Replacing matters: a chunk the
    // player has built in gets sampled again and must overwrite rather than
    // accumulate.
    //
    // `serial` is whatever the caller uses to decide that a held sample has
    // gone out of date -- this keeps it and hands it back from `sampleSerial`
    // and asks nothing about it. For the console that is
    // `WorldStreamer::columnBlockSerial`.
    //
    // **True when the picture actually changed**, which a re-sample often does
    // not: a player mining a tunnel rewrites the blocks under a chunk dozens of
    // times a second and none of it reaches the surface the map draws. An
    // identical sample keeps the serial and the drawn patch and advances
    // nothing, so the redraw the screen hangs off `stats().stored` happens when
    // there is something new to see and not merely when something was dug. The
    // comparison is 768 bytes against a re-shade of the window; it is the
    // cheaper half by three orders of magnitude.
    bool store(i32 chunkX, i32 chunkZ, const MapChunkSample& sample, u32 serial = 0);

    // What `serial` this chunk was stored with, or 0 when it is not held.
    u32 sampleSerial(i32 chunkX, i32 chunkZ) const;

    // Marks a chunk as still wanted, so eviction takes something else first.
    // The screen calls it for the chunks under the visible window.
    void touch(i32 chunkX, i32 chunkZ);

    // ------------------------------------------------------------------
    // The drawn pixels.
    //
    // **`stamp` is everything a patch depends on that is not the chunk
    // itself**: the palette and the grid style. The screen bumps it when the
    // texture pack changes or the grid is toggled, and every patch is then
    // stale and redrawn on the next pass over the window. `store()` sets a
    // chunk's stamp to 0 -- which no live stamp ever is -- so a resampled
    // chunk redraws, **and it does the same to the chunk immediately south**,
    // whose first row shades against this one's last.
    // ------------------------------------------------------------------

    // What this chunk was last drawn as, or null when it has never been
    // sampled. Always current by the time anything blits it: `refreshMapWindow`
    // runs first and leaves nothing in the window stale.
    const MapPixel* patch(i32 chunkX, i32 chunkZ) const;

    // True when the chunk is stored and its patch was not drawn with `stamp`.
    // False for a chunk that is not stored at all -- there is nothing to draw.
    bool patchStale(i32 chunkX, i32 chunkZ, u32 stamp) const;

    // Where to draw it, marking it drawn with `stamp`. Null when the chunk is
    // not stored.
    MapPixel* claimPatch(i32 chunkX, i32 chunkZ, u32 stamp);

    void clear();

    const Stats& stats() const { return stats_; }

private:
    struct Entry {
        i32 x = 0;
        i32 z = 0;
        u32 used = 0;   // the tick it was last stored or touched at
        u32 stamp = 0;  // what `patch` was drawn with; 0 is "not drawn"
        u32 serial = 0; // the caller's token for the world this was sampled from
        MapChunkSample sample;
        MapChunkPatch patch;
    };

    const Entry* entryAt(i32 chunkX, i32 chunkZ) const;

    // Marks the chunk south of this one as needing to be drawn again.
    void invalidateSouthOf(i32 chunkX, i32 chunkZ);

    // Open addressing with linear probing over a power-of-two table twice the
    // capacity, so the load factor never passes a half and a probe is short.
    // The index holds positions in `entries_`, or -1.
    //
    // **Eviction rebuilds the index rather than deleting from it.** Deleting
    // from a linear-probe table means shifting a run back, which is easy to get
    // subtly wrong, and a rebuild is one pass over at most a few thousand
    // slots against an event that happens at most twice a frame and only once
    // the store is full. The cost is named here so it is a choice rather than
    // something to discover later.
    int slotOf(i32 chunkX, i32 chunkZ) const;
    void rebuildIndex();

    std::vector<Entry> entries_;
    std::vector<i32> index_;

    // **The cap, kept apart from `entries_.capacity()`.** `reserve` is allowed
    // to give more room than it was asked for, and the index table is sized
    // against the number asked for -- so trusting the vector would let the
    // table pass half full and, at the limit, let a probe for a free slot never
    // find one.
    int capacity_ = 0;
    u32 tick_ = 0;
    Stats stats_;
};

}  // namespace mc::map
