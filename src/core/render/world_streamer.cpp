#include "core/render/world_streamer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace mc::render {

WorldStreamer::WorkerSpawn WorldStreamer::workerSpawn_ = nullptr;
WorldStreamer::WorkerJoin WorldStreamer::workerJoin_ = nullptr;

namespace {

int floorMod(i32 value, int modulus)
{
    const int r = int(value % modulus);
    return r < 0 ? r + modulus : r;
}

}  // namespace

bool WorldStreamer::open(const char* worldDir, int meshDistance, i64 nowMillis)
{
    if (storage_.open(worldDir, nowMillis) != world::OpenResult::Ok) {
        return false;
    }

    path_ = worldDir;
    level_ = storage_.level();
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

    // **Storage, and only storage.** An earlier version answered out of the
    // resident grid, which is faster and became wrong the moment this started
    // running on another thread: the grid is the main thread's, and it changes
    // under the player's feet. Storage is authoritative anyway -- every column
    // the generator finishes is written before it is handed over -- so the only
    // thing lost is a shortcut, and the generator's own cache absorbs most of
    // what that shortcut was saving.
    std::lock_guard<std::mutex> guard(self.storageLock_);
    return self.storage_.loadChunk(chunkX, chunkZ, scratch) ? scratch : nullptr;
}

void WorldStreamer::generatorDeliver(void* context, world::ChunkColumn& column)
{
    WorldStreamer& self = *static_cast<WorldStreamer*>(context);

    // **Saving is not optional.** The generator evicts what it has handed over
    // and reaches it again through load(); a column that was never written
    // would come back as bare terrain with every neighbour's population
    // missing, and the world would be quietly wrong rather than obviously so.
    {
        std::lock_guard<std::mutex> guard(self.storageLock_);
        self.storage_.saveChunk(column);
    }

    // Then the grid gets it, and **not from here**: this runs on the worker,
    // and the grid, the renderer and the visibility masks are the main
    // thread's. The column is queued and adopted in drainGenerated().
    //
    // The two locks are taken one after the other and never nested, which is
    // the whole deadlock argument for this class.
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
    // "core 2". See setWorkerThreadOps.
    if (workerSpawn_ != nullptr && workerJoin_ != nullptr) {
        platformWorker_ = workerSpawn_(&WorldStreamer::workerEntry, this);
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
    idle_.wait(guard, [this] { return !jobPending_ && !jobActive_; });
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
        workerJoin_(platformWorker_);
        platformWorker_ = nullptr;
    } else {
        worker_.join();
    }
    std::lock_guard<std::mutex> guard(queueLock_);
    workerRunning_ = false;
}

// The worker's whole life: wait for a column to be asked for, make it, repeat.
//
// It holds no lock while generating, which is the point -- the main thread must
// never wait on anything longer than a pointer swap. What it does share is the
// storage slot, and that is taken and released once per chunk file inside
// generatorLoad and generatorDeliver.
void WorldStreamer::workerMain()
{
    for (;;) {
        i32 cx = 0;
        i32 cz = 0;
        {
            std::unique_lock<std::mutex> guard(queueLock_);
            wake_.wait(guard, [this] { return workerStop_ || jobPending_; });
            if (workerStop_) {
                jobActive_ = false;
                return;
            }
            cx = jobX_;
            cz = jobZ_;
            jobPending_ = false;
            jobActive_ = true;
        }

        generateColumn(cx, cz);

        // The generator's own counters are published here rather than read
        // from the main thread. They belong to the worker, which is the only
        // thread allowed inside the generator at all, and "it is only a
        // counter" is not a reason to leave a read unsynchronised -- the
        // thread sanitizer named this one before it could become a habit.
        std::lock_guard<std::mutex> guard(queueLock_);
        workerPeakLive_ = generator_->stats().peakLive;
        workerEvictedLive_ = generator_->stats().evictedLive;
        jobActive_ = false;
        jobDone_ = true;
        idle_.notify_all();
    }
}

bool WorldStreamer::generationIdle() const
{
    if (!generationQueue_.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> guard(queueLock_);
    return !jobPending_ && !jobActive_ && finished_.empty();
}

// Takes what the worker finished into the grid. Main thread, once a frame.
//
// A result may be for a column that has since gone out of range -- the player
// kept walking while it was being made -- and dropping it costs nothing,
// because it was written to the card before it was queued.
void WorldStreamer::drainGenerated(ChunkRenderer& renderer)
{
    std::vector<std::unique_ptr<world::ChunkColumn>> batch;
    bool done = false;
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        batch.swap(finished_);
        stats_.generatorPeakLive = workerPeakLive_;
        stats_.generatorEvictedLive = workerEvictedLive_;
        done = jobDone_;
        jobDone_ = false;
    }
    if (done) {
        queued_.erase(inFlight_);
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

    // Two grids' worth. A player running across ungenerated ground can queue
    // faster than the worker drains, and the cap is what keeps that bounded --
    // as a function of the grid rather than of the clock, so it bites at the
    // same point on every machine.
    generationQueueCap_ = usize(edge_) * usize(edge_) * 2;

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
    storage_.close(nowMillis);
    cells_.clear();
    finished_.clear();
    generationQueue_.clear();
    queued_.clear();
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

bool WorldStreamer::loadColumn(i32 chunkX, i32 chunkZ)
{
    Cell& cell = cells_[cellIndex(chunkX, chunkZ)];

    auto column = std::make_unique<world::ChunkColumn>(chunkX, chunkZ);
    {
        std::lock_guard<std::mutex> guard(storageLock_);
        if (!storage_.loadChunk(chunkX, chunkZ, column.get())) {
            // Classified as present and unreadable now: a damaged file, or one
            // removed under us. Treat it as the edge of the world rather than
            // asking again every frame.
            cell.state = CellState::Absent;
            return false;
        }
    }

    adoptColumn(cell, std::move(column));
    return true;
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
    bool exists = true;
    if (generator_ != nullptr) {
        std::lock_guard<std::mutex> guard(storageLock_);
        exists = storage_.hasChunk(chunkX, chunkZ);
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
    if (unclassified_ > 0) {
        --unclassified_;
    }
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
    // The one thing that would not fall out is this: a column can be queued as
    // missing and then written by the sweep of a *neighbour* before the worker
    // reaches it. Sweeping for it anyway would reach three rings further out and
    // generate -- and populate -- ground that the synchronous path never would,
    // and how often that happened would depend on how many frames a generation
    // took. Taking the stored column instead makes such a job a no-op, which is
    // exactly what it is.
    // -------------------------------------------------------------------
    {
        auto stored = std::make_unique<world::ChunkColumn>(chunkX, chunkZ);
        bool have = false;
        {
            std::lock_guard<std::mutex> guard(storageLock_);
            have = storage_.loadChunk(chunkX, chunkZ, stored.get());
        }
        if (have && stored->terrainPopulated) {
            std::lock_guard<std::mutex> guard(queueLock_);
            finished_.push_back(std::move(stored));
            return true;
        }
    }

    if (!generator_->provide(chunkX, chunkZ, generated_.get())) {
        return false;
    }

    // The column that was asked for is the one provide() writes out rather than
    // hands to deliver(), so it is saved and queued here. Everything else the
    // sweep finished has already gone through deliver().
    {
        std::lock_guard<std::mutex> guard(storageLock_);
        storage_.saveChunk(*generated_);
    }
    auto owned = std::make_unique<world::ChunkColumn>(std::move(*generated_));
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        finished_.push_back(std::move(owned));
    }
    return true;
}

// Hands the head of the generation queue to the worker, or makes it here if
// there is no worker.
//
// One column is in flight at a time and the queue is consumed strictly in
// order, which is what makes the world the same however fast the generator is:
// when job N starts, jobs 1..N-1 have finished, on any machine.
void WorldStreamer::pumpGeneration(const Budget& budget)
{
    if (generator_ == nullptr || generationQueue_.empty()) {
        return;
    }

    if (workerRunning_) {
        std::lock_guard<std::mutex> guard(queueLock_);
        // **`jobDone_` is part of the test, and leaving it out cost a column.**
        // The worker can finish between drainGenerated() and here, in the same
        // frame: the job is then neither pending nor active, so without this
        // the head would be handed out a second time. Two completions then
        // arrive, two entries come off the queue, and the second one is a
        // column that was never generated -- a hole in the world that only
        // appears when the timing lines up.
        if (jobPending_ || jobActive_ || jobDone_) {
            return;
        }
        inFlight_ = generationQueue_.front();
        generationQueue_.erase(generationQueue_.begin());
        jobX_ = inFlight_.first;
        jobZ_ = inFlight_.second;
        jobPending_ = true;
        wake_.notify_one();
        return;
    }

    // No worker. This is the path that stutters, and it is kept only so a
    // console whose thread would not start still fills its world in. It takes
    // from the same queue in the same order, which is why the two produce the
    // same world.
    for (int made = 0; made < budget.generatedPerFrame && !generationQueue_.empty(); ++made) {
        const std::pair<i32, i32> at = generationQueue_.front();
        generationQueue_.erase(generationQueue_.begin());
        const auto begin = std::chrono::steady_clock::now();
        generateColumn(at.first, at.second);
        stats_.generateMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count();
        queued_.erase(at);
    }
}

// Appends a column to the generation queue if it is not already on it.
//
// **Append only, and never reordered.** See the note on generationQueue_: the
// order sweeps happen in is part of the world, so it must not depend on where
// the camera got to while the last one was being made.
void WorldStreamer::enqueueGeneration(i32 chunkX, i32 chunkZ)
{
    const std::pair<i32, i32> at{chunkX, chunkZ};
    if (queued_.count(at) != 0) {
        return;
    }
    if (generationQueue_.size() >= generationQueueCap_) {
        ++stats_.generationRefused;
        return;
    }
    queued_.insert(at);
    generationQueue_.push_back(at);
}

void WorldStreamer::dropCell(Cell& cell, ChunkRenderer& renderer)
{
    if (cell.state == CellState::Empty) {
        return;
    }
    ++unclassified_;
    if (cell.published) {
        renderer.dropColumn(cell.chunkX, cell.chunkZ);
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
    if (cell == nullptr || cell->state != CellState::Loaded || cell->published) {
        return;
    }
    if (!renderer.inRange(chunkX, chunkZ) || !neighboursReady(chunkX, chunkZ)) {
        return;
    }
    if (renderer.publishColumn(chunkX, chunkZ, cell->masks)) {
        cell->published = true;
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
    if (!centreSet_ || cameraChunkX != centreX_ || cameraChunkZ != centreZ_) {
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
    }

    // Whatever the worker finished since the last frame, and after the centre
    // has moved so a result is judged against where the player is now. A column
    // adopted here is one the scan below does not have to ask storage about.
    drainGenerated(renderer);

    // **Classification first, over the whole grid, and unbudgeted.** It is one
    // existence check per cell and it happens once in a session; what it buys
    // is that every question about what the world already has is asked before
    // any sweep can have changed the answer. See the note on CellState.
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
    }

    // Then read what the world already has, nearest first and within the
    // budget, and queue what it does not.
    int loaded = 0;
    int pending = 0;
    int pendingGeneration = 0;
    for (const Offset& offset : spiral_) {
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        if (std::abs(offset.dx) > loadRadius_ || std::abs(offset.dz) > loadRadius_) {
            continue;  // classification only out here
        }
        Cell& cell = cells_[cellIndex(cx, cz)];

        if (cell.state == CellState::Ungenerated) {
            ++pendingGeneration;
            // Nothing is queued until every cell has been asked about: a sweep
            // that started earlier could otherwise write a column before the
            // streamer had a chance to ask whether the world already had it,
            // and the answer would then depend on the clock.
            if (unclassified_ == 0) {
                enqueueGeneration(cx, cz);
            }
            continue;
        }

        if (cell.state != CellState::OnDisk) {
            continue;
        }

        ++pending;
        if (loaded >= budget.columnsPerFrame) {
            continue;
        }
        if (loadColumn(cx, cz)) {
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
    stats_.generationQueued = int(generationQueue_.size());
    stats_.generationGated = unclassified_ != 0;
    stats_.generationStale = 0;
    for (const std::pair<i32, i32>& at : generationQueue_) {
        if (std::abs(at.first - centreX_) > loadRadius_
            || std::abs(at.second - centreZ_) > loadRadius_) {
            ++stats_.generationStale;
        }
    }

    // Only after the whole spiral has been walked, so the queue this frame
    // added to is in nearest-first order before anything is taken from it.
    pumpGeneration(budget);

    stats_.pendingColumns = pending;

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

    countResidency();
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
