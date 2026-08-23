#pragma once

// The loaded world around the camera: which columns are in memory, which are
// meshed, and the budget that keeps both off the frame time.
//
// Everything here is bookkeeping around three core pieces -- the storage slot,
// the mesher, and ChunkRenderer -- and none of it needs a GPU, which is why it
// is core rather than platform code. The console and the host harness run the
// same streamer over the same world; only who draws the result differs.
//
// Both budgets are per frame and both are small. Reading a column costs a
// gzip inflate (median 2,917 bytes on the real world) and meshing a section
// measured 30.4 us on the dev host, so a frame that did all the work the walk
// asked for would stall for tens of milliseconds the first time the player
// turned round. Meshing moves to a worker thread later -- the seam for that is
// this class, not the renderer.
//
// The rule that shapes most of it: **a section cannot be meshed until all eight
// of its column's neighbours are loaded**, because every face is culled against
// a block that may live in the column next door. A column is therefore held
// back from the renderer until its neighbourhood is complete, which is why
// columns are kept one ring wider than the render distance.

#include "core/io/posix_file_system.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/scratch.hpp"
#include "core/mesh/visibility.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/util/worker.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/level_data.hpp"
#include "version_slots.hpp"

#include <condition_variable>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc::render {

class WorldStreamer {
public:
    struct Budget {
        int columnsPerFrame = 1;
        int meshesPerFrame = 4;

        // **Generated columns are not loaded columns and do not share their
        // budget.** Reading one is a gzip inflate; making one is terrain,
        // caves, sixteen population passes over the sweep and a light solve,
        // measured at 3.7 ms on the dev host and therefore tens of milliseconds
        // on a 268 MHz ARM11 -- several whole frames for one column.
        //
        // **This is the budget for generating on the calling thread**, which is
        // the fallback when a worker could not be started. With the worker
        // running it is not used at all: the main thread posts one job and
        // moves on, and columns appear when they appear.
        int generatedPerFrame = 1;
    };

    struct Stats {
        int columnsResident = 0;
        int columnsLoadedThisFrame = 0;
        int columnsMissing = 0;   // in range, and there is no such chunk on disk
        int meshedThisFrame = 0;
        int emptyThisFrame = 0;
        usize blockBytes = 0;     // what the columns cost in the newlib heap
        int pendingColumns = 0;   // still to load inside the radius

        int generatedThisFrame = 0;   // columns the worker finished and handed back
        int adoptedThisFrame = 0;     // ...of which this many landed in the grid
        int pendingGeneration = 0;    // in range, not on the card, not made yet

        // **What the queue is doing, which `pendingGeneration` alone cannot
        // say.** A column that is owed can be owed for three different reasons
        // and they need different fixes: it is waiting its turn
        // (`generationQueued` high), it cannot be asked for at all because the
        // grid is not fully classified (`generationGated`), or it was refused
        // because the queue is full (`generationRefused`). Without these, a
        // stalled world and a slow one look the same from the debug page.
        int generationQueued = 0;     // columns on the queue, waiting their turn
        int generationStale = 0;      // ...of which this many are out of range now
        bool generationGated = false; // unclassified cells: nothing may be queued
        int generationRefused = 0;    // enqueues dropped because the queue is full
        i64 generateMicros = 0;       // main-thread cost only: 0 while the worker runs
        u32 generatorPeakLive = 0;    // the generator's own high-water mark
        u32 generatorEvictedLive = 0; // must stay zero; see ChunkGenerator
        bool workerRunning = false;   // false means generation is on this thread

        // What the card is doing, copied out of the cache once a frame. The
        // one to watch is `io.mainThreadMicros`: it is the number this whole
        // arrangement exists to hold at zero.
        world::ChunkCache::Stats io;

        int pendingReads = 0;   // columns in range being read right now
        int prefetched = 0;     // columns read ahead this frame
    };

    // `meshDistance` is the render distance in chunks. Columns are held one
    // ring wider than that, because a section cannot be meshed until all eight
    // of its column's neighbours are present -- a face is culled against a
    // block that may live in the column next door.
    bool open(const char* worldDir, int meshDistance, i64 nowMillis);
    void close(i64 nowMillis);

    // **Whether a chunk the world does not have gets made, and it is off by
    // default.**
    //
    // Generating is what the original does -- an Alpha world is unbounded and
    // walking west makes more of it -- so the game turns this on. The host
    // harnesses do not: `--mesh` and `--fly` measure a fixed world, and a
    // streamer that quietly extended it would both change the numbers and write
    // chunk files into the copy under measurement.
    //
    // Call it before open(); the generator is built there, from level.dat's
    // seed and its SnowCovered flag, and it is roughly a megabyte plus its
    // cache, so nothing is allocated when this is off.
    void setGenerateMissing(bool generate) { generateMissing_ = generate; }
    bool generateMissing() const { return generateMissing_; }

    // **Runs generation on a thread of its own, and this is the whole point of
    // the exercise.** A generated column is several frames' worth of work; done
    // on the main thread it does not slow the game down, it stops it. Off the
    // main thread the frame rate is untouched and a column simply takes longer
    // to appear, which is the trade a player can live with.
    //
    // On by default. Call it before open(). If the thread cannot be started the
    // streamer says so through `Stats::workerRunning` and falls back to
    // generating inline, which is slow but not broken.
    void setGenerationThreaded(bool threaded) { generationThreaded_ = threaded; }

    // The chunk cache's budget and whether it gets a thread. Call before
    // open(); resizing a table the I/O thread is walking is not something this
    // needs to support.
    void setCacheConfig(const world::ChunkCache::Config& config) { cacheConfig_ = config; }
    const world::ChunkCache::Config& cacheConfig() const { return cacheConfig_; }

    // **How wide the read-ahead band is, in chunks beyond the load radius.**
    //
    // The grid is already classified three rings wider than what is loaded --
    // that is a generation sweep's reach -- so those cells cost nothing extra
    // to know about, and reading them into the cache before the player reaches
    // them is what turns a chunk-boundary crossing from an SD read into a
    // memcpy. 0 turns it off, which is what a console short of heap wants.
    //
    // It is capped at the classification band, because a cell further out than
    // that has no cell to be asked about in.
    void setPrefetchRings(int rings) { prefetchRings_ = rings < 0 ? 0 : rings; }
    int prefetchRings() const { return prefetchRings_; }

    // **The autosave interval, in seconds; 0 turns the timer off.**
    //
    // It is ours rather than the original's: a1.1.2 has no timed autosave at
    // all. Disassembling the client jar, `ft.saveChunks(saveAll, progress)`
    // writes at most two dirty chunks per call when `saveAll` is false, and its
    // only periodic caller is the "Saving level.." screen reached from Save and
    // quit to title; otherwise a chunk is written when it falls out of the
    // provider's 1024-slot cache, synchronously, on the main thread.
    //
    // What the timer governs here is level.dat and session.lock, which have no
    // other trigger during a session, and -- once M3 has block placement --
    // player edits, where coalescing many edits to one column into one deflate
    // is exactly what a timer buys. **Generated columns do not wait for it**:
    // they are queued for writing as soon as they are made, because holding
    // them would open a window in which a power-off loses world that
    // regenerating cannot reproduce -- population order is the world.
    void setAutosaveSeconds(int seconds) { autosaveSeconds_ = seconds < 0 ? 0 : seconds; }
    int autosaveSeconds() const { return autosaveSeconds_; }

    // **Where the player is and what time the world thinks it is.**
    //
    // Held here and written into level.dat by whatever saves next -- the
    // autosave timer, the pause menu, or close(). Before this, level.dat was
    // only ever rewritten with its LastPlayed changed, so a world always
    // reopened at the position it was first entered at and at the time it was
    // created; the fields were read at open and never written back.
    //
    // Call it every frame; it costs four stores. A caller that never calls it
    // leaves the stored player exactly as it was, which is what the harnesses
    // want and what a world nobody has stood in must keep getting.
    //
    // `timeTicks` is absolute rather than a time of day: level.dat's `Time` is
    // a running tick count and the day is `Time % 24000`, so storing the
    // remainder would throw away which day it is every time the world was
    // saved.
    void setPlayerState(double x, double y, double z, float yaw, float pitch, i64 timeTicks);

    // Once a frame, with the wall clock. Separate from update() so the existing
    // signature and its harness callers stay put, and so core keeps having no
    // clock seam -- the value is passed in, exactly as open() and close() do.
    void tickSaves(i64 nowMillis);

    // Hands every dirty column to the I/O thread. **Blocking is for the way
    // out**; the pause menu wants the other one, because the world is stopped
    // while the menu is up and an async flush is finished before the player
    // resumes without anything having waited.
    void flushSaves(bool blocking);

    // **Save now**: everything the autosave timer would have done, at a moment
    // of the caller's choosing, and without blocking. What the pause menu
    // calls. It also restarts the interval, so resuming does not immediately
    // trip an autosave over work that has just been written.
    void saveNow(i64 nowMillis);

    // True when nothing is queued for the card either. What close() and the
    // tests wait on.
    bool storageIdle() const { return cache_.idle(); }

    // Who creates the generation worker, and on which core, is
    // `mc::setWorkerThreadOps` in core/util/worker.hpp. It used to live here as
    // a pair of statics on this class; it moved out when the chunk cache gained
    // a thread of its own, because the two want opposite cores and the platform
    // has to be able to tell them apart. See WorkerRole.

    // True when nothing is queued, nothing is being generated and nothing is
    // waiting to be taken into the grid. What a test or a harness waits on;
    // the game never needs it.
    bool generationIdle() const;

    int meshDistance() const { return meshDistance_; }

    // Changes the render distance without giving the world back to the SD card.
    //
    // The grid wraps modulo its own edge, so its width is part of how a column
    // is addressed and a new radius means a new grid. Columns still inside the
    // new radius are moved across rather than reloaded -- re-reading them costs
    // seconds on a console, and this is a setting a player is expected to try
    // both ways.
    //
    // **Reset the renderer first.** Its field is sized to the same radius, and
    // everything moved across is republished into whatever field it finds, so
    // calling this against the old one would publish into a grid that is about
    // to be thrown away.
    void setMeshDistance(int meshDistance, ChunkRenderer& renderer);

    // Switches the cube encoding, which means re-meshing everything: a section
    // already in the pool is in the old format and the draw loop would read its
    // bytes as the new one. Nothing is re-read from the SD card -- the columns
    // stay where they are and only their geometry is rebuilt.
    //
    // **Reset the renderer's pool first**, for the same reason setMeshDistance
    // wants its field reset first: this republishes into whatever it finds.
    void setCubeFormat(mesh::CubeFormat format, ChunkRenderer& renderer);

    mesh::CubeFormat cubeFormat() const { return builder_.cubeFormat(); }

    bool isOpen() const { return open_; }
    const world::LevelData& level() const { return level_; }
    const std::string& path() const { return path_; }

    // Where the player starts: their saved position, or the spawn point in a
    // world a server made and no player has entered.
    // Double, because a spawn point can be anywhere in a +-32,000,000 block
    // world and a float stops being able to name individual blocks past
    // 16,777,216. Matches level.dat, which stores the player position as
    // TAG_Double, and the camera, which now holds it as one.
    void spawnPosition(double* x, double* y, double* z) const;

    // Loads, drops, meshes and uploads within the budget. Call once per frame
    // after the renderer's beginFrame, so the mesh queue reflects this frame's
    // view.
    void update(ChunkRenderer& renderer, i32 cameraChunkX, i32 cameraChunkZ,
                const Budget& budget);

    const Stats& stats() const { return stats_; }

private:
    enum class CellState : u8 {
        Empty,        // never asked about
        OnDisk,       // the world has this chunk; it has not been read in yet
        Loaded,       // block data in memory
        Absent,       // the world has no such chunk and none is coming
        Ungenerated,  // the world has no such chunk *yet*: it is the work queue
    };

    // **The two "not here" states are not interchangeable.** `Absent` is the
    // edge of a finite world, and a column next to one may be meshed -- waiting
    // for a chunk that will never arrive would leave a permanently unmeshed
    // ring. `Ungenerated` is a chunk that is on its way, and meshing against it
    // would cull faces against air and never come back to fix them. Which one a
    // cell gets depends only on whether there is a generator.
    //
    // ---------------------------------------------------------------------
    // **Asking the world whether it has a chunk is separated from reading it,
    // and that separation is what makes the generated world independent of how
    // fast the generator runs.**
    //
    // The question "is this chunk on the card" has a different answer before
    // and after a sweep writes it, so if the streamer asks it late it gets an
    // answer that depends on the clock -- and the answer decides whether the
    // column is generated, which decides the population order, which *is* the
    // world. Measured, before this: the same path produced 36 columns of world
    // with generation on a worker and 48 with it inline, and the two worlds
    // disagreed from the second column onwards.
    //
    // So every cell is classified once, and the classification is arranged to
    // happen before any sweep could have written it:
    //
    //   * the cell grid is **three rings wider than the load radius**, which is
    //     exactly the reach of a sweep, so every column the generator can write
    //     has a cell of its own to be classified in;
    //   * nothing is queued for generation until the whole grid is classified,
    //     so the first sweep cannot outrun the questions; and
    //   * a cell newly exposed by the camera moving was, one chunk ago, outside
    //     the reach of every target that existed then -- so it, too, is asked
    //     about before anything can have written it.
    //
    // What is left is a queue whose *contents* are built from the camera's path
    // and nothing else, consumed one column at a time. Which entry is taken
    // next is the nearest to the camera rather than the oldest -- see
    // pumpGeneration -- so the order, unlike the membership, does depend on how
    // far behind the generator got. That is a deliberate trade and the reason
    // for it is in that note.
    // ---------------------------------------------------------------------

    struct Cell {
        std::unique_ptr<world::ChunkColumn> column;
        mesh::SectionVisibility masks[world::ChunkColumn::kSectionCount];
        i32 chunkX = 0;
        i32 chunkZ = 0;
        CellState state = CellState::Empty;
        bool published = false;  // handed to the renderer, i.e. its neighbours arrived

        // **This column arrived since the renderer last heard about it**, so
        // whatever the renderer still holds under these coordinates is from a
        // previous visit and must not be kept.
        //
        // The renderer keeps a column's meshes when the same column is
        // published again -- that is what a neighbour arriving should do -- and
        // it decides "same" by comparing coordinates, which is the only thing
        // it can see. A column that left the render distance, was dropped from
        // this grid while it was out there, and has now been read back in has
        // the same coordinates and none of the same meshes: their pool slots
        // were handed out again long ago. Without this flag it is republished
        // as "the same column", keeps slots that now belong to other sections,
        // and draws whatever is in them.
        bool freshlyAdopted = false;
    };

    // Sizes cells_ and spiral_ to loadRadius_. Shared by open() and
    // setMeshDistance(), which is the only reason it is a function.
    void buildGrid();

    // Recounts what the grid holds. Cheap -- one pass over at most 729 cells --
    // and called wherever residency can have changed without update() running.
    void countResidency();

    int cellIndex(i32 chunkX, i32 chunkZ) const;
    Cell* find(i32 chunkX, i32 chunkZ);
    const Cell* find(i32 chunkX, i32 chunkZ) const;

    // Asks the world whether it has this chunk, and records the answer. One
    // cheap existence check; the column itself is read later and only inside
    // the load radius. See the note on CellState for why this is its own step.
    void classifyCell(Cell& cell, i32 chunkX, i32 chunkZ);

    // What one attempt at a column found. `Pending` is the cache having posted
    // a read: nothing is wrong, the column is on its way, and the cell is asked
    // about again next frame. It must not spend the per-frame budget, or one
    // column still being read would hold up every other one behind it.
    enum class LoadResult { Loaded, Pending, Failed };

    // Takes a column the world does have, from the cache. Never touches a card.
    LoadResult loadColumn(i32 chunkX, i32 chunkZ);

    // Makes the chunk the world does not have, and everything the sweep
    // finishes on the way. **Runs on the worker thread** when there is one, and
    // on the caller's when there is not.
    bool generateColumn(i32 chunkX, i32 chunkZ);

    // Hands the head of the generation queue to the worker, or makes it inline
    // when there is none.
    void pumpGeneration(const Budget& budget);

    // Appends a column to the generation queue, once.
    void enqueueGeneration(i32 chunkX, i32 chunkZ);

    // Removes and returns the queued column nearest the camera. **queueLock_
    // held**; the worker calls it as well as the main thread. See the note on
    // pumpGeneration for why it is nearest rather than oldest.
    std::pair<i32, i32> takeNearestQueuedLocked();

    // Lets the worker take jobs again after waitForWorkerIdle() stopped it.
    void resumeGeneration();

    // Once per chunk-boundary crossing, not once per frame: the set of cells
    // worth warming or reading ahead only changes when the centre does, and
    // both are a lock and a lookup per cell.
    void warmAndPrefetch();

    bool startWorker();
    void stopWorker();
    void waitForWorkerIdle();
    void workerMain();
    static void workerEntry(void* self);

    // Takes the columns the worker has finished into the grid. Main thread.
    void drainGenerated(ChunkRenderer& renderer);

    // Puts a finished column into the grid and into the save. Shared by the
    // loaded and the generated paths, because from here on they are the same
    // column.
    void adoptColumn(Cell& cell, std::unique_ptr<world::ChunkColumn> column);

    // ChunkGenerator::Store, bound to this streamer.
    static const world::ChunkColumn* generatorLoad(void* context, i32 chunkX, i32 chunkZ,
                                                   world::ChunkColumn* scratch);
    static void generatorDeliver(void* context, world::ChunkColumn& column);

    void dropCell(Cell& cell, ChunkRenderer& renderer);

    // A column can only be meshed once its eight neighbours are in memory.
    // Publishing is what tells the renderer it may be walked into and meshed.
    bool neighboursReady(i32 chunkX, i32 chunkZ) const;
    void publishIfReady(i32 chunkX, i32 chunkZ, ChunkRenderer& renderer);

    bool meshSection(const VisibleSection& section, ChunkRenderer& renderer);

    io::PosixFileSystem fs_;

    // **The only thing here that reaches a card.** Reads, writes, existence and
    // the retention ring all go through it, and the render thread's half of its
    // API never blocks on storage. See core/world/chunk_cache.hpp -- in
    // particular the note on why deferring a write changes no answer the
    // generator or the classification can observe.
    world::ChunkCache cache_{fs_};

    // Null unless generation is on. Roughly 900 KB of generator plus a cache
    // sized to the load radius -- see ChunkGenerator::cacheColumnsFor -- so it
    // is built at open() and only when it is wanted.
    std::unique_ptr<mcver::ChunkGenerator> generator_;
    bool generateMissing_ = false;

    // The column a generated one is built into before it is adopted. Held here
    // because Store::deliver hands over a reference the generator reuses, and
    // the grid wants an owned one. Worker thread only.
    std::unique_ptr<world::ChunkColumn> generated_;

    // ---------------------------------------------------------------------
    // The worker, and what the lock covers.
    //
    // `queueLock_` guards the job slot and the finished-column queue, and is
    // held for a pointer swap at a time. The storage slot used to have a second
    // lock here, taken by the main thread and the generation worker alike; it
    // moved inside ChunkCache along with everything that touches a card, which
    // is what took the render thread off the storage path entirely.
    // ---------------------------------------------------------------------
    bool generationThreaded_ = true;
    // One of these holds the worker, never both: `platformWorker_` when a
    // platform supplied its own creation, `worker_` otherwise.
    std::thread worker_;
    void* platformWorker_ = nullptr;
    mutable std::mutex queueLock_;
    std::condition_variable wake_;
    std::condition_variable idle_;

    bool workerRunning_ = false;
    bool workerStop_ = false;
    // **What is owed. The scan appends in spiral order; the pump takes the
    // entry nearest the camera.**
    //
    // This was strictly FIFO, and the reasoning was that population order is
    // the world in a1.1.2 -- two chunks whose passes reach the same ground come
    // out differently depending on which ran first -- so the sequence of sweeps
    // had to be a property of the game rather than of how fast the generator
    // ran. What that bought was a *reproducible* order. It was not the
    // original's, and it stranded a player who outran the generator behind
    // every column they had already passed; see pumpGeneration for the numbers
    // and for the jar evidence that a1.1.2 generates nearest-to-the-player.
    //
    // Two properties survive the change and are worth keeping true:
    //
    //   * **Nothing is dropped.** A coordinate that has gone out of range is
    //     generated anyway rather than skipped -- it is simply taken after the
    //     ones the player can see. The set of columns the world ends up with is
    //     still a function of the camera path alone.
    //   * **One at a time.** Job N still starts against the world left by every
    //     job before it; only which column is job N has changed.
    std::vector<std::pair<i32, i32>> generationQueue_;

    // What is on the queue, for a membership test the queue itself cannot give
    // cheaply. **Not a flag on the cell**: the grid wraps, so a cell is recycled
    // by whatever column lands on it next and a flag on it is lost the moment
    // the camera moves far enough. A column that came back and was queued a
    // second time put a second sweep into the sequence, and the sequence is the
    // world.
    std::set<std::pair<i32, i32>> queued_;

    // Coordinates the worker has finished, handed back so the main thread can
    // take them out of `queued_` exactly once. A list rather than a single
    // slot: the worker takes its own next job, so more than one column can
    // finish between two frames.
    std::vector<std::pair<i32, i32>> completed_;

    // The camera position the worker picks against, republished once a frame.
    // A copy under the lock rather than `centreX_`, which is the main thread's.
    i32 queueCentreX_ = 0;
    i32 queueCentreZ_ = 0;

    // Stops the worker taking a new job. Held while something on the main
    // thread reaches into the generator itself -- growing its cache moves a
    // table the worker walks.
    bool queuePaused_ = false;

    // A player who runs across ungenerated ground can queue faster than the
    // worker drains. The cap is what stops that being unbounded; it is a
    // function of the grid size, so it is the same on every machine.
    usize generationQueueCap_ = 0;

    bool jobActive_ = false;
    std::vector<std::unique_ptr<world::ChunkColumn>> finished_;

    // The generator's counters, copied out by the worker under queueLock_. The
    // generator itself knows nothing about threads and should not have to.
    u32 workerPeakLive_ = 0;
    u32 workerEvictedLive_ = 0;

    world::LevelData level_;
    std::string path_;
    bool open_ = false;

    world::ChunkCache::Config cacheConfig_;
    world::ChunkCache::PlayerState player_;
    int prefetchRings_ = 0;
    int autosaveSeconds_ = 0;
    i64 lastSaveMillis_ = 0;

    int meshDistance_ = 0;
    int loadRadius_ = 0;

    // The cell grid, three rings wider than the load radius: that is a sweep's
    // reach, and every column the generator can write needs somewhere to be
    // classified. Cells beyond loadRadius_ are classification only -- never
    // read in, never published.
    int gridRadius_ = 0;

    // Cells still to be asked about. Nothing is queued for generation until
    // this reaches zero; see the note on CellState.
    int unclassified_ = 0;
    int edge_ = 0;
    i32 centreX_ = 0;
    i32 centreZ_ = 0;
    bool centreSet_ = false;
    std::vector<Cell> cells_;

    // Chunk offsets sorted by distance from the centre, built once. Loading
    // walks it in order, so the world fills in from the player outwards without
    // sorting anything per frame.
    struct Offset {
        i16 dx, dz;
    };
    std::vector<Offset> spiral_;

    // Reused across sections; 17.5 KB and 12 KB respectively, which is why they
    // belong to the object rather than to a stack frame.
    mesh::MeshScratch scratch_;
    mesh::VisibilityScratch visScratch_;
    mesh::MeshBuilder builder_;

    // The two vertex streams laid end to end for the upload. Kept here so it
    // grows to the largest mesh once and never reallocates again.
    std::vector<u8> staging_;

    Stats stats_;
};

}  // namespace mc::render
