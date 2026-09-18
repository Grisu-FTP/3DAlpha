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
#include "core/world/tile_entity.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"
#include "impl/worldgen/alpha_nobiome/populate.hpp"
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

        // **Terrain somebody else already generated**, for `blocks` -- which
        // holds `kChunkBlocks`. True when it filled the buffer; false to
        // generate locally, which is what a null callback always means.
        //
        // This exists for one caller: a session where another console has
        // generated the terrain for this column and sent it over. Terrain,
        // the surface pass and caves are pure functions of the seed and the
        // chunk's coordinates -- see caves.hpp, whose carver reads a 17x17
        // neighbourhood *into* this column and never writes out of it -- so a
        // column produced elsewhere is byte-identical to one produced here and
        // arrives in any order without changing the world.
        //
        // **Population is deliberately not delegable.** It is the one stage
        // that writes into its neighbours, so its order is the world (status.md
        // §0g); keeping it on the console that owns the world is what lets the
        // rest be handed out at all. It is also the cheap stage: measured at
        // ~400 us a pass against ~1400 us for the terrain this replaces.
        bool (*supplyTerrain)(void* context, i32 chunkX, i32 chunkZ, u8* blocks) = nullptr;
    };

    struct Stats {
        u32 generated = 0;   // columns whose terrain, surface and caves this ran
        u32 terrainSupplied = 0;  // ...of which came from Store::supplyTerrain
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

        // **The two ways provide() can return false, kept apart.** They were
        // one number once, reported as "the cache could not hold a sweep" --
        // and when generation did stop on hardware, the cause was the other
        // one, so the number said the wrong thing at exactly the moment it was
        // being read. Both must stay zero.
        u32 sweepIncomplete = 0;   // a column of the 6x6 had nowhere to live
        u32 sweepUnlightable = 0;  // the 3x3 never became final

        // What retire() has let go of. `retired` is every column it freed;
        // `retiredLive` is the share of those that had not been handed over,
        // which is the leak it exists to drain. Unlike `evictedLive` these are
        // not a fault: a retired region goes as a unit, so nothing is left
        // holding a half-written neighbour. See retire().
        u32 retired = 0;
        u32 retiredLive = 0;

    };

    // **How large the cache has to be, from the measurement rather than from
    // the geometry.**
    //
    // A column cannot be handed over until it is final and its 3x3 is final, so
    // the generator is always holding two rings of frontier that the sweep has
    // reached and the passes have not finished with. That band's length grows
    // with the radius being generated, which is the one number here that is not
    // a constant. Measured peak, columns held and not yet handed over, filling
    // a fresh world nearest-first from a *fixed* centre:
    //
    //     radius  4 -> 98      radius 8 -> 162      radius 11 -> 210
    //
    // which is close enough to linear that 16 per ring plus the 6x6 sweep and
    // headroom covers it.
    //
    // **This formula is only correct because retire() exists, and for a long
    // time it was not.** Those numbers come from `--generate`, which fills a
    // disc and stops. A player walks, and a moving centre abandons live columns
    // along the sides of the corridor it sweeps: measured under `--fly`, the
    // peak goes 235, 274, 372, 468 as the walk goes 75, 200, 400, 600 chunks,
    // which no fixed size covers. Retiring what the player has left behind is
    // what turns the walking case back into the fixed-centre case this is
    // fitted to.
    //
    // With retirement running, a walk peaks at 128, 158, 192, 224 and 256 for
    // load radii 4, 6, 8, 10 and 12 -- `16r + 64`, so this leaves a flat 32
    // columns of headroom at every distance. **Undersizing is still not a slow
    // path but a wrong world**; see the note on `evictedLive` in acquire().
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

    // **Forgets everything further than `radius` from (centreX, centreZ)**, and
    // it is what keeps the cache from growing without bound.
    //
    // A live column -- one whose four passes have not all run -- cannot be
    // evicted, because its neighbours' populations are written into it and
    // nowhere else. Nothing ever finishes the ones the player walked away from:
    // a column at the lateral edge of the corridor a walk sweeps out never gets
    // its eastern and southern neighbours, so it stays live for the rest of the
    // session. Measured on the host with an 8,240-column cache, walking in a
    // straight line at distance 8, peak live against distance walked:
    //
    //     75 chunks -> 235      200 -> 274      400 -> 372      600 -> 468
    //
    // -- linear in distance travelled and bounded by nothing. Whatever
    // `cacheColumnsFor` says, a long enough session fills it, and then acquire()
    // has no delivered victim and takes a live column instead: `evictedLive`,
    // after which that column comes back as bare terrain with its neighbours'
    // trees and snow missing, `lightable` fails for it and its neighbours for
    // ever, and generation stops until the world is reloaded. All four of those
    // were reported from hardware at once.
    //
    // **Dropping a whole region at once is what makes this safe**, and dropping
    // one column is not. A live column that goes on its own comes back bare
    // while the passes that wrote into it stay marked done, so its share of
    // their work is lost silently. Everything outside the radius goes together,
    // so the neighbourhood re-derives as a unit -- exactly the state a world
    // reload leaves behind, which is the case the design already accounts for
    // (see ChunkCache::save on regenerating against populated neighbours).
    //
    // The radius has to cover what a sweep can reach: provide() touches
    // (cx-3..cx+2), and the caller's own load radius on top of that. Returns the
    // number of columns freed.
    u32 retire(i32 centreX, i32 centreZ, int radius);

    // One place worth keeping, in chunk coordinates.
    struct Centre {
        i32 chunkX = 0;
        i32 chunkZ = 0;
    };

    // **The same, for a world with more than one player in it.** A column is
    // kept when it is within `radius` of *any* of the centres, so a host
    // serving ground to a guest on the other side of the map does not retire
    // the guest's neighbourhood every time it sweeps for its own -- which
    // would be correct (a region always goes as a unit) and would generate
    // everything twice. `count` may be zero, which is the single-centre call
    // above. See WorldStreamer::setServedAreas.
    u32 retire(const Centre* centres, int count, int radius);

    // What a sweep in progress needs kept: provide() touches (cx-3..cx+2) in
    // both axes, which is Chebyshev radius 3 around its own centre.
    static constexpr int kSweepKeepRadius = 3;

    // Columns held that have not been handed over. `Stats::peakLive` is this at
    // its worst; this is it now, which is what a caller sizing its retirement
    // radius wants.
    u32 liveColumns() const;

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

        // **What population produced that is not a block.** A dungeon writes a
        // spawner and up to two chests, and the mob and the loot are exactly
        // the two things 32,768 bytes of block ids cannot hold -- see
        // impl/worldgen/alpha_nobiome/dungeon.hpp.
        //
        // It lives on the entry rather than on the column because a pass at
        // (px, pz) writes into a 2x2 quadrant, so a dungeon rolled for one
        // chunk routinely lands in the next one, and the record has to follow
        // the blocks. `finish` hands the list to the column it builds.
        //
        // Empty for the overwhelming majority of entries: eight tries a chunk
        // and nearly all of them refused.
        std::vector<world::TileEntity> tileEntities;
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

    // **Records that the pass at (px, pz) has run**, by setting its bit in the
    // popMask of each of the four columns it can write into.
    //
    // Separate from populate() because a pass can have run in a *previous
    // session*: a column that came out of the save with `terrainPopulated` set
    // is one whose pass is done, and the original skips it on that basis. Its
    // four columns still have to be told, or a freshly generated neighbour of a
    // stored chunk waits for ever for a pass that will never run again. See the
    // note in ensure().
    void notePopulated(i32 px, i32 pz);

    Entry* claimFreeSlot(i32 x, i32 z);
    Entry* acquire(i32 x, i32 z);

    // The column provide() is sweeping for, so acquire()'s last resort knows
    // which 36 entries it must not let go of. Meaningless outside a sweep, and
    // only read there.
    i32 sweepX_ = 0;
    i32 sweepZ_ = 0;
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

    // Routes the last pass's dungeon records onto the entries whose columns
    // they landed in. See the note on Entry::tileEntities.
    void recordDungeons();

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

    // Terrain for one column: the session's, when it has it, and this
    // console's generator otherwise. See Store::supplyTerrain.
    void makeTerrain(i32 x, i32 z, u8* blocks);

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

    // Reused across populations so a chunk with no dungeon in it -- nearly
    // every one -- allocates nothing on the generation worker.
    PopulationSideEffects sideEffects_;
    std::unique_ptr<world::ChunkColumn> loaded_;

    Stats stats_;
};

}  // namespace mc::worldgen
