#pragma once

// The world's chunks in RAM, and the only thing in the process that touches the
// storage slot.
//
// It exists because three separate pieces of SD work used to run on the render
// thread -- an existence check per newly exposed cell, a read plus a gzip
// inflate per column loaded, and both of those queueing behind the generation
// worker's writes on a shared lock. A chunk file is 2,917 bytes at the median on
// a real world, so none of that is bandwidth: it is four to six IPC round trips
// to the FS sysmodule plus FAT metadata, per operation. The lever is the number
// of operations and which thread pays for them, and this class is where both are
// decided. See docs/3ds-performance.md and docs/world-format.md.
//
// Three things in one, because they are the same table looked at three ways:
//
//   1. **A write-back cache.** A finished column is stored here and written by
//      the I/O thread afterwards, so nothing that dirties a column waits for a
//      card. Coalescing falls out of it: a column written twice costs one file.
//   2. **A read-through cache with a retention ring.** A column dropped from the
//      streamer's grid comes back here instead of being freed, and a column the
//      player is walking towards is read ahead of them. Turning round, or
//      crossing a chunk boundary, then costs no SD operation at all. This is the
//      same idea as the original's own `ft` -- a 1024-slot direct-mapped table
//      of live chunks -- with a byte cap and LRU instead of a fixed grid.
//   3. **An existence index.** `hasChunk` is answered from a per-group set
//      rather than a `stat`. See the note on groups below.
//
// ---------------------------------------------------------------------------
// **The invariant everything else rests on: this changes what a read *costs*,
// never what it *says*.**
//
// Population order is the world in a1.1.2 -- two chunks whose passes reach the
// same ground come out differently depending on which ran first -- and
// WorldStreamer's cell classification is what feeds the generation queue. So:
//
//   * every read returns byte-identical content to what the card would return,
//     hit or miss; and
//   * `hasChunk` answers true from the moment `save()` accepts a column, not
//     from the moment its bytes reach the card.
//
// Deferring the *disk* write therefore changes no answer that any
// classification or any generator sweep can observe. This class sits **below**
// ChunkGenerator: `provide()` sees exactly what it saw before, and
// `ChunkGenerator::cacheColumnsFor` is a different cache for a different reason
// and is untouched. The test that proves it is the third arm of
// `a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up`, which compares
// whole world trees byte for byte.
// ---------------------------------------------------------------------------
//
// **Groups.** A *group* is the set of chunks whose existence one listing
// answers -- for the Alpha format, one leaf directory. The layout puts a chunk
// in `<x & 63>/<z & 63>/`, so a group holds only chunks spaced 64 apart: walking
// one chunk lands in a different group every step and returns to a given one
// only after 64. That is what makes a lazy index work. One listing settles up to
// a thousand chunks for the rest of the session, an unvisited region costs
// nothing, there is no cache file and nothing to invalidate -- which is why this
// replaces the "walk 4,096 directories at open and cache to disk" sketch in
// docs/world-format.md rather than implementing it.
//
// **Columns are handed out as clones.** The cache keeps a shared, immutable
// column and every caller gets a copy of it. That is what removes the window a
// move-out would open -- there is never a moment when an entry exists but its
// contents have been handed to someone else -- and the copy is ~18 KB against an
// SD read plus an inflate, which is not a close call. An entry whose column is
// taken by the grid and is not owed to the card is dropped rather than kept, so
// a resident column is not stored twice.
//
// **Which thread may call what** is in the table on each method and is not
// decoration: `tryTake` returning false rather than blocking is the whole reason
// the render thread never touches a card.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"
#include "core/world/level_data.hpp"
#include "core/world/storage.hpp"
#include "version_slots.hpp"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace mc::world {

class ChunkCache {
public:
    struct Config {
        // What retained and read-ahead columns may cost. A column is 18,013
        // bytes on the real world, so 8 MB is ~465 of them -- more than the 264
        // that sit between the load radius and the classification ring at
        // distance 8, which is what it is chosen around. Dirty columns are not
        // counted here; they have their own cap and cannot be evicted.
        usize cleanCapBytes = 8u << 20;

        // **How much finished world may be waiting for the card, and it is a
        // memory backstop rather than a schedule.** Nothing is written outside a
        // flush -- the autosave timer, the pause menu, world exit -- so this is
        // what stops an unbounded queue when generation outruns any interval. A
        // dirty column cannot be evicted, because it is the only copy of that
        // part of the world.
        //
        // 4 MB is ~220 columns at the measured 18,013-byte mean, comfortably
        // more than a New 3DS worker finishes in the 45-second default
        // interval, so in ordinary play the timer governs and this never
        // bites. It is doubled memory while it lasts: the grid holds the
        // column too.
        usize dirtyCapBytes = 4u << 20;

        // Off means every operation happens on the calling thread, which is
        // what the tests and the `--mesh` harness want: no thread, no timing,
        // and a miss is a read rather than a request. The game turns it on.
        bool threaded = false;
    };

    struct Stats {
        u32 hits = 0;            // a column served without touching the card
        u32 misses = 0;          // ...and one that could not be
        u32 prefetchHits = 0;    // of the hits, this many were read ahead
        u32 reads = 0;           // chunk files read
        u32 writes = 0;          // chunk files written
        u32 listings = 0;        // group listings
        u32 stats = 0;           // stat fallbacks: a group asked about too soon
        u32 readsQueued = 0;
        u32 writesQueued = 0;
        u32 groupsQueued = 0;
        usize cleanBytes = 0;
        usize dirtyBytes = 0;
        u32 cleanColumns = 0;
        u32 dirtyColumns = 0;
        u32 evicted = 0;

        // **Must read zero on the console.** Microseconds the main thread spent
        // inside a storage call -- which after this class exists means the
        // `stat` fallback and nothing else. It is the regression test for the
        // whole design, so it is a counter rather than a comment.
        i64 mainThreadMicros = 0;

        bool workerRunning = false;
    };

    // **Where the player was and what time it was**, carried into level.dat by
    // whatever writes it next.
    //
    // It goes through the cache rather than through `level()` directly because
    // `storage_.level()` is the I/O thread's to touch: the housekeeping job
    // encodes and writes it, and a main thread editing the same struct
    // meanwhile is a race the sanitizer would find and a player would
    // experience as a corrupt level.dat. So the main thread hands over a value
    // and the I/O thread applies it under the storage lock.
    //
    // `valid` false leaves the stored player alone, which is what a harness
    // wants -- and what a world with no Player compound must keep getting until
    // somebody actually stands in it, because inventing one moves the player to
    // 0,0,0 the next time it is opened elsewhere.
    struct PlayerState {
        bool valid = false;
        double pos[3] = {0.0, 0.0, 0.0};
        float rotation[2] = {0.0f, 0.0f};  // yaw, pitch
        i64 timeTicks = 0;
    };

    explicit ChunkCache(io::FileSystem& fs) : storage_(fs) {}
    ~ChunkCache();

    ChunkCache(const ChunkCache&) = delete;
    ChunkCache& operator=(const ChunkCache&) = delete;

    // Before open(). Changing it afterwards would mean resizing a table the I/O
    // thread is walking.
    void configure(const Config& config) { config_ = config; }
    const Config& config() const { return config_; }

    // Opens the world and, if configured, starts the I/O thread. A thread that
    // will not start is not fatal: everything falls back to the calling thread,
    // which is exactly what the unthreaded configuration does.
    OpenResult open(const char* worldDir, i64 nowMillis);

    // Flushes everything, blocking, then stops the thread and closes the world.
    // The order matters: the thread is the other user of the storage slot.
    // The player state is applied before level.dat is rewritten, which is what
    // makes leaving a world put you back where you left it.
    //
    // Two overloads rather than a default argument: a nested type's default
    // member initializers are not available while the enclosing class is still
    // being defined, so `= PlayerState{}` does not compile here.
    void close(i64 nowMillis);
    void close(i64 nowMillis, const PlayerState& player);

    bool isOpen() const { return open_; }
    LevelData& level() { return storage_.level(); }
    const LevelData& level() const { return storage_.level(); }

    // ---------------------------------------------------------------- main --

    // Does the world have this chunk? Answered from the index, from a group
    // listing if one has arrived, and only failing both from a `stat` -- which
    // is the one call on this class that can touch a card from the main thread.
    //
    // It cannot be deferred and it cannot be budgeted. WorldStreamer's
    // correctness argument is that a newly exposed cell is classified *before*
    // any sweep in flight could have written it, and a sweep reaches exactly the
    // three rings the classification band is wide. Answering "ask me later"
    // would break that, so instead `warmGroup` is called far enough ahead that
    // the fallback almost never fires.
    bool hasChunk(i32 x, i32 z);

    // What a take found. **Pending and Missing are not interchangeable**, for
    // the same reason WorldStreamer's Absent and Ungenerated are not: a caller
    // that treated a column still being read as unreadable would give up on it,
    // and one that treated a damaged file as pending would ask for it again
    // every frame for the rest of the session.
    enum class Take {
        Took,     // *out is filled
        Pending,  // being read; ask again next frame
        Missing,  // read and failed: no such file, or one that will not decode
    };

    // Takes a column for the grid. **Never touches storage.**
    //
    // A hit fills *out. A miss posts a read request and answers Pending; the
    // caller leaves the cell as it was and tries again next frame, by which time
    // the I/O thread has usually finished. That is the trade that keeps the
    // render thread off the card, and it costs at most a frame of latency on a
    // column that was going to take milliseconds to read anyway.
    //
    // Unthreaded, a miss reads on this thread and answers Took or Missing,
    // which is what the host harnesses and the tests want.
    Take tryTake(i32 x, i32 z, std::unique_ptr<ChunkColumn>* out);

    // A column the grid has finished with. It becomes a retained clean entry, or
    // is dropped if the cache already holds one for that chunk. This is what
    // makes walking back over ground you just left free.
    void give(std::unique_ptr<ChunkColumn> column);

    // Read this column ahead of being asked for it. Lowest priority of the
    // three queues, and dropped if the cache already has the chunk.
    void prefetch(i32 x, i32 z);

    // List the group containing (x, z), so a later hasChunk about any chunk in
    // it is free. Cheap to call repeatedly: a group that is listed or already
    // queued is ignored.
    void warmGroup(i32 x, i32 z);

    // Once a frame. Publishes counters and, unthreaded, does one unit of queued
    // work so a harness still makes progress.
    void pump();

    // Hands every dirty column to the I/O thread. Blocking waits for the queue
    // to drain, which is what close() wants and what the pause menu does not:
    // the world is stopped while the menu is up, so an async flush there is
    // finished before the player resumes and nothing was ever blocked.
    void flush(bool blocking);

    // True when the I/O thread has nothing left. What a test waits on.
    bool idle() const;

    // **level.dat and session.lock, on the I/O thread.**
    //
    // These are small files and it is tempting to write them from wherever the
    // autosave timer happens to run -- which is the main thread. That would be
    // a deflate and an atomic write inside a frame, every interval, which is
    // exactly the thing this class exists to stop; `Stats::mainThreadMicros`
    // would have reported it and it should never have had the chance. So it is
    // a job like any other, and unthreaded it happens on the calling thread
    // because there is nowhere else for it to happen.
    void requestHousekeeping(i64 nowMillis);
    void requestHousekeeping(i64 nowMillis, const PlayerState& player);

    // **By value, under the lock, and not a reference.** The I/O thread writes
    // these counters and the main thread reads them once a frame; handing out a
    // reference means every locked write races an unlocked read. "It is only a
    // counter" is not a reason to leave a read unsynchronised -- the thread
    // sanitizer named this one, exactly as it named the generation worker's
    // counters before it. See docs/status.md.
    Stats stats() const;

    // ------------------------------------------------------- generation ------

    // ChunkGenerator::Store::load, near enough: the column at (x, z), or null.
    //
    // The generator reads the result immediately and does not keep it, so a
    // clone into `scratch` is what it wants anyway. A miss reads from the card
    // on this thread -- the generation worker may block, and making it wait for
    // a low-priority I/O thread on another core would be worse than the read.
    const ChunkColumn* load(i32 x, i32 z, ChunkColumn* scratch);

    // A finished column. Clones it in as dirty, records that the world now has
    // this chunk, and queues the write. **The caller keeps its own column** --
    // the generator reuses it, and the grid wants it.
    bool save(const ChunkColumn& column);

private:
    // Chunk coordinates as one key. Packed rather than a pair so the map node is
    // small and the comparison is one instruction.
    //
    // **Through unsigned the whole way**, because half the coordinates in a
    // world are negative and shifting a negative signed value left is
    // undefined -- which the sanitizer says out loud the first time a chunk at
    // a negative x is asked about. The round trip back through u32 is
    // implementation-defined rather than undefined and is exactly what every
    // compiler this builds on does. The same trap is called out in
    // tests/storage_test.cpp, which is where it was learned the first time.
    static i64 key(i32 x, i32 z)
    {
        return i64((u64(u32(x)) << 32) | u64(u32(z)));
    }
    static i32 keyX(i64 k) { return i32(u32(u64(k) >> 32)); }
    static i32 keyZ(i64 k) { return i32(u32(u64(k))); }

    struct Entry {
        // Shared and immutable. Shared because the I/O thread encodes from it
        // while the main thread may be cloning it; immutable because a change
        // installs a new column and bumps `version` rather than editing one that
        // a write in flight is reading.
        std::shared_ptr<const ChunkColumn> column;
        usize bytes = 0;
        u64 used = 0;       // LRU stamp
        u32 version = 0;    // bumped by save(); a write that finds it changed rewrites
        bool dirty = false;
        bool writing = false;      // handed to the I/O thread
        bool queuedWrite = false;  // on writeQueue_, so flush() does not queue it twice
        bool prefetched = false;   // read ahead rather than asked for, for the stats
    };

    // What one listing answers. `listed` separates "this group is empty" from
    // "nobody has looked", which is the difference between a free answer and a
    // stat. Chunks written this session are inserted whether or not the group
    // has been listed, so a chunk the generator just made is present from the
    // moment it is made.
    struct Group {
        std::vector<i64> chunks;  // sorted; binary searched
        bool listed = false;
        bool queued = false;
    };

    enum class JobKind : u8 { Read, Write, List, Housekeeping };

    // Defaulted member by member so a construction that names only the fields
    // a job kind uses is still complete. The 3DS build turns on
    // -Wmissing-field-initializers and the host one does not, which is exactly
    // the kind of difference worth not discovering at link time.
    struct Job {
        JobKind kind = JobKind::Read;
        i64 chunk = 0;
        u32 group = 0;
        bool prefetch = false;
        i64 nowMillis = 0;
    };

    // --- called with mutex_ held ---
    Entry* find(i64 k);
    void installColumn(i64 k, std::shared_ptr<const ChunkColumn> column, bool dirty,
                       bool prefetched);
    void noteExists(i64 k);
    bool groupSays(i64 k, bool* exists) const;
    void evictLocked();
    void recountLocked();
    bool takeJobLocked(Job* out);
    bool takeWriteLocked(Job* out);
    bool takeHousekeepingLocked(Job* out);

    // Copies the player state into the stored level. **Storage lock held**, and
    // that is the whole reason it exists as its own function.
    void applyPlayerState(const PlayerState& player);

    // --- no lock held ---
    void runJob(const Job& job);
    bool readThrough(i64 k, ChunkColumn* out);

    bool startWorker();
    void stopWorker();
    void workerMain();
    static void workerEntry(void* self);

    mcver::Storage storage_;
    Config config_;
    bool open_ = false;

    // **Two locks, never nested, in either direction.**
    //
    // `mutex_` covers the table, the groups, the queues and the counters, and is
    // held for a lookup or a pointer swap at a time. `storageLock_` covers the
    // storage slot, which the I/O thread and the generation worker both reach,
    // and is held for one file at a time. The main thread takes `storageLock_`
    // in exactly one place -- the `stat` fallback in hasChunk -- and that is why
    // `Stats::mainThreadMicros` is expected to read zero.
    mutable std::mutex mutex_;
    std::mutex storageLock_;
    std::condition_variable wake_;
    std::condition_variable drained_;

    std::map<i64, Entry> entries_;
    std::map<u32, Group> groups_;

    // Three queues rather than one, because their priorities differ and a
    // single queue would let a hundred prefetches sit in front of the column the
    // player is standing on. Drained reads first, then writes, then prefetches.
    std::vector<i64> readQueue_;
    std::vector<i64> writeQueue_;
    std::vector<i64> prefetchQueue_;
    std::vector<u32> groupQueue_;

    // level.dat and the session.lock refresh, as a pending timestamp. One at a
    // time: a second request before the first has run replaces it, because
    // writing an older LastPlayed after a newer one would be worse than
    // skipping the write.
    bool housekeepingPending_ = false;
    i64 housekeepingMillis_ = 0;
    PlayerState housekeepingPlayer_;
    // The state the job in flight is carrying, taken off the pending slot when
    // the job is claimed so a later request cannot overwrite it mid-write.
    PlayerState pendingPlayer_;

    // Reads and prefetches already asked for. **Not a flag on the entry**: a
    // chunk with no entry is exactly the one a read is for, so there would be
    // nowhere to put the flag. Scanning the queues instead would be a few
    // hundred comparisons per cell per frame, which is the cost this class
    // exists to remove.
    std::set<i64> inFlight_;

    // Chunks the world claims to have and that would not read. A damaged file,
    // or one taken off the card under us. Remembered so that the answer is
    // given once rather than re-attempted every frame; cleared by a save(),
    // which is what makes the chunk readable again.
    std::set<i64> unreadable_;

    // One chunk coordinate per queued group, because a group key is
    // deliberately opaque -- the storage slot packs it however its layout
    // wants -- and the listing has to be asked for by chunk.
    std::map<u32, i64> groupRep_;

    u64 clock_ = 0;      // the LRU stamp source
    usize cleanBytes_ = 0;
    usize dirtyBytes_ = 0;

    std::thread worker_;
    void* platformWorker_ = nullptr;
    bool workerRunning_ = false;
    bool workerStop_ = false;
    int jobsActive_ = 0;

    Stats stats_;
};

}  // namespace mc::world
