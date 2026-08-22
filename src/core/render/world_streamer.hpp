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
#include "core/world/chunk.hpp"
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

    // **Who creates the worker, because on a New 3DS the answer decides whether
    // generation gets a whole core or the leftovers of one.**
    //
    // `std::thread` cannot name a core, and on the 3DS it does not merely fail
    // to: devkitARM's pthread shim hardcodes `threadCreate(..., 0x3F, 0, ...)`,
    // so every `std::thread` lands on **core 0 at the bottom priority**, beside
    // the render thread. The scheduler is strictly priority-ordered, so the
    // worker then runs only in whatever is left of the frame after the main
    // thread blocks -- which on a console holding 30 fps is a sliver, and it is
    // why a walking player outruns generation and never sees it catch up.
    //
    // A New 3DS has core 2 sitting idle and Luma's 3DSX exheader already grants
    // it (`0xFF002109`, "Access core2"), so a worker created with
    // `threadCreate(..., 2, ...)` gets 804 MHz to itself and competes with
    // nothing. Expressing that needs a seam, because it is exactly the thing
    // the portable API cannot say.
    //
    // `spawn` returns an opaque handle, or null if the thread could not be
    // started -- in which case the streamer falls back to generating inline,
    // the same as if `std::thread` had thrown. `join` is handed that handle
    // back, once, and must not return until the thread has finished. Set both
    // or neither, before open().
    //
    // This replaced a hook that ran *on* the worker and changed its priority
    // from the inside. That could never have reached the real problem: by then
    // the thread already exists on core 0, and a 3DS thread cannot move.
    using WorkerSpawn = void* (*)(void (*entry)(void*), void* arg);
    using WorkerJoin = void (*)(void* handle);
    static void setWorkerThreadOps(WorkerSpawn spawn, WorkerJoin join)
    {
        workerSpawn_ = spawn;
        workerJoin_ = join;
    }

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
    // What is left is a queue built from the camera's path and nothing else,
    // consumed strictly in order, one column at a time. Job N therefore starts
    // against the world left by jobs 1..N-1 on any machine, at any frame rate.
    // ---------------------------------------------------------------------

    struct Cell {
        std::unique_ptr<world::ChunkColumn> column;
        mesh::SectionVisibility masks[world::ChunkColumn::kSectionCount];
        i32 chunkX = 0;
        i32 chunkZ = 0;
        CellState state = CellState::Empty;
        bool published = false;  // handed to the renderer, i.e. its neighbours arrived
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

    // Reads a column the world does have. False if it could not be read.
    bool loadColumn(i32 chunkX, i32 chunkZ);

    // Makes the chunk the world does not have, and everything the sweep
    // finishes on the way. **Runs on the worker thread** when there is one, and
    // on the caller's when there is not.
    bool generateColumn(i32 chunkX, i32 chunkZ);

    // Hands the head of the generation queue to the worker, or makes it inline
    // when there is none.
    void pumpGeneration(const Budget& budget);

    // Appends a column to the generation queue, once.
    void enqueueGeneration(i32 chunkX, i32 chunkZ);

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
    mcver::Storage storage_{fs_};

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
    // The worker, and what each lock covers.
    //
    // `queueLock_` guards the job slot and the finished-column queue, and is
    // held for a pointer swap at a time. `storageLock_` guards the storage slot
    // itself, which both threads reach: the main thread reads the columns the
    // player is walking into, and the worker reads its own neighbourhood and
    // writes everything it finishes. Held for one chunk file at a time.
    //
    // They are never nested, in either direction. That is the whole deadlock
    // argument and it is worth keeping true.
    // ---------------------------------------------------------------------
    bool generationThreaded_ = true;
    // One of these holds the worker, never both: `platformWorker_` when a
    // platform supplied its own creation, `worker_` otherwise.
    std::thread worker_;
    void* platformWorker_ = nullptr;
    mutable std::mutex queueLock_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::mutex storageLock_;

    bool workerRunning_ = false;
    bool workerStop_ = false;
    // **The order columns are generated in, and it is deliberately not "whatever
    // is nearest now".**
    //
    // Population order is the world in a1.1.2 -- two chunks whose passes reach
    // the same ground come out differently depending on which ran first -- so
    // the sequence of sweeps has to be a property of the game rather than of how
    // fast the generator happens to be. Choosing the target afresh each time the
    // worker went idle made it the latter: a slower generator is further behind
    // when the camera moves on, so it picks a different column, and the world
    // that comes out is not the one a faster machine would have made. Measured,
    // before this queue existed: the same path produced 36 columns of world
    // threaded against 48 inline.
    //
    // So the scan appends to this in spiral order as it discovers missing
    // columns, and the worker consumes it in order. The queue is a function of
    // the camera path and the per-frame load budget; nothing about it depends
    // on how long a column takes to make. A coordinate that has since gone out
    // of range is generated anyway rather than skipped, for the same reason.
    std::vector<std::pair<i32, i32>> generationQueue_;

    // What is on the queue, for a membership test the queue itself cannot give
    // cheaply. **Not a flag on the cell**: the grid wraps, so a cell is recycled
    // by whatever column lands on it next and a flag on it is lost the moment
    // the camera moves far enough. A column that came back and was queued a
    // second time put a second sweep into the sequence, and the sequence is the
    // world.
    std::set<std::pair<i32, i32>> queued_;

    // The column the worker has in hand, and the flag that says it has finished
    // with it. The coordinate is kept on the main thread's side so that the
    // membership set can be cleared exactly once, when the job is drained.
    std::pair<i32, i32> inFlight_{0, 0};
    bool jobDone_ = false;

    // A player who runs across ungenerated ground can queue faster than the
    // worker drains. The cap is what stops that being unbounded; it is a
    // function of the grid size, so it is the same on every machine.
    usize generationQueueCap_ = 0;

    bool jobPending_ = false;
    bool jobActive_ = false;
    i32 jobX_ = 0;
    i32 jobZ_ = 0;
    std::vector<std::unique_ptr<world::ChunkColumn>> finished_;

    // The generator's counters, copied out by the worker under queueLock_. The
    // generator itself knows nothing about threads and should not have to.
    u32 workerPeakLive_ = 0;
    u32 workerEvictedLive_ = 0;

    static WorkerSpawn workerSpawn_;
    static WorkerJoin workerJoin_;
    world::LevelData level_;
    std::string path_;
    bool open_ = false;

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
