#include "core/render/world_streamer.hpp"

#include "core/util/worker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace mc::render {

namespace {

int floorMod(i32 value, int modulus)
{
    const int r = int(value % modulus);
    return r < 0 ? r + modulus : r;
}

}  // namespace

bool WorldStreamer::open(const char* worldDir, int meshDistance, i64 nowMillis)
{
    cache_.configure(cacheConfig_);
    if (cache_.open(worldDir, nowMillis) != world::OpenResult::Ok) {
        return false;
    }

    path_ = worldDir;
    level_ = cache_.level();
    lastSaveMillis_ = nowMillis;
    open_ = true;

    meshDistance_ = meshDistance;
    loadRadius_ = meshDistance + 1;
    buildGrid();

    if (generateMissing_) {
        mcver::WorldGenOptions options;
        options.snowCovered = level_.snowCovered;

        mcver::ChunkGenerator::Store store;
        store.context = this;
        store.load = &WorldStreamer::generatorLoad;
        store.deliver = &WorldStreamer::generatorDeliver;

        // Sized to the load radius, because what the generator has to hold at
        // once is two rings of unfinished frontier and that band grows with the
        // radius being filled. Undersizing it is not a slow path but a wrong
        // world -- see ChunkGenerator::cacheColumnsFor.
        generator_ = std::make_unique<mcver::ChunkGenerator>(
            level_.randomSeed, options, store,
            mcver::ChunkGenerator::cacheColumnsFor(loadRadius_));
        generated_ = std::make_unique<world::ChunkColumn>();

        if (generationThreaded_ && !startWorker()) {
            // Not fatal. Generating on the main thread is what this class did
            // before the worker existed: it stutters badly, and it is still
            // better than a world that stops at the edge of what is on the
            // card. `Stats::workerRunning` is how a caller finds out.
        }
    }

    builder_.reserveQuads(4096);
    return true;
}

// ChunkGenerator::Store. The generator asks the world what is already there and
// hands back what it makes; both go through the same storage slot the streamer
// reads from, so a generated chunk is on the SD card before anything else can
// ask for it.
const world::ChunkColumn* WorldStreamer::generatorLoad(void* context, i32 chunkX, i32 chunkZ,
                                                       world::ChunkColumn* scratch)
{
    WorldStreamer& self = *static_cast<WorldStreamer*>(context);

    // **The cache, and only the cache.** An earlier version answered out of the
    // resident grid, which is faster and became wrong the moment this started
    // running on another thread: the grid is the main thread's, and it changes
    // under the player's feet.
    //
    // ChunkCache is authoritative in the way storage was -- every column the
    // generator finishes is stored here before it is handed over, and a read
    // returns byte-identical content whether it comes from the table or the
    // card -- so nothing about what the generator sees has changed. What has
    // changed is the cost: a column the sweep wrote a moment ago used to be a
    // deflate, a file write and a read back, and is now a clone.
    return self.cache_.load(chunkX, chunkZ, scratch);
}

void WorldStreamer::generatorDeliver(void* context, world::ChunkColumn& column)
{
    WorldStreamer& self = *static_cast<WorldStreamer*>(context);

    // **Saving is not optional.** The generator evicts what it has handed over
    // and reaches it again through load(); a column that was never stored would
    // come back as bare terrain with every neighbour's population missing, and
    // the world would be quietly wrong rather than obviously so.
    //
    // "Stored" now means the cache rather than the card -- the write follows on
    // the I/O thread -- and that is the same guarantee, because the cache is
    // what load() reads and it answers with this column from here on.
    self.cache_.save(column);

    // Then the grid gets it, and **not from here**: this runs on the worker,
    // and the grid, the renderer and the visibility masks are the main
    // thread's. The column is queued and adopted in drainGenerated().
    auto owned = std::make_unique<world::ChunkColumn>(std::move(column));
    {
        std::lock_guard<std::mutex> guard(self.queueLock_);
        self.finished_.push_back(std::move(owned));
    }
}

void WorldStreamer::workerEntry(void* self)
{
    static_cast<WorldStreamer*>(self)->workerMain();
}

bool WorldStreamer::startWorker()
{
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        workerStop_ = false;
        workerRunning_ = true;
    }

    // The platform gets first refusal, because it is the only one that can say
    // "core 2". See core/util/worker.hpp.
    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformWorker_ =
            workerSpawn()(&WorldStreamer::workerEntry, this, WorkerRole::Generation);
        if (platformWorker_ == nullptr) {
            std::lock_guard<std::mutex> guard(queueLock_);
            workerRunning_ = false;
            return false;
        }
        return true;
    }

    worker_ = std::thread([this] { workerMain(); });
    return true;
}

// Blocks until the worker has nothing in hand. Only for the few main-thread
// operations that reach into the generator itself -- growing its cache while it
// is walking its own entry table would move the table under it.
void WorldStreamer::waitForWorkerIdle()
{
    if (!workerRunning_) {
        return;
    }
    std::unique_lock<std::mutex> guard(queueLock_);
    // **Pause first, then wait.** The worker takes its own next job the moment
    // it finishes one, so waiting for "not busy" without stopping it handing
    // itself more work is a wait that can never end on a full queue.
    queuePaused_ = true;
    idle_.wait(guard, [this] { return !jobActive_; });
}

void WorldStreamer::resumeGeneration()
{
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        queuePaused_ = false;
    }
    wake_.notify_one();
}

void WorldStreamer::stopWorker()
{
    if (platformWorker_ == nullptr && !worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        workerStop_ = true;
    }
    wake_.notify_all();
    if (platformWorker_ != nullptr) {
        workerJoin()(platformWorker_);
        platformWorker_ = nullptr;
    } else {
        worker_.join();
    }
    std::lock_guard<std::mutex> guard(queueLock_);
    workerRunning_ = false;
}

// The worker's whole life: take the nearest column that is owed, make it,
// repeat.
//
// **It takes its own next job**, and that is the whole reason this loop looks
// the way it does. The main thread used to hand one out per `update()`, which
// capped generation at one column per rendered frame -- thirty a second at
// thirty frames a second -- however fast the worker actually was. On a New 3DS
// the worker is on core 2 with nothing else on it and can beat that, and every
// column it could have made and did not is a frame the frontier stays empty.
//
// What does *not* change is the rule: one column at a time, nearest to the
// camera. The realised sequence does change, in the sense that finishing three
// columns between two frames means the camera moved less between the picks --
// which is the same speed-dependence a faster console already has and which
// nearest-first accepted deliberately. See the note on slate_.
//
// It holds no lock while generating, which is the point -- the main thread must
// never wait on anything longer than a pointer swap. What it does share is the
// storage slot, and that is taken and released once per chunk file inside
// generatorLoad and generatorDeliver.
void WorldStreamer::workerMain()
{
    for (;;) {
        std::pair<i32, i32> at{0, 0};
        {
            std::unique_lock<std::mutex> guard(queueLock_);
            wake_.wait(guard, [this] {
                return workerStop_ || (!queuePaused_ && !slate_.empty());
            });
            if (workerStop_) {
                jobActive_ = false;
                return;
            }
            takeSlateLocked(&at);
            inFlight_ = at;
            jobActive_ = true;
        }

        generateColumn(at.first, at.second);

        // The generator's own counters are published here rather than read
        // from the main thread. They belong to the worker, which is the only
        // thread allowed inside the generator at all, and "it is only a
        // counter" is not a reason to leave a read unsynchronised -- the
        // thread sanitizer named this one before it could become a habit.
        std::lock_guard<std::mutex> guard(queueLock_);
        workerPeakLive_ = generator_->stats().peakLive;
        workerEvictedLive_ = generator_->stats().evictedLive;
        // The coordinate goes back to the main thread, which keeps it off the
        // slate until drainGenerated has put the column in the grid. A list
        // rather than a single slot, because more than one column can finish
        // between two frames -- which is the point of the worker taking its own
        // jobs.
        completed_.push_back(at);
        jobActive_ = false;
        idle_.notify_all();
    }
}

bool WorldStreamer::generationIdle() const
{
    // **A cell nobody has been able to ask about yet counts as outstanding**,
    // because it may be about to become a column that is owed. Without this a
    // caller can see an idle generator on the first frame of a world -- nothing
    // classified, so nothing owed, so nothing to do -- and conclude the world
    // has settled before a single question has been answered.
    //
    // That is not hypothetical. It is what made the three-arm world test
    // flaky the moment classification could be deferred: the harness settles at
    // each waypoint, and at the first one it moved on having generated nothing
    // at all. Every caller of this asks the same question -- the tests, the
    // `--fly` settle point, the console's loading screen -- so it belongs here
    // rather than in each of them.
    if (stats_.unclassified != 0) {
        return false;
    }
    std::lock_guard<std::mutex> guard(queueLock_);
    return slate_.empty() && !jobActive_ && finished_.empty() && completed_.empty();
}

// Takes what the worker finished into the grid. Main thread, once a frame, and
// **before the scan that rewrites the slate** -- which is what makes it safe to
// clear `completed_` here: a coordinate leaves that list and the column lands in
// the grid in the same call, so there is no moment where a finished column looks
// like one that is still owed.
//
// A result may be for a column that has since gone out of range -- the player
// kept walking while it was being made -- and dropping it costs nothing,
// because it was written to the card before it was handed back.
void WorldStreamer::drainGenerated(ChunkRenderer& renderer)
{
    std::vector<std::unique_ptr<world::ChunkColumn>> batch;
    std::vector<std::pair<i32, i32>> done;
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        batch.swap(finished_);
        done.swap(completed_);
        stats_.generatorPeakLive = workerPeakLive_;
        stats_.generatorEvictedLive = workerEvictedLive_;
        stats_.generationFailures = generationFailures_;
    }
    if (batch.empty()) {
        return;
    }
    stats_.generatedThisFrame = int(batch.size());

    for (auto& column : batch) {
        if (column == nullptr) {
            continue;
        }
        const i32 cx = column->x;
        const i32 cz = column->z;
        if (!centreSet_ || std::abs(cx - centreX_) > loadRadius_
            || std::abs(cz - centreZ_) > loadRadius_) {
            continue;
        }
        Cell& cell = cells_[cellIndex(cx, cz)];
        if (cell.state == CellState::Loaded && cell.chunkX == cx && cell.chunkZ == cz) {
            continue;  // already here; the card answered first
        }
        if (cell.state != CellState::Empty && (cell.chunkX != cx || cell.chunkZ != cz)) {
            dropCell(cell, renderer);
        }
        adoptColumn(cell, std::move(column));
        ++stats_.adoptedThisFrame;
    }

    // A generated column arrives with its neighbours, so the nine-cell
    // republish the load path does is not enough. Once over the grid is cheap
    // next to what was just generated, and only on a frame that adopted.
    if (stats_.adoptedThisFrame > 0) {
        for (const Offset& offset : spiral_) {
            publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
        }
    }
}

void WorldStreamer::buildGrid()
{
    // Three rings wider than the load radius, which is a sweep's reach -- but
    // only when there is something that sweeps. With generation off the extra
    // ring would be cells that are classified and never used, and the harnesses
    // that measure a fixed world would be paying for a question they never ask.
    gridRadius_ = loadRadius_ + (generateMissing_ ? 3 : 0);
    edge_ = gridRadius_ * 2 + 1;
    cells_.clear();
    cells_.resize(usize(edge_) * edge_);

    // Nearest first, so the columns under the player's feet arrive before the
    // ones at the horizon. Ordering it once here is what keeps the per-frame
    // load step to a scan rather than a search.
    spiral_.clear();
    spiral_.reserve(usize(edge_) * edge_);
    for (int dz = -gridRadius_; dz <= gridRadius_; ++dz) {
        for (int dx = -gridRadius_; dx <= gridRadius_; ++dx) {
            spiral_.push_back({i16(dx), i16(dz)});
        }
    }
    std::sort(spiral_.begin(), spiral_.end(), [](const Offset& a, const Offset& b) {
        return a.dx * a.dx + a.dz * a.dz < b.dx * b.dx + b.dz * b.dz;
    });

    // **One ring wider, and in the same order**, for the group listings the
    // classification asks about. The order is what makes deferring a
    // classification cheap: listings arrive nearest-first, so the cells under
    // the player are the first to be answered and the horizon waits. Warming
    // row-major -- which is what this was -- had the far north-west corner
    // answered first and the ground being walked on last.
    warmSpiral_.clear();
    const int warm = gridRadius_ + 1;
    warmSpiral_.reserve(usize(warm * 2 + 1) * usize(warm * 2 + 1));
    for (int dz = -warm; dz <= warm; ++dz) {
        for (int dx = -warm; dx <= warm; ++dx) {
            warmSpiral_.push_back({i16(dx), i16(dz)});
        }
    }
    std::sort(warmSpiral_.begin(), warmSpiral_.end(), [](const Offset& a, const Offset& b) {
        return a.dx * a.dx + a.dz * a.dz < b.dx * b.dx + b.dz * b.dz;
    });
}

void WorldStreamer::setMeshDistance(int meshDistance, ChunkRenderer& renderer)
{
    if (!open_ || meshDistance < 1 || meshDistance == meshDistance_) {
        return;
    }

    // The old grid is moved aside rather than indexed in place: the new one
    // wraps modulo a different edge, so a column's cell index changes even
    // though the column has not moved.
    std::vector<Cell> previous = std::move(cells_);

    meshDistance_ = meshDistance;
    loadRadius_ = meshDistance + 1;
    buildGrid();

    // The generator's working set scales with the radius being filled, so a
    // step up needs a bigger cache. It is only ever grown: shrinking would mean
    // discarding columns that neighbouring population passes are still writing
    // into, and those cannot be rebuilt -- see ChunkGenerator::growCacheTo.
    //
    // The wait is what makes it safe to reach into the generator at all from
    // here. It is a settings change, so a pause of one column's work is not
    // something a player will notice against the re-mesh that follows it.
    if (generator_ != nullptr) {
        waitForWorkerIdle();
        generator_->growCacheTo(mcver::ChunkGenerator::cacheColumnsFor(loadRadius_));
        {
            // The grid this was staged from has just been rebuilt around a
            // different radius. Nothing on it is wrong, but it is a frame out
            // of date and the next update() rewrites it from the new grid
            // anyway, so it starts empty rather than half-stale.
            std::lock_guard<std::mutex> guard(queueLock_);
            slate_.clear();
        }
        // The wait paused the worker so it could not hand itself another job
        // while the generator's table was being moved under it. Let it go.
        resumeGeneration();
    }

    // Nothing is published yet -- the renderer's field was rebuilt with the
    // rest -- so every column that survives comes back through the same
    // neighbours-ready gate a freshly loaded one does.
    for (Cell& cell : previous) {
        if (cell.state == CellState::Empty || !centreSet_) {
            continue;
        }
        if (std::abs(cell.chunkX - centreX_) > gridRadius_
            || std::abs(cell.chunkZ - centreZ_) > gridRadius_) {
            continue;  // outside the new radius: let it go
        }
        Cell& destination = cells_[cellIndex(cell.chunkX, cell.chunkZ)];
        destination = std::move(cell);
        destination.published = false;
    }

    // Residency changed the moment the grid did, and the caller may well read
    // it before the next update() -- a debug page reporting the old count would
    // look exactly like columns having been dropped and reloaded, which is the
    // thing this function exists not to do.
    countResidency();

    if (!centreSet_) {
        return;
    }

    // update() only republishes around a column that has just loaded, and after
    // this none has. Once over the grid is what puts the world back on screen
    // in the frame the setting changed rather than as columns trickle in.
    renderer.setCentre(centreX_, centreZ_);
    for (const Offset& offset : spiral_) {
        publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
    }
}

void WorldStreamer::setCubeFormat(mesh::CubeFormat format, ChunkRenderer& renderer)
{
    if (!open_ || format == builder_.cubeFormat()) {
        return;
    }
    builder_.setCubeFormat(format);

    // The grid keeps its columns -- this is a change of geometry, not of what
    // is loaded -- so all that has to happen is that every one of them goes
    // back through the publish gate and gets meshed again.
    for (Cell& cell : cells_) {
        cell.published = false;
    }
    if (!centreSet_) {
        return;
    }
    renderer.setCentre(centreX_, centreZ_);
    for (const Offset& offset : spiral_) {
        publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
    }
}

void WorldStreamer::close(i64 nowMillis)
{
    if (!open_) {
        return;
    }
    // The worker first, and before anything touches storage: it is the other
    // user of the slot, and joining it is what makes the rest of this function
    // single-threaded again.
    stopWorker();

    // Anything the generator finished and has not handed over yet goes to the
    // card now. Dropping it would mean regenerating it next session, and
    // regenerating is not the same as reloading: the population order would be
    // the one this session's walk produced, not the one that made the columns
    // around it.
    if (generator_ != nullptr) {
        generator_->flush();
    }

    // close() blocks until the last column is on the card, which is what the
    // "Saving level.." message on the way out is for. Everything the generator
    // just flushed went into the cache, so this is where it becomes files.
    cache_.close(nowMillis, player_);
    cells_.clear();
    finished_.clear();
    completed_.clear();
    slate_.clear();
    queuePaused_ = false;
    generator_.reset();
    open_ = false;
}

void WorldStreamer::spawnPosition(double* x, double* y, double* z) const
{
    // No narrowing on the way out: level.dat already holds these as doubles,
    // and rounding a far-out player position to float here would move them by
    // up to a block before the game had drawn a frame.
    if (level_.player.present) {
        *x = level_.player.pos[0];
        *y = level_.player.pos[1];
        *z = level_.player.pos[2];
        return;
    }
    *x = double(level_.spawnX);
    *y = double(level_.spawnY);
    *z = double(level_.spawnZ);
}

int WorldStreamer::cellIndex(i32 chunkX, i32 chunkZ) const
{
    return floorMod(chunkZ, edge_) * edge_ + floorMod(chunkX, edge_);
}

WorldStreamer::Cell* WorldStreamer::find(i32 chunkX, i32 chunkZ)
{
    return const_cast<Cell*>(static_cast<const WorldStreamer*>(this)->find(chunkX, chunkZ));
}

const WorldStreamer::Cell* WorldStreamer::find(i32 chunkX, i32 chunkZ) const
{
    if (cells_.empty()) {
        return nullptr;
    }
    if (std::abs(chunkX - centreX_) > gridRadius_ || std::abs(chunkZ - centreZ_) > gridRadius_) {
        return nullptr;
    }
    const Cell& cell = cells_[cellIndex(chunkX, chunkZ)];
    if (cell.state == CellState::Empty || cell.chunkX != chunkX || cell.chunkZ != chunkZ) {
        return nullptr;
    }
    return &cell;
}

// Takes a column the world does have. **Nothing here can touch a card.**
//
// A cache hit is a clone; a miss is a request posted to the I/O thread and a
// cell left exactly as it was, to be asked about again next frame. That is the
// whole change: this used to be an open, a read, a close and a gzip inflate,
// inside the frame, behind a lock the generation worker held while it wrote.
WorldStreamer::LoadResult WorldStreamer::loadColumn(i32 chunkX, i32 chunkZ)
{
    Cell& cell = cells_[cellIndex(chunkX, chunkZ)];

    std::unique_ptr<world::ChunkColumn> column;
    switch (cache_.tryTake(chunkX, chunkZ, &column)) {
    case world::ChunkCache::Take::Took:
        adoptColumn(cell, std::move(column));
        return LoadResult::Loaded;

    case world::ChunkCache::Take::Missing:
        // Classified as present and unreadable now: a damaged file, or one
        // removed under us. Treat it as the edge of the world rather than
        // asking again every frame.
        cell.state = CellState::Absent;
        return LoadResult::Failed;

    case world::ChunkCache::Take::Pending:
        break;
    }
    return LoadResult::Pending;
}

void WorldStreamer::classifyCell(Cell& cell, i32 chunkX, i32 chunkZ)
{
    // Asked once, and the answer kept. Which "no" it is -- the edge of a finite
    // world, or a frontier waiting to be filled -- is the whole difference
    // between a column that may be meshed against and one that may not.
    // **With no generator there is nothing to classify.** Whether the world has
    // a chunk only matters here because the answer decides what gets generated,
    // and it has to be taken before a sweep can change it. Without one, asking
    // is a second trip to the SD card for every chunk that is about to be read
    // anyway: the cell goes straight to OnDisk and loadColumn finds out by
    // reading, exactly as it did before any of this existed.
    //
    // The question is answered from the cache's group index, and **the cell is
    // left alone when the index cannot answer yet**. That is what removes the
    // storm: crossing a chunk boundary used to re-classify a whole row of
    // cells, each one falling back to a `stat` on the render thread that first
    // had to wait for whatever file the I/O thread had open. The symptom was
    // the game stopping for a second or two while moving, with the storage
    // page's main-thread figure climbing to match. Now the group listing is
    // asked for urgently and this cell is asked about again next frame; the
    // sweep that must not run before the answer arrives is held back by
    // classifiedAround rather than by the frame.
    bool exists = true;
    if (generator_ != nullptr) {
        switch (cache_.chunkPresence(chunkX, chunkZ)) {
        case world::ChunkCache::Presence::Present:
            exists = true;
            break;
        case world::ChunkCache::Presence::Absent:
            exists = false;
            break;
        case world::ChunkCache::Presence::Unknown:
            return;  // the listing is on its way; the cell stays Empty
        }
    }

    cell.column.reset();
    cell.chunkX = chunkX;
    cell.chunkZ = chunkZ;
    cell.published = false;
    if (exists) {
        cell.state = CellState::OnDisk;
    } else {
        cell.state = generator_ != nullptr ? CellState::Ungenerated : CellState::Absent;
    }
}

// The 7x7 around a candidate column, which is the reach of the sweep that would
// make it. See the note on the declaration -- and note that every one of those
// cells exists: a candidate is inside the load radius, the grid is three rings
// wider, so three rings out from a candidate is still inside the grid.
bool WorldStreamer::classifiedAround(i32 chunkX, i32 chunkZ) const
{
    for (i32 dz = -3; dz <= 3; ++dz) {
        for (i32 dx = -3; dx <= 3; ++dx) {
            const Cell* cell = find(chunkX + dx, chunkZ + dz);
            if (cell == nullptr || cell->state == CellState::Empty) {
                return false;
            }
        }
    }
    return true;
}

void WorldStreamer::adoptColumn(Cell& cell, std::unique_ptr<world::ChunkColumn> column)
{
    const i32 chunkX = column->x;
    const i32 chunkZ = column->z;
    for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
        cell.masks[sy] = mesh::computeVisibility(column->section(sy), visScratch_);
    }
    cell.column = std::move(column);
    cell.chunkX = chunkX;
    cell.chunkZ = chunkZ;
    cell.state = CellState::Loaded;
    cell.published = false;
    cell.freshlyAdopted = true;
}

bool WorldStreamer::generateColumn(i32 chunkX, i32 chunkZ)
{
    // **Touches nothing the main thread owns.** No cells, no renderer, no
    // visibility masks -- the finished column goes on the queue and
    // drainGenerated() puts it in the grid. That is the whole reason this is
    // safe to call from the worker.

    // -------------------------------------------------------------------
    // **A job for a chunk the world already has does nothing at all**, and
    // this early return is what keeps the worker from changing the world.
    //
    // Population order is the world in a1.1.2: two chunks whose passes reach
    // the same blocks come out differently depending on which ran first, so the
    // sequence of sweeps has to be a function of the game and not of the
    // scheduler. Everything else about that falls out of asking for columns in
    // spiral order -- the nearest missing column is the nearest missing column
    // however many frames have gone by, because the probe walks the same spiral
    // the requests do.
    //
    // The one thing that would not fall out is this: a column can be staged as
    // missing and then written by the sweep of a *neighbour* before the worker
    // reaches it. Sweeping for it anyway would reach three rings further out and
    // generate -- and populate -- ground that the synchronous path never would,
    // and how often that happened would depend on how many frames a generation
    // took. Taking the stored column instead makes such a job a no-op, which is
    // exactly what it is.
    // -------------------------------------------------------------------
    {
        auto stored = std::make_unique<world::ChunkColumn>(chunkX, chunkZ);
        const bool have = cache_.load(chunkX, chunkZ, stored.get()) != nullptr;
        if (have && stored->terrainPopulated) {
            std::lock_guard<std::mutex> guard(queueLock_);
            finished_.push_back(std::move(stored));
            return true;
        }
    }

    if (!generator_->provide(chunkX, chunkZ, generated_.get())) {
        // **Counted, because a silent one stops the world.** The nearest owed
        // column is asked for again next frame and fails again, so a column
        // that cannot be made is a frontier that never advances -- and until
        // this counter existed, the only symptom was generation appearing to
        // stop with nothing on the debug page to say why. It means the
        // generator's own cache could not hold the sweep; see
        // ChunkGenerator::cacheColumnsFor and the `lost` figure beside it.
        std::lock_guard<std::mutex> guard(queueLock_);
        ++generationFailures_;
        return false;
    }

    // The column that was asked for is the one provide() writes out rather than
    // hands to deliver(), so it is saved and queued here. Everything else the
    // sweep finished has already gone through deliver().
    cache_.save(*generated_);
    auto owned = std::make_unique<world::ChunkColumn>(std::move(*generated_));
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        finished_.push_back(std::move(owned));
    }
    return true;
}

// **The slate is rewritten, not appended to: the nearest columns that are owed,
// as of this frame, straight off the grid.**
//
// The spiral is already sorted nearest-first, so the first `kSlateDepth`
// candidates it yields are the next few jobs in the order a1.1.2 asks for them
// -- see the note on slate_ for the jar evidence and for what the queue that
// used to be here was costing.
//
// A column the worker has in hand, or one it has finished and the main thread
// has not adopted yet, is passed over: both still read `Ungenerated` on the
// grid, and putting either back on the slate would run a second sweep for it.
// A column whose neighbourhood is not fully classified **stops the walk**
// rather than being passed over; the note where that happens says why.
//
// There is no cap and nothing to refuse: the slate is at most kSlateDepth long
// by construction, and it is thrown away and rebuilt next frame.
//
// **queueLock_ held**, because the worker reads the slate.
void WorldStreamer::refreshSlate()
{
    slate_.clear();
    if (generator_ == nullptr || !centreSet_) {
        return;
    }

    bool blocked = false;
    for (const Offset& offset : spiral_) {
        if (slate_.size() >= kSlateDepth) {
            break;
        }
        if (std::abs(offset.dx) > loadRadius_ || std::abs(offset.dz) > loadRadius_) {
            continue;
        }
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        const Cell& cell = cells_[cellIndex(cx, cz)];

        // **A cell that has not been asked about stops the walk**, for the same
        // reason a blocked one does below: nobody knows yet whether this column
        // is owed, and if it turns out to be, it has to be swept before
        // anything further out. Passing over it would put the choice of the
        // next sweep in the hands of whichever directory listing landed first.
        if (cell.state == CellState::Empty || cell.chunkX != cx || cell.chunkZ != cz) {
            blocked = true;
            break;
        }
        if (cell.state != CellState::Ungenerated) {
            continue;  // the world has it, or never will
        }

        const std::pair<i32, i32> at{cx, cz};
        if (jobActive_ && inFlight_ == at) {
            continue;
        }
        if (std::find(completed_.begin(), completed_.end(), at) != completed_.end()) {
            continue;
        }
        if (!classifiedAround(cx, cz)) {
            // **Stop, rather than skip to the next one.** Taking a column
            // further out because a nearer one is still waiting on a directory
            // listing would make the order of the sweeps a function of when
            // listings happened to arrive -- and the order of the sweeps is the
            // world. Nearest-first has to mean nearest-first whatever the card
            // is doing, so a blocked column blocks the ones behind it. It is
            // waiting on one listing that has already been asked for urgently.
            blocked = true;
            break;
        }
        slate_.push_back(at);
    }

    // Gated means the nearest ground that is owed cannot be made *yet*, which
    // is a listing away rather than a fault. It reads on the debug page next to
    // the count of cells still waiting to be asked about, because the two are
    // the same sentence.
    stats_.generationGated = blocked;
}

// **queueLock_ held.** The worker calls this as well as the main thread.
bool WorldStreamer::takeSlateLocked(std::pair<i32, i32>* out)
{
    if (slate_.empty()) {
        return false;
    }
    *out = slate_.front();
    slate_.erase(slate_.begin());
    return true;
}

// Makes the columns on the slate when there is no worker to make them.
//
// **a1.1.2 has no worker and no queue at all.** `ft.b` (provideChunk) loads the
// chunk and, failing that, calls the generator *inline* on the game thread, so
// the order is simply the order things ask for chunks -- and what asks is the
// renderer, which does `Arrays.sort(worldRenderers, new RenderSorter(player))`
// before it rebuilds them. Nearest-to-the-player, verified in the jar: class `e`
// at bytecode 686, comparator `fb`, ordering on `WorldRenderer.a(Entity)`
// ascending. This path is that, one column per frame; the worker is that with
// the stall taken off the frame.
void WorldStreamer::pumpGeneration(const Budget& budget)
{
    if (generator_ == nullptr) {
        return;
    }

    if (workerRunning_) {
        // **Nothing is handed out here.** The worker takes its own next job the
        // moment it finishes one; all this does is make sure it is awake.
        // Dispatching from here capped generation at one column per rendered
        // frame, which on a console holding thirty frames a second was thirty
        // columns a second however fast core 2 could actually go.
        std::lock_guard<std::mutex> guard(queueLock_);
        if (!slate_.empty()) {
            wake_.notify_one();
        }
        return;
    }

    // No worker. This is the path that stutters, and it is kept only so a
    // console whose thread would not start still fills its world in.
    for (int made = 0; made < budget.generatedPerFrame; ++made) {
        std::pair<i32, i32> at{0, 0};
        {
            std::lock_guard<std::mutex> guard(queueLock_);
            if (!takeSlateLocked(&at)) {
                break;
            }
        }
        const auto begin = std::chrono::steady_clock::now();
        generateColumn(at.first, at.second);
        stats_.generateMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count();
    }
}

void WorldStreamer::dropCell(Cell& cell, ChunkRenderer& renderer)
{
    if (cell.state == CellState::Empty) {
        return;
    }
    if (cell.published) {
        renderer.dropColumn(cell.chunkX, cell.chunkZ);
    }

    // **Given back rather than freed.** A column that leaves the grid is one
    // the player may walk straight back into, and re-reading it costs an open,
    // a read and an inflate. The cache keeps it until its byte cap says
    // otherwise, so turning round is free. It is also what makes the read-ahead
    // band worth having: the two are the same table.
    if (cell.column != nullptr) {
        cache_.give(std::move(cell.column));
    }
    cell.column.reset();
    cell.state = CellState::Empty;
    cell.published = false;
}

bool WorldStreamer::neighboursReady(i32 chunkX, i32 chunkZ) const
{
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            // Absent counts as ready: the world genuinely ends there, and
            // waiting for a chunk that will never arrive would leave a ring of
            // permanently unmeshed columns at the edge of a finite world.
            //
            // **Ungenerated does not.** That chunk is on its way, and meshing
            // against it now would cull the faces of this column against air
            // and never come back to fix them -- a wall of holes along the
            // frontier, one ring behind wherever the player has walked.
            const Cell* cell = find(chunkX + dx, chunkZ + dz);
            if (cell == nullptr || cell->state == CellState::Empty
                || cell->state == CellState::OnDisk || cell->state == CellState::Ungenerated) {
                return false;
            }
        }
    }
    return true;
}

void WorldStreamer::publishIfReady(i32 chunkX, i32 chunkZ, ChunkRenderer& renderer)
{
    Cell* cell = find(chunkX, chunkZ);
    if (cell == nullptr || cell->state != CellState::Loaded) {
        return;
    }
    if (!renderer.inRange(chunkX, chunkZ)) {
        return;
    }

    // ---------------------------------------------------------------------
    // **`published` is a cache of "the renderer has this column", and the
    // renderer can invalidate it without telling anyone.**
    //
    // The field wraps modulo the render distance, so a column one ring outside
    // it shares a cell with the column on the opposite side -- and when that
    // one is published, `publishColumn` releases this one's meshes and takes
    // the cell. Nothing tells this class. Walk out past the render distance and
    // back and the flag still says "published" while the field holds somebody
    // else, so the column is never handed over again: the walk finds its cell
    // occupied by a different column, treats this one as not loaded, and draws
    // nothing of it. Changing the render distance put it right, because that
    // rebuilds the field and republishes everything -- which is exactly the
    // shape of the bug report this comes from.
    //
    // The streamer's grid is three rings wider than what it loads, so the band
    // where this happens is real and a few chunks of walking wide. Asking the
    // renderer costs one lookup and makes the flag advisory instead of load
    // bearing.
    // ---------------------------------------------------------------------
    if (cell->published && renderer.hasColumn(chunkX, chunkZ)) {
        return;
    }
    if (!neighboursReady(chunkX, chunkZ)) {
        return;
    }

    // A column that has been read in since the renderer last saw it cannot
    // inherit what the renderer still holds under its coordinates -- see
    // Cell::freshlyAdopted. Dropping first is what makes publishColumn treat it
    // as an arrival rather than as the same column coming back, so its sections
    // are meshed again instead of drawing slots that belong to somebody else
    // now.
    if (cell->freshlyAdopted) {
        renderer.dropColumn(chunkX, chunkZ);
    }
    cell->published = renderer.publishColumn(chunkX, chunkZ, cell->masks);
    if (cell->published) {
        cell->freshlyAdopted = false;
    }
}

bool WorldStreamer::meshSection(const VisibleSection& section, ChunkRenderer& renderer)
{
    const Cell* cell = find(section.chunkX, section.chunkZ);
    if (cell == nullptr || cell->state != CellState::Loaded) {
        // Only published columns reach the queue and only loaded ones are
        // published, so this is the column having gone in between. There is
        // nothing to do and nothing wrong: true, so the caller moves on to the
        // next section rather than treating it as a full pool.
        return true;
    }

    const int sy = int(section.sectionY);

    // The cheap rejection: 36 % of a real world's sections are uniform air and
    // never reach the mesher at all.
    if (mesh::sectionIsEmpty(*cell->column, sy)) {
        renderer.noteEmptySection(section.chunkX, sy, section.chunkZ);
        ++stats_.emptyThisFrame;
        return true;
    }

    mesh::ColumnNeighbourhood neighbourhood;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Cell* n = find(section.chunkX + dx, section.chunkZ + dz);
            neighbourhood.at(dx, dz) = n != nullptr ? n->column.get() : nullptr;
        }
    }

    builder_.clear();
    scratch_.fill(neighbourhood, sy);
    mesh::meshSection(scratch_, builder_);

    if (builder_.empty()) {
        renderer.noteEmptySection(section.chunkX, sy, section.chunkZ);
        ++stats_.emptyThisFrame;
        return true;
    }

    // One contiguous buffer per section: cubes, opaque detail, translucent
    // detail, in that order.
    const mesh::MeshRanges ranges = builder_.ranges();
    staging_.resize(ranges.total());
    builder_.copyTo(staging_.data());

    if (!renderer.uploadSection(section.chunkX, sy, section.chunkZ, staging_.data(), ranges)) {
        // The pool is full of geometry this frame is already drawing. The
        // section stays unmeshed and comes back round; nothing is lost but the
        // work, which is why the mesh budget is small.
        return false;
    }

    ++stats_.meshedThisFrame;
    return true;
}

void WorldStreamer::update(ChunkRenderer& renderer, i32 cameraChunkX, i32 cameraChunkZ,
                       const Budget& budget)
{
    stats_.columnsLoadedThisFrame = 0;
    stats_.meshedThisFrame = 0;
    stats_.emptyThisFrame = 0;
    stats_.generatedThisFrame = 0;
    stats_.adoptedThisFrame = 0;
    stats_.generateMicros = 0;
    stats_.pendingGeneration = 0;

    if (!open_) {
        return;
    }

    // Moving the centre invalidates nothing by itself -- the grid wraps -- but
    // a cell now holding a column from outside the new radius has to go, or it
    // would answer for a column that is no longer there.
    //
    // The first frame always takes this path even if the camera happens to be
    // standing at chunk (0, 0), because the renderer's centre has to be told
    // once before the walk can find anything.
    const bool centreMoved =
        !centreSet_ || cameraChunkX != centreX_ || cameraChunkZ != centreZ_;
    if (centreMoved) {
        centreSet_ = true;
        centreX_ = cameraChunkX;
        centreZ_ = cameraChunkZ;
        renderer.setCentre(cameraChunkX, cameraChunkZ);

        for (Cell& cell : cells_) {
            if (cell.state == CellState::Empty) {
                continue;
            }
            if (std::abs(cell.chunkX - centreX_) > gridRadius_
                || std::abs(cell.chunkZ - centreZ_) > gridRadius_) {
                dropCell(cell, renderer);
            }
        }

        // Only here, and deliberately: the set of groups worth listing and
        // columns worth reading ahead changes when the centre does and at no
        // other time, so doing this per frame would be several hundred lookups
        // a frame to reach the same conclusion.
        warmAndPrefetch();
    }

    // Whatever the worker finished since the last frame, and after the centre
    // has moved so a result is judged against where the player is now. A column
    // adopted here is one the scan below does not have to ask storage about.
    drainGenerated(renderer);

    // **Classification first, over the whole grid, and unbudgeted.** It is one
    // index lookup per cell and it never touches a card, so "unbudgeted" now
    // means what it says: the frame cost is a lookup, not an SD operation. A
    // cell whose group has not been listed yet is left Empty and asked about
    // again next frame -- see classifyCell.
    stats_.unclassified = 0;
    for (const Offset& offset : spiral_) {
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        Cell& cell = cells_[cellIndex(cx, cz)];

        const bool stale = cell.state != CellState::Empty
                           && (cell.chunkX != cx || cell.chunkZ != cz);
        if (stale) {
            dropCell(cell, renderer);
        }
        if (cell.state == CellState::Empty) {
            classifyCell(cell, cx, cz);
        }
        if (cell.state == CellState::Empty) {
            ++stats_.unclassified;
        }
    }

    // **Republish everything the camera has just brought back into range.**
    //
    // Publishing only ever happened where something arrived -- around a column
    // that had just been read, or over the whole grid when a generated one was
    // adopted. Walking back over ground that is already in memory and already
    // on the card does neither: nothing is loaded and nothing is generated, so
    // nothing published, and a column that had lost its cell to the field's
    // wrap never got it back. That is the other half of the bug the note in
    // publishIfReady describes, and it is why the symptom needed *revisiting*
    // rather than merely arriving.
    //
    // Once per chunk-boundary crossing, and after classification so that a
    // newly exposed neighbour has a state for neighboursReady to read. Most of
    // the sweep exits on the range check; what is left is a lookup each.
    if (centreMoved) {
        for (const Offset& offset : spiral_) {
            publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
        }
    }

    // Then read what the world already has, nearest first and within the
    // budget, and count what it does not.
    int loaded = 0;
    int pending = 0;
    int pendingReads = 0;
    int pendingGeneration = 0;
    for (const Offset& offset : spiral_) {
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        if (std::abs(offset.dx) > loadRadius_ || std::abs(offset.dz) > loadRadius_) {
            continue;  // classification only out here
        }
        Cell& cell = cells_[cellIndex(cx, cz)];

        if (cell.state == CellState::Ungenerated) {
            // Counted here; which of them the worker is given is decided in
            // refreshSlate, once the whole spiral has been walked.
            ++pendingGeneration;
            continue;
        }

        if (cell.state != CellState::OnDisk) {
            continue;
        }

        ++pending;
        if (loaded >= budget.columnsPerFrame) {
            continue;
        }

        // **A pending column costs nothing and must not spend the budget.** It
        // is a request posted to the I/O thread, not work done here, and
        // counting it would mean one column still being read held up every
        // other one in the spiral behind it -- which at a budget of one or two
        // a frame is most of the world.
        const LoadResult result = loadColumn(cx, cz);
        if (result == LoadResult::Pending) {
            ++pendingReads;
            continue;
        }
        if (result == LoadResult::Loaded) {
            ++stats_.columnsLoadedThisFrame;
        }
        ++loaded;

        // A column arriving completes its own neighbourhood and possibly its
        // eight neighbours', so all nine are re-examined rather than just this
        // one.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                publishIfReady(cx + dx, cz + dz, renderer);
            }
        }
    }
    stats_.pendingGeneration = pendingGeneration;

    // **After the whole spiral, because the slate is the head of it.** The scan
    // above is what put the grid in the state this reads.
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        refreshSlate();
    }

    pumpGeneration(budget);

    stats_.pendingColumns = pending;
    stats_.pendingReads = pendingReads;

    // With no worker the generator ran on this thread, so its counters can be
    // read directly. With one they arrive through drainGenerated, published by
    // the worker under the queue lock.
    if (generator_ != nullptr && !workerRunning_) {
        stats_.generatorPeakLive = generator_->stats().peakLive;
        stats_.generatorEvictedLive = generator_->stats().evictedLive;
    }
    stats_.workerRunning = workerRunning_;

    // Anything the walk asked for, up to the budget. The queue is already cut
    // to it by the renderer and is ordered nearest first.
    for (const VisibleSection& section : renderer.meshQueue()) {
        if (!meshSection(section, renderer)) {
            break;
        }
    }

    // Last, so the counters describe the frame that just happened. Unthreaded
    // this also does one unit of queued I/O, which is how the host harnesses
    // make progress without a thread.
    const world::ChunkCache::Stats before = cache_.stats();
    cache_.pump();
    stats_.io = cache_.stats();
    stats_.prefetched = int(stats_.io.prefetchHits - before.prefetchHits);

    countResidency();
}

// Lists the directory groups the classification is about to ask about, and
// reads ahead the columns just outside what is loaded.
//
// **One ring wider than the grid**, because a group has to be listed before the
// cell that needs it is exposed, and a cell at `gridRadius_` was one chunk
// outside the grid a crossing ago. Sprinting can still outrun it, which is what
// the `stat` fallback in ChunkCache::hasChunk is for -- it is then as slow as it
// always was, for one cell, instead of for a whole row.
void WorldStreamer::warmAndPrefetch()
{
    // The index only earns anything where classification asks a question, and
    // classification only asks one when there is a generator; see classifyCell.
    if (generator_ != nullptr) {
        for (const Offset& offset : warmSpiral_) {
            cache_.warmGroup(centreX_ + offset.dx, centreZ_ + offset.dz);
        }
    }

    if (prefetchRings_ <= 0) {
        return;
    }

    // Capped at the classification band: a cell further out than that has no
    // cell of its own, so nothing here knows whether the world even has it.
    const int reach = std::min(loadRadius_ + prefetchRings_, gridRadius_);
    for (const Offset& offset : spiral_) {
        if (std::abs(offset.dx) > reach || std::abs(offset.dz) > reach) {
            continue;
        }
        if (std::abs(offset.dx) <= loadRadius_ && std::abs(offset.dz) <= loadRadius_) {
            continue;  // inside the load radius: the ordinary path reads it
        }
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        const Cell& cell = cells_[cellIndex(cx, cz)];
        if (cell.state == CellState::OnDisk && cell.chunkX == cx && cell.chunkZ == cz) {
            cache_.prefetch(cx, cz);
        }
    }
}

void WorldStreamer::setPlayerState(double x, double y, double z, float yaw, float pitch,
                                   i64 timeTicks)
{
    player_.valid = true;
    player_.pos[0] = x;
    player_.pos[1] = y;
    player_.pos[2] = z;
    player_.rotation[0] = yaw;
    player_.rotation[1] = pitch;
    player_.timeTicks = timeTicks;
}

void WorldStreamer::tickSaves(i64 nowMillis)
{
    if (!open_ || autosaveSeconds_ <= 0) {
        return;
    }
    if (nowMillis - lastSaveMillis_ < i64(autosaveSeconds_) * 1000) {
        return;
    }
    saveNow(nowMillis);
}

void WorldStreamer::saveNow(i64 nowMillis)
{
    if (!open_) {
        return;
    }
    lastSaveMillis_ = nowMillis;

    // level.dat and session.lock have no other trigger during a session --
    // close() is the only thing that rewrites either today, so a console that
    // loses power mid-session comes back with a LastPlayed from whenever the
    // world was last left properly, and with a lock that has not been refreshed
    // since it was claimed. docs/world-format.md says to run the lock on a
    // timer; this is that timer.
    //
    // **Requested rather than done here.** Both are small files and it would be
    // easy to write them from this thread, which is the render thread -- and a
    // deflate plus an atomic write inside a frame every interval is precisely
    // the thing this whole arrangement exists to stop.
    cache_.requestHousekeeping(nowMillis, player_);
    flushSaves(false);
}

void WorldStreamer::flushSaves(bool blocking)
{
    if (!open_) {
        return;
    }
    cache_.flush(blocking);
}

void WorldStreamer::countResidency()
{
    stats_.columnsResident = 0;
    stats_.columnsMissing = 0;
    stats_.blockBytes = 0;
    for (const Cell& cell : cells_) {
        if (centreSet_
            && (std::abs(cell.chunkX - centreX_) > loadRadius_
                || std::abs(cell.chunkZ - centreZ_) > loadRadius_)) {
            continue;  // classification-only ring; never read in, never drawn
        }
        if (cell.state == CellState::Loaded) {
            ++stats_.columnsResident;
            stats_.blockBytes += cell.column->memoryUsage();
        } else if (cell.state == CellState::Absent || cell.state == CellState::Ungenerated) {
            // Both are "not in memory". Which one it is says whether anything
            // is going to change that, and pendingGeneration is the count that
            // separates them.
            ++stats_.columnsMissing;
        }
    }
}

}  // namespace mc::render
