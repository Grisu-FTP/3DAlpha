#include "core/world/chunk_cache.hpp"

#include "core/util/worker.hpp"

#include <algorithm>
#include <chrono>

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
    groups_.clear();
    readQueue_.clear();
    writeQueue_.clear();
    prefetchQueue_.clear();
    groupQueue_.clear();
    inFlight_.clear();
    unreadable_.clear();
    housekeepingPending_ = false;
    cleanBytes_ = 0;
    dirtyBytes_ = 0;
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
    flush(true);
    stopWorker();
    // After the thread is joined, so this is single-threaded again and the
    // level can be edited in place. storage_.close() rewrites level.dat, which
    // is what carries the position out.
    applyPlayerState(player);
    storage_.close(nowMillis);
    entries_.clear();
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
    const u32 g = storage_.chunkGroupKey(keyX(k), keyZ(k));
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

void ChunkCache::warmGroup(i32 x, i32 z)
{
    const u32 g = storage_.chunkGroupKey(x, z);
    std::lock_guard<std::mutex> guard(mutex_);
    Group& group = groups_[g];
    if (group.listed || group.queued) {
        return;
    }
    group.queued = true;
    groupQueue_.push_back(g);
    groupRep_[g] = key(x, z);
    if (workerRunning_) {
        wake_.notify_one();
    }
}

// ------------------------------------------------------------- the writes --

bool ChunkCache::save(const ChunkColumn& column)
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
        overCap = dirtyBytes_ > config_.dirtyCapBytes;
    }

    // **The one thing that writes outside a save: running out of memory for
    // what is owed.** The generation worker can finish columns faster than any
    // interval collects them, and a dirty column cannot be evicted -- it is the
    // only copy of that world. So over the cap, whoever dirtied the column
    // writes one itself. That is back-pressure paid by the generation worker,
    // which is the thread that outran the card; it is never the main thread
    // today and must not become it -- an edit path reaching here would want to
    // give up frame budget instead.
    while (overCap) {
        Job job;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (dirtyBytes_ <= config_.dirtyCapBytes || !takeWriteLocked(&job)) {
                break;
            }
        }
        runJob(job);
        std::lock_guard<std::mutex> guard(mutex_);
        overCap = dirtyBytes_ > config_.dirtyCapBytes;
    }
    return true;
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

    std::unique_lock<std::mutex> guard(mutex_);
    drained_.wait(guard, [this] {
        return writeQueue_.empty() && !housekeepingPending_ && jobsActive_ == 0;
    });
}

bool ChunkCache::idle() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return readQueue_.empty() && writeQueue_.empty() && prefetchQueue_.empty()
           && groupQueue_.empty() && !housekeepingPending_ && jobsActive_ == 0;
}

void ChunkCache::applyPlayerState(const PlayerState& player)
{
    if (!player.valid) {
        return;
    }
    world::LevelData& level = storage_.level();
    level.time = player.timeTicks;
    level.player.pos[0] = player.pos[0];
    level.player.pos[1] = player.pos[1];
    level.player.pos[2] = player.pos[2];
    level.player.rotation[0] = player.rotation[0];
    level.player.rotation[1] = player.rotation[1];
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
    return true;
}

ChunkCache::Stats ChunkCache::stats() const
{
    std::lock_guard<std::mutex> guard(mutex_);
    return stats_;
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

    std::lock_guard<std::mutex> guard(mutex_);
    recountLocked();
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
    }
    entry.bytes = column->memoryUsage();
    entry.column = std::move(column);
    entry.used = ++clock_;
    ++entry.version;
    entry.dirty = dirty;
    entry.prefetched = prefetched;
    (dirty ? dirtyBytes_ : cleanBytes_) += entry.bytes;
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
        entries_.erase(it);
        ++stats_.evicted;
    }
}

void ChunkCache::recountLocked()
{
    stats_.cleanBytes = cleanBytes_;
    stats_.dirtyBytes = dirtyBytes_;
    stats_.readsQueued = u32(readQueue_.size());
    stats_.writesQueued = u32(writeQueue_.size());
    stats_.groupsQueued = u32(groupQueue_.size());
    stats_.workerRunning = workerRunning_;

    u32 clean = 0;
    u32 dirty = 0;
    for (const auto& [k, entry] : entries_) {
        if (entry.column == nullptr) {
            continue;
        }
        (entry.dirty ? dirty : clean) += 1;
    }
    stats_.cleanColumns = clean;
    stats_.dirtyColumns = dirty;
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
        return true;
    }
    return false;
}

// Reads first, then writes, then reading ahead. The order is the whole reason
// there are three queues: a hundred prefetches must never sit in front of the
// column the player is standing on, and a write must never sit behind them.
bool ChunkCache::takeJobLocked(Job* out)
{
    if (!readQueue_.empty()) {
        const i64 k = readQueue_.front();
        readQueue_.erase(readQueue_.begin());
        *out = Job{JobKind::Read, k, 0, false};
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
        const u32 g = groupQueue_.front();
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
    return false;
}

bool ChunkCache::readThrough(i64 k, ChunkColumn* out)
{
    bool ok = false;
    {
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
        --jobsActive_;
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
                --jobsActive_;
                drained_.notify_all();
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
                evictLocked();
            } else if (!entry->queuedWrite) {
                // Saved again while this was in flight, or the write failed.
                // Either way the card does not hold what the cache holds.
                entry->queuedWrite = true;
                writeQueue_.push_back(job.chunk);
            }
        }
        --jobsActive_;
        drained_.notify_all();
        break;
    }

    case JobKind::Housekeeping: {
        PlayerState player;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            player = pendingPlayer_;
        }
        {
            std::lock_guard<std::mutex> guard(storageLock_);
            applyPlayerState(player);
            storage_.saveLevel();
            // The original re-reads the lock on every chunk save; we do not,
            // because a console cannot run two copies of the game at once and
            // it would double the operations on the hottest path there is.
            // Refreshing it on the autosave timer is what docs/world-format.md
            // says to do instead, and before this it was said and not done.
            storage_.refreshLock(job.nowMillis);
        }
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.writes;
        --jobsActive_;
        drained_.notify_all();
        break;
    }

    case JobKind::List: {
        std::vector<i64> found;
        GroupScan scan{&found};
        {
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
        --jobsActive_;
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
                       || !groupQueue_.empty() || !prefetchQueue_.empty()
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
