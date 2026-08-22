#include "impl/worldgen/alpha_nobiome/chunk_generator.hpp"

#include "core/world/chunk.hpp"
#include "impl/worldgen/alpha_nobiome/dungeon.hpp"
#include "impl/worldgen/alpha_nobiome/populate.hpp"

#include <cstring>
#include <memory>

namespace mc::worldgen {
namespace {

using world::ChunkColumn;
using world::Section;

constexpr int kRuns = ChunkColumn::kArea;            // one vertical run per (x, z)
constexpr int kRunBlocks = Section::kSize;           // 16 blocks of it per section
constexpr int kColumnStride = ChunkColumn::kHeight;  // bytes between runs
constexpr int kColumnNibbleStride = ChunkColumn::kHeight / 2;

// A column array is 256 vertical runs laid end to end and a section takes the
// same slice out of every one of them.
//
// **The same two loops live in impl/storage/alpha_chunkfiles/chunk_nbt.cpp**,
// and they are not shared on purpose: that is a different slot. The column
// array is a fact about a1.1.2's *generator* here and about a1.1.2's *file
// format* there, the two happen to agree, and a version where they stop
// agreeing must be able to change one without the other. Sixteen lines is the
// price of that.
void gatherBlocks(const u8* column, int sy, u8* dst)
{
    const u8* src = column + sy * kRunBlocks;
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kRunBlocks, src + run * kColumnStride, usize(kRunBlocks));
    }
}

// The other direction, for a column that came out of the save: the section's
// 4,096 bytes go back into the 256 vertical runs they were taken from.
void scatterBlocks(const u8* src, int sy, u8* column)
{
    u8* dst = column + sy * kRunBlocks;
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kColumnStride, src + run * kRunBlocks, usize(kRunBlocks));
    }
}

void gatherNibbles(const u8* column, int sy, u8* dst)
{
    const u8* src = column + sy * (kRunBlocks / 2);
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * (kRunBlocks / 2), src + run * kColumnNibbleStride,
                    usize(kRunBlocks / 2));
    }
}

// Which bit of Entry::popMask the population at (px, pz) sets in the column at
// (x, z). The four passes that can reach a column are the ones at (x-1, z-1),
// (x-1, z), (x, z-1) and (x, z).
u8 popBit(i32 x, i32 z, i32 px, i32 pz)
{
    const i32 dx = px - (x - 1);
    const i32 dz = pz - (z - 1);
    return u8(1u << u32(dx * 2 + dz));
}

}  // namespace

ChunkGenerator::ChunkGenerator(i64 seed, GeneratorOptions options, Store store,
                               int cacheColumns)
    : provider_(seed, options), store_(store)
{
    // The 6x6 sweep plus the ring of window columns its outermost passes pull
    // in on demand -- an 8x8 -- all have to be resident at once, so the cache
    // can never be smaller than that.
    const int capacity = cacheColumns < 64 ? 64 : cacheColumns;

    entries_.resize(usize(capacity));
    blocks_.resize(usize(capacity) * usize(kChunkBlocks));
    freeSlots_.reserve(usize(capacity));
    for (int i = capacity - 1; i >= 0; --i) {
        freeSlots_.push_back(i);
    }

    windowScratch_.resize(usize(kScratchColumns) * usize(kChunkBlocks));
    lightScratch_.resize(usize(world::NibbleArray::kBytes) * usize(ChunkColumn::kSectionCount) * 2);
    sectionScratch_.resize(usize(Section::kVolume));
}

ChunkGenerator::~ChunkGenerator() = default;

ChunkGenerator::Entry* ChunkGenerator::find(i32 x, i32 z)
{
    for (Entry& e : entries_) {
        if (e.used && e.x == x && e.z == z) {
            return &e;
        }
    }
    return nullptr;
}

const ChunkGenerator::Entry* ChunkGenerator::find(i32 x, i32 z) const
{
    return const_cast<ChunkGenerator*>(this)->find(x, z);
}

u8* ChunkGenerator::scratchColumn(i32 x, i32 z)
{
    Scratch* victim = nullptr;
    for (Scratch& s : scratch_) {
        if (s.used && s.x == x && s.z == z) {
            s.lastUse = ++clock_;
            ++stats_.scratchHits;
            return windowScratch_.data() + usize(&s - scratch_) * usize(kChunkBlocks);
        }
        if (!s.used) {
            victim = &s;
        } else if (victim == nullptr || (victim->used && s.lastUse < victim->lastUse)) {
            victim = &s;
        }
    }

    victim->used = true;
    victim->x = x;
    victim->z = z;
    victim->lastUse = ++clock_;
    u8* blocks = windowScratch_.data() + usize(victim - scratch_) * usize(kChunkBlocks);
    provider_.generateColumn(x, z, blocks);
    ++stats_.generated;
    ++stats_.scratchColumns;
    return blocks;
}

bool ChunkGenerator::finalAt(i32 x, i32 z) const
{
    const Entry* e = find(x, z);
    return e != nullptr && e->popMask == kFinalMask;
}

// Takes a slot for (x, z), evicting if it has to.
//
// **Only a delivered column may be evicted.** One that has been handed out is
// on the caller's side and, by this class's contract, in the world's save; one
// that has not is still being written into by its neighbours' population, and
// dropping it would lose their work silently -- the column would come back as
// bare terrain the next time it was reached. `evictedLive` counts the case
// where there was nothing else to take, which is the cache being too small.
ChunkGenerator::Entry* ChunkGenerator::acquire(i32 x, i32 z)
{
    if (!freeSlots_.empty()) {
        for (Entry& e : entries_) {
            if (!e.used) {
                e = Entry{};
                e.slot = freeSlots_.back();
                freeSlots_.pop_back();
                e.used = true;
                e.x = x;
                e.z = z;
                e.lastUse = ++clock_;
                return &e;
            }
        }
    }

    Entry* victim = nullptr;
    for (Entry& e : entries_) {
        if (!e.delivered) {
            continue;
        }
        if (victim == nullptr || e.lastUse < victim->lastUse) {
            victim = &e;
        }
    }
    if (victim == nullptr) {
        ++stats_.evictedLive;
        for (Entry& e : entries_) {
            if (victim == nullptr || e.lastUse < victim->lastUse) {
                victim = &e;
            }
        }
    }
    if (victim == nullptr) {
        return nullptr;
    }

    ++stats_.evicted;
    const int slot = victim->slot;
    *victim = Entry{};
    victim->slot = slot;
    victim->used = true;
    victim->x = x;
    victim->z = z;
    victim->lastUse = ++clock_;
    return victim;
}

// `ft.b(int, int)`: the chunk at (x, z) from the cache, from the save or newly
// generated, followed by the four quadrant checks that decide whether anything
// can now be populated.
//
// **The trigger is the thing being transcribed here.** Which chunks get
// populated is not "the ones we asked for" -- it is `ft`'s answer over the
// sequence of arrivals, which for a rectangular sweep is one ring smaller than
// the sweep on the east and south. Deciding that set ourselves instead, which
// this file did in its first form, populates a different set from the original
// and the difference reaches the delivered columns through four steps of
// neighbour population.
//
// The original's last-chunk fast path (`ft.a`/`ft.b` fields) is left out: it is
// a one-entry memo in front of a 1024-entry array and changes no answer.
ChunkGenerator::Entry* ChunkGenerator::ensure(i32 x, i32 z)
{
    if (Entry* hit = find(x, z)) {
        hit->lastUse = ++clock_;
        return hit;
    }
    ++stats_.cacheMisses;

    Entry* entry = acquire(x, z);
    if (entry == nullptr) {
        return nullptr;
    }

    bool populated = false;
    bool loaded = false;
    if (store_.load != nullptr) {
        if (loaded_ == nullptr) {
            loaded_ = std::make_unique<world::ChunkColumn>();
        }
        *loaded_ = world::ChunkColumn(x, z);
        const world::ChunkColumn* existing = store_.load(store_.context, x, z, loaded_.get());
        if (existing != nullptr) {
            loaded = true;
            populated = existing->terrainPopulated;
            u8* blocks = blocksOf(*entry);
            for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
                existing->section(sy).writeBlocks(sectionScratch_.data());
                scatterBlocks(sectionScratch_.data(), sy, blocks);
            }
        }
    }
    if (loaded) {
        ++stats_.loaded;
        entry->populated = populated;
        // **A column that came out of the save is treated as final**, and this
        // is the one assumption in the file that the jar cannot settle for us.
        // The original never needs to decide: it relights from a queue, so a
        // chunk arriving from disk is simply relit against whatever is around
        // it. We light once, so we have to say when a column has stopped
        // changing -- and a populated chunk in a real save has already been
        // through the whole pipeline, so re-running any pass into it would
        // duplicate what is there. An unpopulated one is bare terrain and its
        // four passes are still owed, exactly as if we had generated it.
        if (populated) {
            entry->popMask = kFinalMask;

            // **And it may be evicted again the moment it is in the way.** It
            // came out of the store, so the store still has it; nothing here
            // will write to it, because every pass that could has run. Leaving
            // this false is what made the cache fill with columns it had just
            // read back and could not let go of, until it started evicting
            // ones that were still being written into.
            entry->delivered = true;
        }
    } else {
        ++stats_.generated;
        provider_.generateColumn(x, z, blocksOf(*entry));
        entry->populated = false;
    }

    // The four overlapping quadrants, in the original's order and with its
    // conditions -- including the two it tests twice, which are transcribed
    // rather than tidied so a diff against `ft`'s bytecode stays readable.
    if (!entry->populated && exists(x + 1, z + 1) && exists(x, z + 1) && exists(x + 1, z)) {
        populate(x, z);
    }
    if (exists(x - 1, z) && !find(x - 1, z)->populated && exists(x - 1, z + 1) &&
        exists(x, z + 1) && exists(x - 1, z)) {
        populate(x - 1, z);
    }
    if (exists(x, z - 1) && !find(x, z - 1)->populated && exists(x + 1, z - 1) &&
        exists(x, z - 1) && exists(x + 1, z)) {
        populate(x, z - 1);
    }
    if (exists(x - 1, z - 1) && !find(x - 1, z - 1)->populated && exists(x - 1, z - 1) &&
        exists(x, z - 1) && exists(x - 1, z)) {
        populate(x - 1, z - 1);
    }

    // A population above may have taken a slot, which cannot move this entry --
    // an undelivered column is never a victim -- but looking it up again costs
    // nothing next to what just ran and does not lean on that argument.
    return find(x, z);
}

// `ft.a(aw, int, int)`. The flag goes up **before** the pass runs, which in the
// original is what stops `World.setBlock` generating a chunk that populates
// back into the one being populated. We have no such recursion, and the order
// is kept anyway because it is the original's.
void ChunkGenerator::populate(i32 px, i32 pz)
{
    Entry* centre = find(px, pz);
    if (centre == nullptr || centre->populated) {
        return;
    }

    // The 3x3 window, which tests/populate_test.cpp measures to be exactly as
    // good as the 5x5 the oracle fixture was captured with. Every one of the
    // nine has to be resident: a null would read as air where the original
    // reads terrain.
    //
    // **A missing window column is generated here into scratch, and the scratch
    // is the important half.**
    //
    // `ft`'s trigger only guarantees the 2x2 quadrant is resident. A generator
    // reading one block further west makes `World.getBlockId` fetch a chunk
    // that is not loaded, and `ft.b` generates it on the spot -- mutual
    // recursion between generation and population, which a 32 KB stack cannot
    // have. So it is done up front instead, from the same pure terrain
    // function, which needs no recursion because it runs no triggers.
    //
    // **What the original then does with that chunk is what we must not copy.**
    // It keeps it, so `chunkExists` answers true for it from then on -- and
    // `ft.b` skips its whole body, triggers included, for a chunk that already
    // exists. A column pulled in this way is therefore never the one that
    // completes a quadrant, and the passes that would have written into its
    // neighbours never run. In the original that is invisible: it relights
    // lazily and has no notion of a column being finished. Here it is fatal,
    // because a column whose passes can never run can never be lit, and the
    // generator would refuse to deliver anything near it for the rest of the
    // session. It showed up as the fourth column of a spiral failing outright.
    //
    // Holding these in scratch instead keeps one invariant that makes the whole
    // thing work: **a column enters the cache only through ensure(), so every
    // resident column has had the triggers run on it.** Population then fires
    // for a chunk as soon as the last of its four quadrant columns arrives --
    // whichever of the four that is, since all four trigger cases are here --
    // so every pass the closure needs is guaranteed rather than hoped for.
    //
    // It is safe because population **writes** only inside the 2x2 quadrant,
    // which is resident by the trigger's own precondition; the scratch columns
    // are read and thrown away. That is not an assumption: writes outside the
    // quadrant are counted in `populationEscapes`, and the count is asserted
    // zero against a real World.
    //
    // The content is identical either way, since terrain is a pure function of
    // the chunk coordinate and the seed. Only the cache membership differs.
    //
    // **The flag goes up only once the window is there**, unlike the original,
    // which sets it first. Nothing can reach back into this call to observe the
    // difference, and bailing out with it already set would mark a chunk
    // populated that never was.
    u8* window[9] = {};
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dz = -1; dz <= 1; ++dz) {
            const int slot = (dx + 1) * 3 + (dz + 1);
            Entry* e = find(px + dx, pz + dz);
            if (e != nullptr) {
                e->lastUse = ++clock_;
                window[slot] = blocksOf(*e);
                continue;
            }
            // Safe against the scratch cache evicting a column this same loop
            // already put in the window: it evicts least-recently-used, there
            // are 24 slots, and at most nine of them can be claimed here.
            window[slot] = scratchColumn(px + dx, pz + dz);
        }
    }
    centre->populated = true;


    view_.reset(px - 1, pz - 1, 3, 3, window);

    PopulationSideEffects sideEffects;
    populateChunk(provider_, view_, px, pz, &sideEffects);
    ++stats_.populated;

    stats_.refusedOutOfWindow += view_.refusedOutOfWindow();
    stats_.droppedChests += u32(sideEffects.chests.size());
    stats_.droppedSpawners += u32(sideEffects.spawners.size());

    // The finality rule, checked rather than assumed. Population is expected to
    // have written only into the 2x2 quadrant (px..px+1, pz..pz+1); anything
    // outside it may already have been lit and handed to the renderer, so it
    // would be a column that changed after it was finished.
    //
    // The window is 3x3 and PopulationView indexes its mask by kMaxColumns, so
    // the quadrant's four bits are (1,1), (1,2), (2,1) and (2,2).
    constexpr u32 kQuadrant = (1u << (1 * PopulationView::kMaxColumns + 1)) |
                              (1u << (1 * PopulationView::kMaxColumns + 2)) |
                              (1u << (2 * PopulationView::kMaxColumns + 1)) |
                              (1u << (2 * PopulationView::kMaxColumns + 2));
    if ((view_.writtenColumns() & ~kQuadrant) != 0) {
        ++stats_.populationEscapes;
    }

    for (int dx = 0; dx <= 1; ++dx) {
        for (int dz = 0; dz <= 1; ++dz) {
            Entry* e = find(px + dx, pz + dz);
            if (e != nullptr) {
                e->popMask |= popBit(px + dx, pz + dz, px, pz);
            }
        }
    }
}

bool ChunkGenerator::lightable(i32 x, i32 z) const
{
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dz = -1; dz <= 1; ++dz) {
            if (!finalAt(x + dx, z + dz)) {
                return false;
            }
        }
    }
    return true;
}

bool ChunkGenerator::finish(i32 chunkX, i32 chunkZ, world::ChunkColumn* out)
{
    const u8* window[9] = {};
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dz = -1; dz <= 1; ++dz) {
            window[(dx + 1) * 3 + (dz + 1)] = blocksOf(*find(chunkX + dx, chunkZ + dz));
        }
    }

    constexpr usize kPlaneBytes =
        usize(world::NibbleArray::kBytes) * usize(ChunkColumn::kSectionCount);
    u8* sky = lightScratch_.data();
    u8* blockLight = lightScratch_.data() + kPlaneBytes;

    *out = ChunkColumn(chunkX, chunkZ);
    out->terrainPopulated = true;
    light_.computeCentre(window, sky, blockLight, out->heightMap);
    ++stats_.lit;

    const u8* raw = window[4];
    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        Section& section = out->section(sy);

        gatherBlocks(raw, sy, sectionScratch_.data());
        section.assignBlocks(ConstByteSpan(sectionScratch_.data(), Section::kVolume));

        gatherNibbles(sky, sy, sectionScratch_.data());
        section.skyLight().assign(ConstByteSpan(sectionScratch_.data(), world::NibbleArray::kBytes));

        gatherNibbles(blockLight, sy, sectionScratch_.data());
        section.blockLight().assign(
            ConstByteSpan(sectionScratch_.data(), world::NibbleArray::kBytes));

        // **Block metadata is uniformly zero, and that is a1.1.2's answer
        // rather than a gap.** Every write population makes goes through
        // `World.setBlock(x, y, z, id)`, the three-argument form; nothing in
        // any of the nine generators calls the metadata-carrying one. The
        // Section's Data plane is already zero from construction, so there is
        // nothing to copy in.
    }

    find(chunkX, chunkZ)->delivered = true;
    return true;
}

void ChunkGenerator::growCacheTo(int cacheColumns)
{
    const usize wanted = usize(cacheColumns < 0 ? 0 : cacheColumns);
    if (wanted <= entries_.size()) {
        return;
    }
    const usize had = entries_.size();
    entries_.resize(wanted);
    blocks_.resize(wanted * usize(kChunkBlocks));
    for (usize i = wanted; i > had; --i) {
        freeSlots_.push_back(int(i - 1));
    }
}

void ChunkGenerator::flush()
{
    if (store_.deliver == nullptr) {
        return;
    }
    if (handover_ == nullptr) {
        handover_ = std::make_unique<world::ChunkColumn>();
    }

    // A pass over the cache rather than a queue, because finishing one column
    // does not make another finishable -- only a population does, and those
    // have all run by the time this is reached.
    for (usize i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (!e.used || e.delivered || e.popMask != kFinalMask) {
            continue;
        }
        const i32 x = e.x;
        const i32 z = e.z;
        if (!lightable(x, z)) {
            continue;
        }
        finish(x, z, handover_.get());
        store_.deliver(store_.context, *handover_);
    }
}

bool ChunkGenerator::provide(i32 chunkX, i32 chunkZ, world::ChunkColumn* out)
{
    // **The sweep, and why it is row-major.**
    //
    // Lighting the centre needs the 3x3 around it final; a column is final once
    // the four passes at (cx-1..cx, cz-1..cz) have run; a pass at (px, pz)
    // reads and writes a 3x3 of terrain around itself. Composing those gives
    // passes over (cx-2..cx+1) and terrain over (cx-3..cx+2) -- the 6x6 below.
    //
    // `ft`'s trigger then populates one ring less than the sweep on the east
    // and south, so the sweep actually runs the passes over (cx-3..cx+1)^2,
    // which contains what the closure needs. That extra west-and-north ring is
    // not slack: it is what the original populates for the same sequence of
    // loads, and the passes in it feed the ones the closure does need.
    //
    // Row-major, x outer, is the order the fixture in tests/generate_test.cpp
    // was captured with. **It is a real `ft` order, not an invention** -- it is
    // what a client produces whose chunk loads happen to arrive row-major. The
    // original's own order follows the player and is not reproducible from a
    // seed at all; see the note at the top of this file.
    for (i32 x = chunkX - 3; x <= chunkX + 2; ++x) {
        for (i32 z = chunkZ - 3; z <= chunkZ + 2; ++z) {
            if (ensure(x, z) == nullptr) {
                return false;
            }
        }
    }

    u32 live = 0;
    for (const Entry& e : entries_) {
        if (e.used && !e.delivered) {
            ++live;
        }
    }
    if (live > stats_.peakLive) {
        stats_.peakLive = live;
    }

    if (!lightable(chunkX, chunkZ)) {
        return false;
    }
    const bool ok = finish(chunkX, chunkZ, out);

    // Everything else the sweep finished on the way. Without this the cache has
    // to hold it until someone asks, which is a band whose width is three rings
    // and whose length grows with the render distance.
    flush();
    return ok;
}

}  // namespace mc::worldgen
