#include "core/world/chunk_cache.hpp"

#include "core/util/memory.hpp"
#include "core/util/worker.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace mc::world {
namespace {

// Collects a group listing. The storage slot reports chunk coordinates through
// a plain function pointer, so this is the context it carries.
struct GroupScan {
    std::vector<i64>* out;
};

bool collectChunk(void* context, i32 x, i32 z)
{
    GroupScan& scan = *static_cast<GroupScan*>(context);
    // Same packing as ChunkCache::key, and unsigned for the same reason: half
    // the world has a negative coordinate.
    scan.out->push_back(i64((u64(u32(x)) << 32) | u64(u32(z))));
    return true;
}

}  // namespace

ChunkCache::~ChunkCache()
{
    // A cache destroyed without close() would leave the thread running against
    // a storage slot that is about to go. Nothing dirty is written here on
    // purpose: silently doing I/O from a destructor is how a failure becomes
    // invisible. close() is the way out, and WorldStreamer calls it.
    stopWorker();
}

OpenResult ChunkCache::open(const char* worldDir, i64 nowMillis)
{
    const OpenResult result = storage_.open(worldDir, nowMillis);
    if (result != OpenResult::Ok) {
        return result;
    }
    open_ = true;
    entries_.clear();
    cleanColumns_ = 0;
    dirtyColumns_ = 0;
    groups_.clear();
    readQueue_.clear();
    writeQueue_.clear();
    prefetchQueue_.clear();
    groupQueue_.clear();
    urgentGroups_.clear();
    surveyQueue_.clear();
    surveyQueued_.clear();
    inFlight_.clear();
    unreadable_.clear();
    housekeepingPending_ = false;
    cleanBytes_ = 0;
    dirtyBytes_ = 0;
    dirtyCap_ = config_.dirtyCapBytes;
    stats_ = Stats{};

    if (config_.threaded && !startWorker()) {
        // Not fatal. Everything falls back to the calling thread, which is the
        // unthreaded configuration the tests run -- slower, never wrong.
    }
    stats_.workerRunning = workerRunning_;
    return OpenResult::Ok;
}

void ChunkCache::close(i64 nowMillis)
{
    close(nowMillis, PlayerState{});
}

void ChunkCache::close(i64 nowMillis, const PlayerState& player)
{
    if (!open_) {
        return;
    }

    // **Speculation is abandoned, not finished.** Reading a column ahead of a
    // player who is leaving, or listing a directory to answer a question
    // nobody will ask again, is work whose only effect would be to keep the
    // "Saving level.." screen up. What is owed to the card is a different
    // matter and is what flush waits for. A read already in flight finishes on
    // its own; nothing here interrupts a job, it only stops more being taken.
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const i64 k : prefetchQueue_) {
            inFlight_.erase(k);
        }
        prefetchQueue_.clear();
        // Speculative in exactly the same way, and abandoned on the same terms.
        surveyQueue_.clear();
        surveyQueued_.clear();
        for (const u64 g : groupQueue_) {
            groups_[g].queued = false;
        }
        groupQueue_.clear();
        for (const u64 g : urgentGroups_) {
            groups_[g].queued = false;
            groups_[g].urgent = false;
        }
        urgentGroups_.clear();
    }

    flush(true);
    stopWorker();
    // After the thread is joined, so this is single-threaded again and the
    // level can be edited in place. storage_.close() rewrites level.dat, which
    // is what carries the position out.
    applyPlayerState(player);
    // close() commits in both backends, but saying so here keeps the ordering
    // explicit: everything durable before the level is written out.
    storage_.commit();
    storage_.close(nowMillis);
    entries_.clear();
    cleanColumns_ = 0;
    dirtyColumns_ = 0;
    groups_.clear();
    cleanBytes_ = 0;
    dirtyBytes_ = 0;
    open_ = false;
}

// ---------------------------------------------------------------- lookups --

ChunkCache::Entry* ChunkCache::find(i64 k)
{
    auto it = entries_.find(k);
    return it == entries_.end() ? nullptr : &it->second;
}

bool ChunkCache::groupSays(i64 k, bool* exists) const
{
    const u64 g = storage_.chunkGroupKey(keyX(k), keyZ(k));
    auto it = groups_.find(g);
    if (it == groups_.end()) {
        return false;
    }
    const Group& group = it->second;
    const bool listed = std::binary_search(group.chunks.begin(), group.chunks.end(), k);
    if (listed) {
        // A chunk we know about is present whether or not the directory has
        // been walked -- save() puts it here the moment it is made.
        *exists = true;
        return true;
    }
    if (!group.listed) {
        return false;  // nobody has looked; absence here means nothing
    }
    *exists = false;
    return true;
}

void ChunkCache::noteExists(i64 k)
{
    Group& group = groups_[storage_.chunkGroupKey(keyX(k), keyZ(k))];
    auto at = std::lower_bound(group.chunks.begin(), group.chunks.end(), k);
    if (at == group.chunks.end() || *at != k) {
        group.chunks.insert(at, k);
    }
}

ChunkCache::Presence ChunkCache::chunkPresence(i32 x, i32 z)
{
    const i64 k = key(x, z);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (find(k) != nullptr) {
            return Presence::Present;  // in hand, therefore in the world
        }
        bool exists = false;
        if (groupSays(k, &exists)) {
            return exists ? Presence::Present : Presence::Absent;
        }
        if (workerRunning_) {
            // Somebody is waiting on this one, so it goes to the head of the
            // listing queue. Unthreaded there would be nobody to run it and
            // "ask me later" would be a cell that never gets classified, which
            // is why the fall-through below still exists.
            warmGroupLocked(x, z, true);
            return Presence::Unknown;
        }
    }
    return hasChunk(x, z) ? Presence::Present : Presence::Absent;
}

bool ChunkCache::hasChunk(i32 x, i32 z)
{
    const i64 k = key(x, z);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (find(k) != nullptr) {
            return true;  // in hand, therefore in the world
        }
        bool exists = false;
        if (groupSays(k, &exists)) {
            return exists;
        }
    }

    // The fallback, and the only storage call on this class that the main
    // thread can make. It fires when a group is asked about before its listing
    // has arrived -- a player sprinting into unwalked ground. Correct, just as
    // slow as it was before any of this existed, and counted so that it can be
    // seen going away.
    const auto begin = std::chrono::steady_clock::now();
    bool exists = false;
    {
        payOpLatency();
        std::lock_guard<std::mutex> guard(storageLock_);
        exists = storage_.hasChunk(x, z);
    }
    const i64 micros = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - begin)
                           .count();

    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.stats;
    stats_.mainThreadMicros += micros;
    if (exists) {
        noteExists(k);
    }
    return exists;
}

ChunkCache::Take ChunkCache::tryTake(i32 x, i32 z, std::unique_ptr<ChunkColumn>* out)
{
    const i64 k = key(x, z);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        Entry* entry = find(k);
        if (entry != nullptr && entry->column != nullptr) {
            *out = std::make_unique<ChunkColumn>(entry->column->clone());
            ++stats_.hits;
            if (entry->prefetched) {
                ++stats_.prefetchHits;
                entry->prefetched = false;
            }
            if (!entry->dirty && !entry->writing) {
                // The grid owns the content now and the card already agrees
                // with it, so keeping a second copy here would store every
                // resident column twice. One owed to the card stays.
                cleanBytes_ -= entry->bytes;
                --cleanColumns_;
                entries_.erase(k);
            } else {
                entry->used = ++clock_;
            }
            return Take::Took;
        }
        if (unreadable_.count(k) != 0) {
            return Take::Missing;
        }

        if (workerRunning_) {
            if (inFlight_.insert(k).second) {
                // Counted here rather than on every frame the caller asks
                // again, so the hit rate measures reads rather than retries.
                ++stats_.misses;
                readQueue_.push_back(k);
                wake_.notify_one();
            }
            return Take::Pending;  // the render thread waits for nothing
        }
        ++stats_.misses;
    }

    // Unthreaded: read here. This is what the host harnesses and the tests run,
    // and what a console whose I/O thread would not start falls back to.
    auto column = std::make_unique<ChunkColumn>(x, z);
    if (!readThrough(k, column.get())) {
        std::lock_guard<std::mutex> guard(mutex_);
        unreadable_.insert(k);
        return Take::Missing;
    }
    *out = std::move(column);
    return Take::Took;
}

void ChunkCache::give(std::unique_ptr<ChunkColumn> column)
{
    if (column == nullptr) {
        return;
    }
    const i64 k = key(column->x, column->z);
    std::lock_guard<std::mutex> guard(mutex_);
    if (find(k) != nullptr) {
        return;  // already held, and what is held is at least as fresh
    }
    installColumn(k, std::shared_ptr<const ChunkColumn>(column.release()), false, false);
    evictLocked();
}

void ChunkCache::prefetch(i32 x, i32 z)
{
    if (!workerRunning_) {
        return;  // reading ahead on the thread that would consume it buys nothing
    }
    const i64 k = key(x, z);
    std::lock_guard<std::mutex> guard(mutex_);
    if (find(k) != nullptr || cleanBytes_ >= config_.cleanCapBytes) {
        return;
    }
    if (!inFlight_.insert(k).second) {
        return;
    }
    prefetchQueue_.push_back(k);
    wake_.notify_one();
}

void ChunkCache::setSurveyor(SurveyFn visit, void* context)
{
    std::lock_guard<std::mutex> guard(mutex_);
    surveyor_ = visit;
    surveyorContext_ = context;
}

bool ChunkCache::survey(i32 x, i32 z)
{
    const i64 k = key(x, z);
    std::lock_guard<std::mutex> guard(mutex_);
    if (!open_ || surveyor_ == nullptr) {
        return false;
    }
    if (!surveyQueued_.insert(k).second) {
        return false;
    }
    surveyQueue_.push_back(k);
    wake_.notify_one();
    return true;
}

void ChunkCache::cancelSurveys()
{
    std::lock_guard<std::mutex> guard(mutex_);
    surveyQueue_.clear();
    surveyQueued_.clear();
    surveyor_ = nullptr;
    surveyorContext_ = nullptr;
}

void ChunkCache::warmGroup(i32 x, i32 z, bool urgent)
{
    std::lock_guard<std::mutex> guard(mutex_);
    warmGroupLocked(x, z, urgent);
}

void ChunkCache::warmGroupLocked(i32 x, i32 z, bool urgent)
{
    const u64 g = storage_.chunkGroupKey(x, z);
    Group& group = groups_[g];
    if (group.listed) {
        return;
    }
    if (group.queued) {
        if (!urgent || group.urgent) {
            return;
        }
        // Promotion, not a second entry: it is already going to be listed, and
        // what has changed is that a cell is now waiting on it. A group already
        // taken off the queue is being listed as we speak and there is nothing
        // to promote.
        auto at = std::find(groupQueue_.begin(), groupQueue_.end(), g);
        if (at != groupQueue_.end()) {
            groupQueue_.erase(at);
            urgentGroups_.push_back(g);
            group.urgent = true;
        }
        return;
    }
    group.queued = true;
    group.urgent = urgent;
    (urgent ? urgentGroups_ : groupQueue_).push_back(g);
    groupRep_[g] = key(x, z);
    if (workerRunning_) {
        wake_.notify_one();
    }
}

// ------------------------------------------------------------- the writes --

bool ChunkCache::save(const ChunkColumn& column, SavePressure pressure)
{
    const i64 k = key(column.x, column.z);
    auto copy = std::make_shared<const ChunkColumn>(column.clone());

    // **Stored, not queued.** A finished column becomes the cache's answer for
    // that chunk immediately -- which is what the generator reads back and what
    // hasChunk reports -- but its bytes do not go to the card until something
    // asks for a flush: the autosave timer, the pause menu, or world exit.
    //
    // This is deliberately the shape a1.1.2 has, and it took an argument to get
    // here. Writing eagerly is *safer* than the original: population passes
    // spill across chunk borders, so a column generated and populated in memory
    // and then lost to a crash comes back regenerated against neighbours that
    // are already `terrainPopulated` and will not re-run their passes -- it
    // returns missing whatever they had put into it. But the original has that
    // hazard in a worse form. Its 1024-slot table is indexed by
    // `(x & 31, z & 31)`, so a chunk is only written when something 32 chunks
    // away collides with its slot, and can otherwise sit unwritten for an entire
    // session. What bounds the exposure here is the autosave interval, which is
    // the thing the interval is actually for.
    bool overCap = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        installColumn(k, std::move(copy), true, false);
        noteExists(k);
        unreadable_.erase(k);
        overCap = dirtyBytes_ > dirtyCapLocked();
    }

    // **The one thing that writes outside a save: running out of memory for
    // what is owed.** The generation worker can finish columns faster than any
    // interval collects them, and a dirty column cannot be evicted -- it is the
    // only copy of that world. So over the cap, whoever dirtied the column
    // writes one itself. That is back-pressure paid by the generation worker,
    // which is the thread that outran the card. **The main thread never reaches
    // it**, and that is now enforced by the caller passing SavePressure::Defer
    // rather than by this comment: the tick system made the renderer an edit
    // path, and a frame that stops for an SD write reads as a freeze.
    //
    // **This could not fire until it was measured.** `takeWriteLocked` takes
    // from `writeQueue_`, and nothing but `flush()` ever put anything on it --
    // so past the cap this loop found an empty queue, broke on the first
    // iteration, and the dirty set grew without any bound at all. Measured on
    // the host with the console's 45-second interval, `--fly <world> 8 6000
    // gen` peaked at **11.28 MB owed across 741 columns against a 4 MB cap**;
    // on an Old 3DS that is more heap than the whole world has to spend, and
    // what a player sees is generation stopping with nothing on the debug page
    // looking full. Queueing the oldest of them here is what closes it.
    if (pressure == SavePressure::Defer && overCap) {
        // Hand the oldest of what is owed to the I/O worker and go back to the
        // frame. Sitting over the cap for a few frames costs nothing.
        bool farOver = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            queueDirtyWritesLocked(dirtyCapLocked() / 2);
            if (workerRunning_) {
                wake_.notify_one();
            }
            farOver = dirtyBytes_ > deferCeilingLocked();
        }

        // **But deferring for ever is not an option, and this is the trap the
        // generation worker's back-pressure was written to avoid.** A dirty
        // column is the only copy of that world, so it cannot be evicted; if the
        // card cannot keep up with what is being dirtied, the dirty set is a
        // leak. It grew to 11.28 MB against a 4 MB cap once already, before
        // anything queued the writes.
        //
        // The edit path made that reachable again from the main thread, and
        // more so once relighting started marking a column dirty for every
        // section whose light moved. So: below the ceiling the I/O thread does
        // the work and the frame is free; above it, this thread pays for one
        // column. A hitch is worse than a smooth frame and far better than
        // running the console out of heap.
        if (!farOver) {
            return true;
        }
        Job job;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!takeWriteLocked(&job)) {
                return true;  // everything owed is already in somebody else's hands
            }
        }
        runJob(job);
        return true;
    }

    while (overCap) {
        Job job;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const usize cap = dirtyCapLocked();
            if (dirtyBytes_ <= cap) {
                break;
            }
            // Half the cap rather than the cap itself, so a run of columns does
            // not re-trigger this on every single one. The I/O thread takes
            // these too -- whichever thread gets there first -- so this is a
            // share of the work rather than all of it.
            queueDirtyWritesLocked(cap / 2);
            if (!takeWriteLocked(&job)) {
                break;  // everything owed is already in somebody else's hands
            }
        }
        runJob(job);
        std::lock_guard<std::mutex> guard(mutex_);
        overCap = dirtyBytes_ > dirtyCapLocked();
    }
    return true;
}

// **The oldest dirty columns, queued for writing until what nobody has taken
// yet is back under `downTo`.** mutex_ held.
//
// Oldest first for the same reason eviction is: the column the player is
// standing on is the one most likely to be dirtied again, and writing it twice
// costs two files. What this does *not* do is decide when a world is saved --
// that is the autosave timer, the pause menu and world exit. It is the memory
// backstop, and it runs only when the dirty set is over its cap.
void ChunkCache::queueDirtyWritesLocked(usize downTo)
{
    usize loose = 0;
    std::vector<std::pair<u64, i64>> candidates;
    for (const auto& [k, entry] : entries_) {
        if (!entry.dirty || entry.column == nullptr || entry.queuedWrite || entry.writing) {
            continue;
        }
        loose += entry.bytes;
        candidates.push_back({entry.used, k});
    }
    if (loose <= downTo) {
        return;  // what is over the cap is already on its way
    }

    std::sort(candidates.begin(), candidates.end());
    for (const auto& [used, k] : candidates) {
        if (loose <= downTo) {
            break;
        }
        Entry* entry = find(k);
        if (entry == nullptr) {
            continue;
        }
        entry->queuedWrite = true;
        writeQueue_.push_back(k);
        loose -= entry->bytes;
    }
    if (workerRunning_) {
        wake_.notify_all();
    }
}

void ChunkCache::flush(bool blocking)
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (auto& [k, entry] : entries_) {
            if (entry.dirty && !entry.queuedWrite && !entry.writing) {
                entry.queuedWrite = true;
                writeQueue_.push_back(k);
            }
        }
        if (workerRunning_) {
            wake_.notify_all();
        }
    }

    if (!blocking) {
        return;
    }

    if (!workerRunning_) {
        for (;;) {
            Job job;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!takeWriteLocked(&job) && !takeHousekeepingLocked(&job)) {
                    break;
                }
            }
            runJob(job);
        }
        return;
    }

    // **Writes, not every job.** This used to wait for `jobsActive_ == 0`,
    // which meant world exit also waited out every queued directory listing and
    // every read-ahead -- hundreds of SD operations that nobody will ever need
    // the answers to, in front of a player looking at "Saving level..". What a
    // flush owes is the dirty columns and level.dat, and that is what it waits
    // for now. close() throws the speculative work away first, so the I/O
    // thread is not doing any of it while this waits.
    std::unique_lock<std::mutex> guard(mutex_);
    drained_.wait(guard, [this] {
        return writeQueue_.empty() && !housekeepingPending_ && writesActive_ == 0;
    });
}

bool ChunkCache::idle() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    // Surveys count, because "the I/O thread has nothing left" is what this
    // says and a queued survey is something left. Nothing in the game waits on
    // it -- a flush waits on the writes, which is a different question and one
    // a picture of far-off ground has no business delaying.
    return readQueue_.empty() && writeQueue_.empty() && prefetchQueue_.empty()
           && groupQueue_.empty() && urgentGroups_.empty() && surveyQueue_.empty()
           && !housekeepingPending_ && jobsActive_ == 0;
}

void ChunkCache::applyPlayerState(const PlayerState& player)
{
    world::LevelData& level = storage_.level();
    if (player.entities) level.entities = player.entities;
    if (!player.valid) {
        return;
    }
    level.time = player.timeTicks;
    level.player.pos[0] = player.pos[0];
    level.player.pos[1] = player.pos[1];
    level.player.pos[2] = player.pos[2];
    level.player.rotation[0] = player.rotation[0];
    level.player.rotation[1] = player.rotation[1];
    // **Only when it was set**, which is what keeps a world opened in a mode
    // with no inventory from writing an empty one over what the real client
    // left there. See PlayerState::hasInventory.
    if (player.hasInventory) {
        level.player.inventory = player.inventory;
    }
    if (player.hasVitals) {
        level.player.health = player.health;
        level.player.hurtTime = player.hurtTime;
        level.player.deathTime = player.deathTime;
        level.player.attackTime = player.attackTime;
        level.player.air = player.air;
        level.player.fire = player.fire;
    }
    // A world a server made has no Player compound, and now somebody has stood
    // in it. Writing one is what the original client does the first time you
    // play such a world; what must not happen is inventing one for a world
    // nobody has entered, which is why `valid` gates the whole function.
    level.player.present = true;
}

void ChunkCache::requestHousekeeping(i64 nowMillis)
{
    requestHousekeeping(nowMillis, PlayerState{});
}

void ChunkCache::requestHousekeeping(i64 nowMillis, const PlayerState& player)
{
    if (!open_) {
        return;
    }
    bool runHere = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        housekeepingPending_ = true;
        housekeepingMillis_ = nowMillis;
        housekeepingPlayer_ = player;
        if (workerRunning_) {
            wake_.notify_one();
        } else {
            runHere = true;
        }
    }
    if (!runHere) {
        return;
    }

    // **Queued writes go first, and this loop is what makes that true when
    // there is no worker to enforce it.** Housekeeping ends in
    // `storage_.commit()`, which is what makes staged writes findable; running
    // it while a write is still in the queue commits the state *before* that
    // write and leaves it staged until the next cycle. Threaded, that ordering
    // is `takeJobLocked`'s -- writes outrank housekeeping. Unthreaded the job
    // runs here, so the same rule has to be spelled out.
    //
    // Writes only, not `takeJobLocked`: a read or a group listing pulled onto
    // the caller's thread is exactly the main-thread SD work this class exists
    // to remove, and neither has anything to do with what a commit covers.
    for (;;) {
        Job write;
        bool haveWrite = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            haveWrite = takeWriteLocked(&write);
        }
        if (!haveWrite) {
            break;
        }
        runJob(write);
    }

    Job job;
    bool have = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        have = takeHousekeepingLocked(&job);
    }
    if (have) {
        runJob(job);
    }
}

bool ChunkCache::takeHousekeepingLocked(Job* out)
{
    if (!housekeepingPending_) {
        return false;
    }
    housekeepingPending_ = false;
    *out = Job{JobKind::Housekeeping, 0, 0, false, housekeepingMillis_};
    pendingPlayer_ = housekeepingPlayer_;
    ++jobsActive_;
    ++writesActive_;
    return true;
}

ChunkCache::Stats ChunkCache::stats() const
{
    std::lock_guard<std::mutex> guard(mutex_);

    // **Answered now, not as of the last pump.** The byte and column figures
    // used to be published by `recountLocked` at the end of `pump()`, because
    // producing the column counts meant walking the whole table and that was
    // not something to do on every read. They are maintained incrementally now,
    // so the lock this already holds is enough to answer with the truth --
    // which matters for `close()`, whose save loop reads `dirtyColumns` to
    // decide when the world is written out.
    Stats out = stats_;
    out.cleanBytes = cleanBytes_;
    out.dirtyBytes = dirtyBytes_;
    out.dirtyCapBytes = dirtyCap_;
    out.cleanColumns = cleanColumns_;
    out.dirtyColumns = dirtyColumns_;
    out.readsQueued = u32(readQueue_.size());
    out.writesQueued = u32(writeQueue_.size());
    out.groupsQueued = u32(groupQueue_.size() + urgentGroups_.size());
    out.workerRunning = workerRunning_;
    return out;
}

void ChunkCache::pump()
{
    if (!workerRunning_) {
        // Unthreaded, the queues would otherwise only move at flush time. One
        // unit a call keeps a harness making progress without turning pump()
        // into an unbudgeted drain.
        Job job;
        bool have = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            have = takeJobLocked(&job);
        }
        if (have) {
            runJob(job);
        }
    }

    // No recount here any more. Everything `stats()` reports is either
    // maintained incrementally or read off a queue when asked, so the walk of
    // the whole table this used to end with -- under the lock, once per frame,
    // for two numbers on a debug page -- is gone.
}

// ------------------------------------------------------------ generation --

const ChunkColumn* ChunkCache::load(i32 x, i32 z, ChunkColumn* scratch)
{
    const i64 k = key(x, z);
    {
        std::lock_guard<std::mutex> guard(mutex_);
        Entry* entry = find(k);
        if (entry != nullptr && entry->column != nullptr) {
            *scratch = entry->column->clone();
            entry->used = ++clock_;
            ++stats_.hits;
            return scratch;
        }
        bool exists = false;
        if (groupSays(k, &exists) && !exists) {
            // The world genuinely does not have it, and the index says so
            // without a file operation. This is the common answer at the
            // frontier and it used to be a failed open.
            return nullptr;
        }
        ++stats_.misses;
    }

    // A miss reads here rather than waiting on the I/O thread. The generation
    // worker may block, and on a New 3DS it is on core 2 while the I/O thread is
    // a low-priority thread on core 0 -- handing it the job would trade a read
    // for a wait on a thread that only runs in the main thread's slack.
    return readThrough(k, scratch) ? scratch : nullptr;
}

// ------------------------------------------------------------- internals --

void ChunkCache::installColumn(i64 k, std::shared_ptr<const ChunkColumn> column, bool dirty,
                               bool prefetched)
{
    Entry& entry = entries_[k];
    if (entry.column != nullptr) {
        (entry.dirty ? dirtyBytes_ : cleanBytes_) -= entry.bytes;
        --(entry.dirty ? dirtyColumns_ : cleanColumns_);
    }
    entry.bytes = column->memoryUsage();
    entry.column = std::move(column);
    entry.used = ++clock_;
    ++entry.version;
    entry.dirty = dirty;
    entry.prefetched = prefetched;
    (dirty ? dirtyBytes_ : cleanBytes_) += entry.bytes;
    ++(dirty ? dirtyColumns_ : cleanColumns_);
}

void ChunkCache::evictLocked()
{
    if (cleanBytes_ <= config_.cleanCapBytes) {
        return;
    }
    // One scan per burst rather than an intrusive LRU list: the table holds a
    // few hundred entries, the scan is a comparison each, and the alternative is
    // a heap allocation per insertion on a console whose newlib heap we would
    // rather not fragment.
    std::vector<std::pair<u64, i64>> candidates;
    candidates.reserve(entries_.size());
    for (const auto& [k, entry] : entries_) {
        if (entry.column != nullptr && !entry.dirty && !entry.writing) {
            candidates.push_back({entry.used, k});
        }
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [used, k] : candidates) {
        if (cleanBytes_ <= config_.cleanCapBytes) {
            break;
        }
        auto it = entries_.find(k);
        if (it == entries_.end()) {
            continue;
        }
        cleanBytes_ -= it->second.bytes;
        --cleanColumns_;
        entries_.erase(it);
        ++stats_.evicted;
    }
}

// **What may be owed to the card, from what the heap has left.**
//
// The fixed cap has to be chosen for the worst case -- the longest render
// distance, the generator's cache at full size, block data at its peak -- and a
// number chosen for the worst case is wrong for every other one. Past it the
// generation worker stops and writes a chunk file itself, which is the right
// back-pressure when memory is genuinely short and a stall for nothing when
// twelve megabytes are sitting free.
//
// So the cap follows the free heap, between the configured floor and ceiling:
//
//   * **half of what is spare, not all of it.** The other half is the grid
//     still growing as the player moves and the mesher's staging buffers; a cap
//     that claimed everything free would push the heap to its limit and the
//     thing that fails there is an allocation, not a write.
//   * **less a reserve**, because "free" is measured now and the columns the
//     dirty set is about to hold are 18 KB each.
//
// **Asked on whichever thread dirtied the column, and never on the render
// thread.** `mallinfo` walks the allocator's free lists and takes the malloc
// lock; once per generated column -- tens of milliseconds apart -- is nothing,
// once per frame on the thread that has 33 ms to spend would be the same
// species of mistake as the `stat` this class exists to remove. So the answer
// is cached in `dirtyCap_` and the debug page reads that.
//
// It is only ever a hint -- see core/util/memory.hpp. Being wrong costs a write
// that was not needed or one that was needed a little later, and neither is a
// correctness problem: the autosave timer still governs, and close() still
// writes everything.
usize ChunkCache::dirtyCapLocked()
{
    dirtyCap_ = config_.dirtyCapBytes;
    if (config_.dirtyCapMaxBytes <= config_.dirtyCapBytes) {
        return dirtyCap_;
    }
    const usize free = heapFreeBytes();
    if (free == 0) {
        return dirtyCap_;  // no answer from the platform
    }

    constexpr usize kReserve = 2u << 20;
    const usize spare = free > kReserve ? free - kReserve : 0;
    usize cap = spare / 2;
    if (cap > config_.dirtyCapMaxBytes) {
        cap = config_.dirtyCapMaxBytes;
    }
    if (cap > dirtyCap_) {
        dirtyCap_ = cap;
    }
    return dirtyCap_;
}

// How far past the cap the deferred path will let the dirty set drift before it
// starts paying for writes itself. Twice the cap: far enough that an ordinary
// burst of edits never reaches it and the frame stays clean, near enough that
// the set cannot quietly become the largest thing in the heap.
usize ChunkCache::deferCeilingLocked()
{
    const usize cap = dirtyCapLocked();
    const usize ceiling = cap * 2;
    return ceiling > config_.dirtyCapMaxBytes ? config_.dirtyCapMaxBytes : ceiling;
}

void ChunkCache::debugCountColumns(u32* clean, u32* dirty)
{
    std::lock_guard<std::mutex> guard(mutex_);
    countColumnsLocked(clean, dirty);
}

void ChunkCache::countColumnsLocked(u32* clean, u32* dirty) const
{
    *clean = 0;
    *dirty = 0;
    for (const auto& [k, entry] : entries_) {
        (void)k;
        if (entry.column == nullptr) {
            continue;
        }
        ++(entry.dirty ? *dirty : *clean);
    }
}

bool ChunkCache::takeWriteLocked(Job* out)
{
    while (!writeQueue_.empty()) {
        const i64 k = writeQueue_.front();
        writeQueue_.erase(writeQueue_.begin());
        Entry* entry = find(k);
        if (entry == nullptr || !entry->dirty || entry->column == nullptr) {
            continue;  // taken, or written by an earlier pass over the queue
        }
        entry->queuedWrite = false;
        entry->writing = true;
        *out = Job{JobKind::Write, k, 0, false};
        ++jobsActive_;
        ++writesActive_;
        return true;
    }
    return false;
}

// Reads, then the listings something is waiting on, then writes, then the
// listings nobody is waiting on yet, then reading ahead. The order is the whole
// reason there are five queues: a hundred prefetches must never sit in front of
// the column the player is standing on, and a write must never sit behind them.
//
// **An urgent listing goes ahead of the writes**, and that is the half of the
// frame-stall fix that lives on this thread. A cell whose group is not listed
// is a cell the streamer cannot classify, so it is neither loaded nor generated
// until the listing lands -- and behind a flush it was landing after two
// hundred chunk writes, each one an encode, a deflate and an fsync. A listing
// is one directory read. Writes are deferred work with a memory cap behind
// them and they can wait for it; if the wait ever costs anything, it costs the
// generation worker a write it pays for itself, which is what the cap is for.
//
// The speculative ring stays behind the writes, because nothing is waiting on
// it by definition: a group warmed ahead of the player that turns out to be
// needed is asked for again, urgently, by the cell that needs it.
bool ChunkCache::takeJobLocked(Job* out)
{
    if (!readQueue_.empty()) {
        const i64 k = readQueue_.front();
        readQueue_.erase(readQueue_.begin());
        *out = Job{JobKind::Read, k, 0, false};
        ++jobsActive_;
        return true;
    }
    if (!urgentGroups_.empty()) {
        const u64 g = urgentGroups_.front();
        urgentGroups_.erase(urgentGroups_.begin());
        *out = Job{JobKind::List, groupRep_[g], g, false};
        ++jobsActive_;
        return true;
    }
    if (takeWriteLocked(out)) {
        return true;
    }
    if (takeHousekeepingLocked(out)) {
        return true;
    }
    if (!groupQueue_.empty()) {
        const u64 g = groupQueue_.front();
        groupQueue_.erase(groupQueue_.begin());
        *out = Job{JobKind::List, groupRep_[g], g, false};
        ++jobsActive_;
        return true;
    }
    if (!prefetchQueue_.empty()) {
        const i64 k = prefetchQueue_.front();
        prefetchQueue_.erase(prefetchQueue_.begin());
        *out = Job{JobKind::Read, k, 0, true};
        ++jobsActive_;
        return true;
    }
    // **Last, under the read-ahead ring.** A survey is a picture of ground
    // nobody is standing in; every column the world itself is owed, every write
    // and every listing goes first. See survey().
    if (!surveyQueue_.empty() && surveyor_ != nullptr) {
        const i64 k = surveyQueue_.front();
        surveyQueue_.erase(surveyQueue_.begin());
        surveyQueued_.erase(k);
        *out = Job{JobKind::Survey, k, 0, false};
        ++jobsActive_;
        return true;
    }
    return false;
}

// **The card being modelled, and nothing else in this class knows about it.**
// Called immediately before the storage lock is taken, so a thread waiting for
// that lock waits the way it would on a console.
void ChunkCache::payOpLatency() const
{
    if (config_.opLatencyMicros == 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(config_.opLatencyMicros));
}

bool ChunkCache::readThrough(i64 k, ChunkColumn* out)
{
    bool ok = false;
    {
        payOpLatency();
        std::lock_guard<std::mutex> guard(storageLock_);
        ok = storage_.loadChunk(keyX(k), keyZ(k), out);
    }
    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.reads;
    if (ok) {
        noteExists(k);
    }
    return ok;
}

// **One exit for every job, because the one that did not have it hung the
// game.**
//
// `flush(true)` waits for what is owed to reach the card, and it waits on
// `drained_`. Read and List completions used to decrement the counter without
// notifying: any listing or read-ahead that finished *after* the last write had
// left nothing to do meant the predicate became true with nobody left to say
// so, and the waiter slept for ever. That is the "Saving level.." screen never
// going away -- reported from hardware, and made far likelier by listings being
// prioritised, because at world exit there are usually hundreds of them queued.
//
// **mutex_ held.**
void ChunkCache::finishJobLocked(JobKind kind)
{
    --jobsActive_;
    if (kind == JobKind::Write || kind == JobKind::Housekeeping) {
        --writesActive_;
    }
    drained_.notify_all();
}

void ChunkCache::runJob(const Job& job)
{
    switch (job.kind) {
    case JobKind::Read: {
        auto column = std::make_unique<ChunkColumn>(keyX(job.chunk), keyZ(job.chunk));
        const bool ok = readThrough(job.chunk, column.get());
        std::lock_guard<std::mutex> guard(mutex_);
        inFlight_.erase(job.chunk);
        if (!ok && !job.prefetch) {
            // Classified as present and unreadable now. Recorded so the caller
            // is told once instead of asking again every frame. A prefetch that
            // fails says nothing -- it was speculative, and the chunk may
            // simply not be there.
            unreadable_.insert(job.chunk);
        }
        if (ok && find(job.chunk) == nullptr) {
            installColumn(job.chunk, std::shared_ptr<const ChunkColumn>(column.release()),
                          false, job.prefetch);
            evictLocked();
        }
        finishJobLocked(job.kind);
        break;
    }

    case JobKind::Survey: {
        // **The visitor, on this thread and with no lock of ours held.** It is
        // handed a column that belongs to the cache for the length of the call
        // -- either the one already retained, or the single scratch this thread
        // reads into -- and nothing is installed or evicted either way. See
        // survey().
        SurveyFn visit = nullptr;
        void* context = nullptr;
        std::shared_ptr<const ChunkColumn> held;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            visit = surveyor_;
            context = surveyorContext_;
            Entry* entry = find(job.chunk);
            if (entry != nullptr && entry->column != nullptr) {
                held = entry->column;
                entry->used = ++clock_;
                ++stats_.hits;
            }
        }
        if (visit == nullptr) {
            // Cancelled between being queued and being taken, which is a world
            // closing under it.
            std::lock_guard<std::mutex> guard(mutex_);
            finishJobLocked(job.kind);
            break;
        }
        if (held != nullptr) {
            visit(context, keyX(job.chunk), keyZ(job.chunk), held.get());
            std::lock_guard<std::mutex> guard(mutex_);
            finishJobLocked(job.kind);
            break;
        }
        if (surveyScratch_ == nullptr) {
            surveyScratch_ = std::make_unique<ChunkColumn>(keyX(job.chunk), keyZ(job.chunk));
        }
        const bool read = readThrough(job.chunk, surveyScratch_.get());
        visit(context, keyX(job.chunk), keyZ(job.chunk), read ? surveyScratch_.get() : nullptr);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            finishJobLocked(job.kind);
        }
        break;
    }

    case JobKind::Write: {
        std::shared_ptr<const ChunkColumn> column;
        u32 version = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            Entry* entry = find(job.chunk);
            if (entry == nullptr || entry->column == nullptr) {
                if (entry != nullptr) {
                    entry->writing = false;
                }
                finishJobLocked(job.kind);
                break;
            }
            column = entry->column;
            version = entry->version;
        }

        // The encode and the deflate happen here, off both the generation worker
        // and the render thread. The column is shared and immutable, so nothing
        // has to be locked while 46 KB of NBT is built and compressed.
        bool ok = false;
        {
            payOpLatency();
            std::lock_guard<std::mutex> guard(storageLock_);
            ok = storage_.saveChunk(*column);
        }

        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.writes;
        Entry* entry = find(job.chunk);
        if (entry != nullptr) {
            entry->writing = false;
            if (ok && entry->version == version) {
                // Still the version that was written. It becomes an ordinary
                // retained column, and its bytes move from the dirty budget to
                // the clean one.
                entry->dirty = false;
                dirtyBytes_ -= entry->bytes;
                cleanBytes_ += entry->bytes;
                --dirtyColumns_;
                ++cleanColumns_;
                evictLocked();
            } else if (!entry->queuedWrite) {
                // Saved again while this was in flight, or the write failed.
                // Either way the card does not hold what the cache holds.
                entry->queuedWrite = true;
                writeQueue_.push_back(job.chunk);
            }
        }
        finishJobLocked(job.kind);
        break;
    }

    case JobKind::Housekeeping: {
        PlayerState player;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            player = pendingPlayer_;
        }
        {
            payOpLatency();
            std::lock_guard<std::mutex> guard(storageLock_);
            applyPlayerState(player);
            storage_.saveLevel();
            // Makes every chunk written since the last one findable. Free for
            // the folder backend, three ordered writes for a packed region --
            // and doing it here rather than per chunk is what keeps packed
            // mode's saves cheaper than the format it replaces.
            storage_.commit();
            // The original re-reads the lock on every chunk save; we do not,
            // because a console cannot run two copies of the game at once and
            // it would double the operations on the hottest path there is.
            // Refreshing it on the autosave timer is what docs/world-format.md
            // says to do instead, and before this it was said and not done.
            storage_.refreshLock(job.nowMillis);
        }
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.writes;
        finishJobLocked(job.kind);
        break;
    }

    case JobKind::List: {
        std::vector<i64> found;
        GroupScan scan{&found};
        {
            payOpLatency();
            std::lock_guard<std::mutex> guard(storageLock_);
            storage_.listChunkGroup(keyX(job.chunk), keyZ(job.chunk), &scan, &collectChunk);
        }

        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.listings;
        Group& group = groups_[job.group];
        // Merged rather than assigned: anything save() recorded while the
        // listing was in flight is in the world too, and dropping it would make
        // hasChunk deny a chunk that exists -- which is the one answer that
        // would change what gets generated.
        for (const i64 k : found) {
            auto at = std::lower_bound(group.chunks.begin(), group.chunks.end(), k);
            if (at == group.chunks.end() || *at != k) {
                group.chunks.insert(at, k);
            }
        }
        group.listed = true;
        group.queued = false;
        group.urgent = false;
        finishJobLocked(job.kind);
        break;
    }
    }
}

// ---------------------------------------------------------------- worker --

void ChunkCache::workerEntry(void* self)
{
    static_cast<ChunkCache*>(self)->workerMain();
}

bool ChunkCache::startWorker()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        workerStop_ = false;
        workerRunning_ = true;
    }

    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformWorker_ = workerSpawn()(&ChunkCache::workerEntry, this, WorkerRole::Io);
        if (platformWorker_ == nullptr) {
            std::lock_guard<std::mutex> guard(mutex_);
            workerRunning_ = false;
            return false;
        }
        return true;
    }

    worker_ = std::thread([this] { workerMain(); });
    return true;
}

void ChunkCache::stopWorker()
{
    if (platformWorker_ == nullptr && !worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        workerStop_ = true;
    }
    wake_.notify_all();
    if (platformWorker_ != nullptr) {
        workerJoin()(platformWorker_);
        platformWorker_ = nullptr;
    } else {
        worker_.join();
    }
    std::lock_guard<std::mutex> guard(mutex_);
    workerRunning_ = false;
}

void ChunkCache::workerMain()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> guard(mutex_);
            wake_.wait(guard, [this] {
                return workerStop_ || !readQueue_.empty() || !writeQueue_.empty()
                       || !groupQueue_.empty() || !urgentGroups_.empty()
                       || !prefetchQueue_.empty() || !surveyQueue_.empty()
                       || housekeepingPending_;
            });
            if (workerStop_) {
                return;
            }
            if (!takeJobLocked(&job)) {
                continue;
            }
        }
        runJob(job);
    }
}

}  // namespace mc::world
