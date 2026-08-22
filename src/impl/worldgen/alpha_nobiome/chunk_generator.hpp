#pragma once

// `ft` -- ChunkProviderLoadOrGenerate -- the driver that turns "there is no
// chunk here" into a finished column, and the last piece of M4.
//
// Everything under it was already transcribed and verified: terrain, the
// surface pass, caves, the nine population generators and the driver that
// orders them, plus the light engine. **Nothing called any of it.** This is
// what calls it, and it is a transcription in its own right rather than glue,
// because the original's `ft.b(int,int)` decides three things that are part of
// the world:
//
//   1. whether a chunk is read from the save or generated,
//   2. **when** a chunk is populated -- once a 2x2 quadrant containing it is
//      resident, checked four ways per call, and
//   3. that population happens exactly once per chunk, guarded by the
//      `TerrainPopulated` flag which is set *before* the pass runs.
//
// ---------------------------------------------------------------------------
// **A seed does not determine an Alpha world, and that is the original's
// property, not ours.**
//
// This is worth stating plainly because it looks like a fidelity bug and is
// not. `ft.b` populates a chunk when its quadrant happens to be resident, and
// residency is a consequence of where the player walked and of a 1024-entry
// cache that evicts on collision. Two a1.1.2 clients given the same seed and
// different routes populate in different orders -- and population reads the
// world it writes into, so where two chunks' generators reach the same blocks
// the result differs. Terrain, the surface pass and caves are pure functions of
// the seed; population's *order* is not.
//
// So "seed-exact" means what it has always meant here: the same rule, applied
// to our own load order. The order this class produces is one an a1.1.2 client
// also produces -- see the note on the sweep in provide() -- rather than an
// invention.
//
// ---------------------------------------------------------------------------
// **What has to exist before a column is finished.** Three gates, each measured
// or derived rather than guessed:
//
//   * **Population of (px, pz) reads and writes a 3x3 of columns centred on
//     it.** The 5x5 the oracle fixture uses is not needed: running every
//     fixture case both ways gives identical blocks, which
//     `tests/populate_test.cpp` pins. Writes land only in the 2x2 quadrant
//     (px..px+1, pz..pz+1) -- measured across the fixture, `changedColumns==4`.
//   * **A column is final once the four populations that can write into it have
//     run**: (px, pz) for px in {cx-1, cx} and pz in {cz-1, cz}.
//   * **Lighting the centre needs the 3x3 around it final**, because a tree or
//     a lava spring placed by a later population changes light inside the
//     centre. The original never has to make this call: it relights lazily from
//     a queue, so it converges to the same answer after the fact. We compute the
//     fixed point once, so the blocks have to be settled first. See
//     core/world/lighting.hpp.
//
// Closing that over the 6x6 of terrain it implies is what provide() does.
//
// ---------------------------------------------------------------------------
// **Memory.** The generator holds raw 32,768-byte columns -- Alpha's own
// `x << 11 | z << 7 | y` array, the form every generator writes through -- in a
// fixed-capacity cache. They are not ChunkColumns: a column being populated is
// still being written to by its neighbours, so palette-compressing it would
// mean recompressing it several times over. One is 32 KB against a
// palette-compressed column's measured 18 KB, so the cache is the second
// largest thing the chunk worker owns after the block data itself.
//
// `ChunkProvider` is another ~300 KB and the `LightEngine` 576 KB, both once.
// **Heap-allocate a ChunkGenerator; never make one a local.** A 3DSX's main
// thread has 32 KB of stack in total.

#include "core/util/types.hpp"
#include "core/world/lighting.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

#include <memory>
#include <vector>

namespace mc::world {
class ChunkColumn;
}

namespace mc::worldgen {

class ChunkGenerator {
public:
    // How the generator reaches the world around it: what is already there, and
    // where to put what it makes.
    //
    // Callbacks rather than the storage slot itself, and deliberately: the
    // generator would otherwise have to include version_slots.hpp, which
    // includes the worldgen slot, which would include this. It also lets the
    // tests drive it over a world that is not on a disk at all.
    struct Store {
        void* context = nullptr;

        // The chunk already in the world at (chunkX, chunkZ), or null when
        // there is none -- which is the case that makes this class generate
        // one.
        //
        // A ChunkColumn rather than a raw block array, so Alpha's
        // `x << 11 | z << 7 | y` layout stays inside this slot instead of
        // reaching whoever holds the world. The generator flattens it.
        //
        // `scratch` is the generator's own column, offered so an implementation
        // that has to read from a disk has somewhere to put it; one that
        // already holds the column in memory returns a pointer to that instead
        // and copies nothing. **The result is read immediately and not kept**,
        // so returning a pointer into a live grid is the intended use.
        //
        // `terrainPopulated` is read straight off it: it is the stored flag,
        // and it decides whether the column still owes its population passes.
        const world::ChunkColumn* (*load)(void* context, i32 chunkX, i32 chunkZ,
                                          world::ChunkColumn* scratch) = nullptr;

        // **A column the generator finished that nobody asked for.**
        //
        // One provide() sweeps a 6x6 and finishes several columns on the way.
        // Without somewhere to put them they stay in the cache waiting to be
        // requested, and since a request only ever moves outward, that is a
        // band that never shrinks: measured at 116 columns for a radius of only
        // 4, against 98 with this connected. The rest of the band is columns
        // that are genuinely not finished yet, which nothing can shorten.
        //
        // Nothing is wasted: every column here is inside the sweep of a request
        // that has already been made, so it was going to be asked for shortly.
        //
        // **Consume it synchronously.** The column is the generator's own and
        // is reused for the next one, so a caller that wants to keep it must
        // move out of it or copy what it needs before returning.
        void (*deliver)(void* context, world::ChunkColumn& column) = nullptr;
    };

    struct Stats {
        u32 generated = 0;   // columns whose terrain, surface and caves this ran
        u32 loaded = 0;      // columns taken from the existing world instead
        u32 populated = 0;   // population passes run
        u32 lit = 0;         // columns handed out
        u32 cacheHits = 0;
        u32 cacheMisses = 0;
        u32 evicted = 0;

        // The high-water mark of columns held that have not been handed over.
        // This is the number cacheColumnsFor() is fitted to; re-measure it with
        // `--generate` before changing that formula.
        u32 peakLive = 0;

        // Three counters that must all stay zero. Each is a distinct way for
        // the design above to be wrong, and each is silent otherwise.
        u32 refusedOutOfWindow = 0;  // a population wrote outside its 3x3
        u32 populationEscapes = 0;   // ...or outside its own 2x2 quadrant
        u32 evictedLive = 0;         // the cache dropped a column still in use
        u32 scratchColumns = 0;      // window columns generated outside the cache
        u32 scratchHits = 0;

        // Dungeon chests and spawners population produced. They are counted and
        // dropped: the blocks are placed, the contents are not, because chunk
        // tile entities round-trip as an opaque blob and have never been
        // parsed. See docs/status.md.
        u32 droppedChests = 0;
        u32 droppedSpawners = 0;
    };

    // **How large the cache has to be, from the measurement rather than from
    // the geometry.**
    //
    // A column cannot be handed over until it is final and its 3x3 is final, so
    // the generator is always holding two rings of frontier that the sweep has
    // reached and the passes have not finished with. That band's length grows
    // with the radius being generated, which is the one number here that is not
    // a constant. Measured peak, columns held and not yet handed over, filling
    // a fresh world nearest-first:
    //
    //     radius  4 -> 98      radius 8 -> 162      radius 11 -> 210
    //
    // which is close enough to linear that 16 per ring plus the 6x6 sweep and
    // headroom covers it. **Undersizing is not a slow path, it is a wrong
    // world**: the cache refuses to evict a column still being written into,
    // and when it has no choice it takes one anyway and counts `evictedLive`,
    // after which that column comes back as bare terrain with its neighbours'
    // populations missing. Watch that counter, not the hit rate.
    //
    // 32,768 bytes a column, so radius 11 is 8.4 MB -- the second largest thing
    // the chunk worker owns. It is only needed while a world is being made:
    // once the columns are on the SD card the streamer reads them and this
    // never runs.
    static constexpr int cacheColumnsFor(int loadRadius)
    {
        return 16 * (loadRadius < 0 ? 0 : loadRadius) + 96;
    }

    // Enough for one sweep with room to reuse, which is what a test or a probe
    // wants. A streamer must pass cacheColumnsFor(its own radius).
    static constexpr int kDefaultCacheColumns = 160;

    ChunkGenerator(i64 seed, GeneratorOptions options, Store store,
                   int cacheColumns = kDefaultCacheColumns);
    ~ChunkGenerator();

    ChunkGenerator(const ChunkGenerator&) = delete;
    ChunkGenerator& operator=(const ChunkGenerator&) = delete;

    // The finished column at (chunkX, chunkZ): generated or loaded, populated,
    // lit, and written into `out` with its height map and both light planes.
    //
    // **The caller must persist what it gets before asking for the same chunk
    // again.** The generator evicts columns it has handed out, and re-reaching
    // one goes through Existing::load; a chunk that was never saved would come
    // back as bare terrain with every neighbour's population missing. The
    // original has the same contract and meets it by saving on eviction.
    bool provide(i32 chunkX, i32 chunkZ, world::ChunkColumn* out);

    // **Grows the cache, and only grows it.**
    //
    // The render distance is a live setting, and what the generator has to hold
    // scales with it. Growing is safe at any moment: an entry names its blocks
    // by slot index rather than by pointer, so the pool can move underneath it.
    // Shrinking is not offered, because the only way to shrink is to throw away
    // columns that are still being written into -- which is the one thing this
    // class must never do. A session that has been to distance 20 and back
    // keeps the larger cache; that is the cost of not corrupting the world.
    void growCacheTo(int cacheColumns);

    // Hands over everything already finished, so the cache can let go of it.
    // provide() calls this itself; it is public because a caller that has
    // stopped requesting columns -- the player standing still -- still wants
    // the last sweep's work put away rather than held.
    void flush();

    const Stats& stats() const { return stats_; }

    // The seed and options the columns are being generated from, for the debug
    // page and for whoever writes level.dat.
    i64 seed() const { return provider_.seed(); }

private:
    // A raw column in the cache, in whatever state it has reached.
    struct Entry {
        i32 x = 0;
        i32 z = 0;
        bool used = false;

        // `ga.n`, isTerrainPopulated: whether *this chunk's* population pass
        // has run. Exactly the flag `ft`'s trigger tests.
        bool populated = false;

        // Which of the four populations that can write into this column have
        // run: bit (dx * 2 + dz) for the pass at (x - 1 + dx, z - 1 + dz). All
        // four set means the blocks are final and can be lit.
        //
        // The original tracks nothing like this, because it relights lazily and
        // never has to know when a column has stopped changing.
        u8 popMask = 0;

        // Handed out, so it is on the caller's side and may be evicted.
        bool delivered = false;

        u64 lastUse = 0;
        int slot = 0;  // index into blocks_
    };

    static constexpr u8 kFinalMask = 0x0F;

    // `ft.a(int,int)` -- chunkExists. True when the column is in the cache,
    // which is the only sense in which it can exist for the trigger below.
    bool exists(i32 x, i32 z) const { return find(x, z) != nullptr; }

    Entry* find(i32 x, i32 z);
    const Entry* find(i32 x, i32 z) const;

    // `ft.b(int,int)`: load or generate, then the four quadrant triggers.
    // Returns the entry, which is never null unless the cache could not make
    // room. **The only way a column enters the cache**, which is the invariant
    // populate() depends on.
    Entry* ensure(i32 x, i32 z);

    // `ft.a(aw, int, int)` -- populate once, flag first.
    void populate(i32 px, i32 pz);

    Entry* acquire(i32 x, i32 z);
    u8* blocksOf(const Entry& entry) { return blocks_.data() + usize(entry.slot) * usize(kChunkBlocks); }
    const u8* blocksOf(const Entry& entry) const
    {
        return blocks_.data() + usize(entry.slot) * usize(kChunkBlocks);
    }

    bool finalAt(i32 x, i32 z) const;

    // True when the 3x3 around (x, z) has stopped changing, which is what the
    // light engine needs. See the closure note at the top of the file.
    bool lightable(i32 x, i32 z) const;

    // Lights the column, writes it into `out` and marks the entry delivered.
    bool finish(i32 chunkX, i32 chunkZ, world::ChunkColumn* out);

    ChunkProvider provider_;
    world::LightEngine light_;
    PopulationView view_;
    Store store_;

    std::vector<Entry> entries_;
    std::vector<u8> blocks_;
    std::vector<int> freeSlots_;
    u64 clock_ = 0;

    // The columns of a population window that are not resident, held outside
    // the cache so they can never complete a quadrant. See the note in
    // populate() for why that matters.
    //
    // **Kept between passes, because regenerating them was the single largest
    // cost in the generator.** Neighbouring populations in one sweep want the
    // same ring of columns, and generating a column of terrain is ~1.5 ms on
    // the dev host: without this, 61 % of all terrain generated was scratch
    // being made again, and one delivered column cost 6.2 terrain columns
    // instead of 2.4.
    //
    // Safe to cache because a scratch column is read-only and terrain is a pure
    // function of the chunk coordinate and the seed -- the same bytes every
    // time, whenever it is asked for.
    struct Scratch {
        i32 x = 0;
        i32 z = 0;
        bool used = false;
        u64 lastUse = 0;
    };
    static constexpr int kScratchColumns = 24;  // 768 KB
    Scratch scratch_[kScratchColumns];
    std::vector<u8> windowScratch_;

    // The scratch column for (x, z), generating it if it is not held.
    u8* scratchColumn(i32 x, i32 z);

    // Scratch for the hand-over: the two light planes the engine writes in the
    // chunk file's own layout, and one section's worth of blocks gathered out
    // of the column array. 36 KB, held rather than allocated per call.
    std::vector<u8> lightScratch_;
    std::vector<u8> sectionScratch_;

    // The column handed to Store::deliver, and the one Store::load fills.
    // Reused rather than reallocated -- a ChunkColumn is 16 sections of
    // heap-allocated palettes, so making one per chunk would churn the heap on
    // the busiest path the generator has.
    std::unique_ptr<world::ChunkColumn> handover_;
    std::unique_ptr<world::ChunkColumn> loaded_;

    Stats stats_;
};

}  // namespace mc::worldgen
