#pragma once

#include "core/entity/persistence.hpp"

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

#include "core/gui/progress.hpp"
#include "core/item/use.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/scratch.hpp"
#include "core/mesh/visibility.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/worker.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/level_data.hpp"
#include "core/world/light_update.hpp"
#include "version_slots.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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

        // **The radius columns are actually being admitted to**, which is
        // loadRadius() until the memory budget starts biting and less after
        // that. Equal to loadRadius() is the healthy reading; below it means
        // the world in view is shorter than the render distance asks for, and
        // that the alternative was running the heap out. See setMemoryBudget.
        int admitRadius = 0;

        // Columns dropped to get back under the memory budget, cumulative.
        // **Zero is not luck**: below the budget this cannot fire at all.
        u32 evictedForMemory = 0;

        int generatedThisFrame = 0;   // columns the worker finished and handed back
        int adoptedThisFrame = 0;     // ...of which this many landed in the grid
        int pendingGeneration = 0;    // in range, not on the card, not made yet

        // **Why a column that is owed is not being made**, which
        // `pendingGeneration` alone cannot say. Either the worker is simply
        // behind -- owed high, nothing gated -- or the nearest columns cannot
        // be started at all because the cells a sweep would reach have not been
        // classified yet, which is `generationGated` and is measured in
        // `unclassified`. Without the split, a stalled world and a slow one
        // look the same from the debug page.
        //
        // There is no queue depth here any more because there is no queue: what
        // is owed is read off the grid every frame. See refreshSlate.
        bool generationGated = false;  // the nearest owed columns are waiting on a listing
        int unclassified = 0;          // cells in the grid still waiting to be asked about

        // **Sweeps that could not be finished at all**, which is the one way
        // generation can stop without anything else looking wrong: the nearest
        // owed column is retried every frame and fails every frame, so the
        // frontier never advances. Must read zero; a non-zero one means the
        // generator's cache could not hold a sweep.
        u32 generationFailures = 0;
        // ...and which of the two it was. `generationUnlightable` is the one
        // that reported itself as the other for a whole hardware session; see
        // ChunkGenerator::Stats.
        u32 generationUnlightable = 0;
        u32 generationIncomplete = 0;
        i64 generateMicros = 0;       // main-thread cost only: 0 while the worker runs
        u32 generatorPeakLive = 0;    // the generator's own high-water mark
        u32 generatorEvictedLive = 0; // must stay zero; see ChunkGenerator

        // Live columns retired because the player walked away from them. Not a
        // fault -- it is the thing that keeps `generatorEvictedLive` at zero --
        // but it grows with the distance walked, so it is worth seeing next to
        // it. See ChunkGenerator::retire.
        u32 generatorRetiredLive = 0;
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

    // Closing a world writes everything it owes the card, which on a folder
    // world is a file per dirty column and long enough to look like a hang.
    //
    // `progress` is called while that drains, with how many columns are written
    // and how many were owed, so a caller can put a number in front of the
    // player. It is called at least once even when nothing is owed -- that is
    // the "nothing to do" answer rather than the absence of one -- and the
    // wait happens here rather than inside the cache's own blocking close so
    // there is something to report during it.
    using SaveProgressFn = void (*)(void* context, u32 written, u32 owed);
    void close(i64 nowMillis, void* progressContext = nullptr,
               SaveProgressFn progress = nullptr);

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

    // What the player is carrying, for the next save to write.
    //
    // **Separate from setPlayerState, and called far less often.** That one is
    // four stores a frame; this copies up to forty stacks and the NBT each one
    // preserved, so it belongs on the edit rather than on the frame. Call it
    // when the hand or the backpack actually changes -- picking from the
    // palette, moving a stack, cycling is not a change.
    //
    // A world never told about an inventory saves the one it was loaded with,
    // untouched. That is what Spectator does and it is deliberate: a mode with
    // no hand must not empty a hand the real client filled.
    void setPlayerInventory(const std::vector<item::ItemStack>& stacks);

    // Bind once after opening; restore saved pools before their first tick.
    // The pools must outlive close(). Saves copy them before queuing I/O.
    void bindEntities(const entity::EntityPools& pools);

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
    // calls -- and only when `dirtyColumns()` says there is something to write,
    // so pausing twice in a row costs one save. It also restarts the interval, so resuming does not immediately
    // trip an autosave over work that has just been written.
    void saveNow(i64 nowMillis);

    // True when nothing is queued for the card either. What close() and the
    // tests wait on.
    bool storageIdle() const { return cache_.idle(); }

    // How many columns are dirty *now*, taken from the cache rather than from
    // the once-a-frame copy in stats(). Two callers want it outside the frame
    // loop, where that copy is stale: the pause menu, which saves only what is
    // owed, and the save screen, which counts down against it.
    u32 dirtyColumns() const;

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

    // Greedy meshing on or off; see MeshBuilder::setGreedy. Both kinds of mesh
    // are drawn the same way, so a pool holding a mixture is correct -- but a
    // setting that only reached new sections would leave the A/B it exists for
    // reading half of each, so this re-meshes everything the way
    // setCubeFormat does, and wants the pool reset first for the same reason.
    void setGreedy(bool on, ChunkRenderer& renderer);

    bool greedy() const { return builder_.greedy(); }

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

    // **A column that is resident right now, or null.** The one read-only door
    // into the grid, and it exists for the bottom screen's map: the map is a
    // projection of blocks the game already has in memory, and re-reading them
    // from the card to draw a picture of them would be absurd.
    //
    // Main thread only, and the pointer is good only until the next `update()`
    // -- the grid drops columns that leave the radius, and the generation
    // worker's finished columns are adopted in there. Nothing may hold it
    // across a frame.
    //
    // **Loaded, not published**, and the difference is the whole reason the map
    // came up blank. `published` means the renderer has the column, which needs
    // its eight neighbours to have arrived *and* the column to be inside the
    // mesh distance. Neither is anything to do with a map: a sample reads one
    // column's surface, heights and depths and asks nothing of its neighbours,
    // and the map window reaches seven chunks where the mesh distance can be as
    // little as two. Gating on it meant that at world entry -- when almost
    // nothing has been published yet -- there was nothing to sample however
    // large the budget was, and that at the smaller render distances the
    // outermost ring of the map was permanently unexplored ground.
    //
    // A `Loaded` cell is a complete, lit column: it came either from the card
    // or from the generator, and both hand over finished work. That is the
    // honest gate.
    const world::ChunkColumn* residentColumn(i32 chunkX, i32 chunkZ) const;

    // ---- the world tick ------------------------------------------------
    //
    // **Why the tick lives here and not beside the camera.** It needs the
    // loaded columns, it has to invalidate the sections it changes, and it has
    // to tell the saver what it dirtied -- and this object is the only one
    // holding all three. The tick logic itself is in `core/tick/` and knows
    // nothing about streaming; this is the wiring.
    //
    // Runs `ticks` whole 20 Hz steps, which is what `TickTimer::elapsedTicks`
    // returned for the frame. Zero is the common case at 30 fps and costs a
    // compare. **On the main thread, deliberately**: the order of block
    // updates is the world, the same way generation order is, and moving the
    // tick to another core is a decision to be taken with a measurement in
    // hand rather than on the way past. What could go to core 2 later is the
    // read-only half -- sampling the 80 positions per chunk -- and the seam
    // for it is `tick::TickWorld`, which reads chunks through a pair of
    // function pointers for exactly that reason.
    void stepTicks(ChunkRenderer& renderer, int ticks);

    // **A player's edit, which is a tick's edit made outside a tick.**
    //
    // Everything a block change has to set off is already built: setting it
    // through `TickWorld` notifies the neighbours, schedules the block's own
    // update, and reaches the falling sand and the fluid flow that
    // core/tick/ already implements. What it does *not* do on its own is
    // redraw, and that is the trap this method exists to close.
    //
    // `TickWorld`'s change callback only invalidates renderer sections while
    // `tickRenderer_` is set, and `stepTicks` is the only thing that sets it.
    // An edit made straight from the input handler would mark its column dirty,
    // queue its lighting, and then be **invisible** until something else
    // happened to touch that section. So this brackets the write exactly the
    // way stepTicks brackets a tick, and drains the light the edit queued while
    // the renderer is still in hand.
    //
    // False when the position is outside the world or its column is not
    // resident -- an edit at the edge of the loaded grid is dropped rather than
    // written into a column that is about to be replaced.
    bool setBlock(ChunkRenderer& renderer, i32 x, int y, i32 z, block::BlockId id, u8 metadata);

    // **A player's right-click**, with the renderer held for the whole of it
    // rather than for each write.
    //
    // The same bracket as `setBlock` and for the same reason, but around the
    // *decision* instead of around one cell: one click can be several writes --
    // a door is two blocks, a door being opened is two metadata changes, a
    // lever is a write and four notifications -- and every one of them has to
    // reach the renderer. Bracketing per write would also drain the light queue
    // once per cell, which is work the click has not caused yet.
    //
    // The decision itself is `item::rightClick`, in core and under test; this
    // is the two lines of it that need a renderer.
    bool rightClick(ChunkRenderer& renderer, item::ItemId held, const entity::RayHit& hit,
                    const AABB& playerBox, float yawDegrees, const item::Effects& effects = {});

    // **The item's own right-click**, under the same renderer bracket, and it
    // is a second entry point rather than a flag on the one above because
    // `Minecraft.clickMouse` has two: the block's, which needs a hit, and the
    // item's, which does its own ray. See `item::useItem`.
    //
    // The residency test that `rightClick` does up front cannot be done here --
    // there is no hit yet to test the column of -- so it is done against
    // whatever the item's own ray lands on, inside `item::useItem`'s writes:
    // `TickWorld` refuses a write to a column it does not hold, which is the
    // same guard reached one layer down.
    item::ItemUse useItem(ChunkRenderer& renderer, item::ItemId held, double eyeX,
                          double eyeY, double eyeZ, double dirX, double dirY, double dirZ,
                          const item::Effects& effects = {});

    // The left hand: `PlayerController.onPlayerDestroyBlock`, under the same
    // bracket as everything else here. Particles first, then the removal, then
    // the sound -- `item::destroyBlock` owns that order and this owns the
    // renderer.
    bool breakBlock(ChunkRenderer& renderer, i32 x, int y, i32 z,
                    const item::Effects& effects = {});

    // The incremental relighter, for the debug page. Null before a world opens.
    const world::LightUpdater* lighting() const { return light_.get(); }

    tick::TickWorld* worldTick() { return tick_.get(); }
    const tick::TickWorld* worldTick() const { return tick_.get(); }

    // Columns a tick has changed and the saver has not been told about yet.
    // Flushed on the autosave boundary rather than per frame, because a
    // spreading fluid touches one column hundreds of times in a second and
    // each hand-over is an 18 KB clone.
    u32 tickDirtyColumns() const { return tickDirtyCells_; }

    // **The world coming into being, as a picture.** One `gui::ChunkState` per
    // cell of a square centred on `centreX`/`centreZ`, row-major from the
    // north-west corner, so the caller can draw it with
    // `gui::drawChunkGrid` and north is up.
    //
    // It emits the drawing enum rather than one of its own, and that is the
    // deliberate half of this. The grid's `CellState` is private and has to
    // stay that way -- `Ungenerated` versus `Absent` is a meshing rule, not
    // something a screen may act on -- so the alternative was a second public
    // enum that exists only to be switched over into the first, in every caller
    // that draws one. Two enums to keep in step, for no information neither of
    // them carries. See core/gui/progress.hpp for what the five states mean.
    //
    // Main thread only, and it takes the generation lock once for the whole
    // square rather than once per cell: which column the worker has in hand is
    // the one thing here that another thread writes.
    //
    // Cells outside the grid come back `Unstarted`, which is the truth about
    // them from this streamer's point of view -- nothing has been asked and
    // nothing is owed.
    void progressGrid(i32 centreX, i32 centreZ, int radius, gui::ChunkState* out) const;

    // **How much of the square is finished**, counted the same way and over the
    // same cells. `total` is every cell in the square; `done` is the ones a
    // player can see. Its own call because the bar wants the numbers without
    // the picture -- the top screen has no framebuffer to draw a square into.
    struct ProgressCount {
        int done = 0;
        int total = 0;
        bool finished() const { return total > 0 && done >= total; }
    };
    ProgressCount progressWithin(i32 centreX, i32 centreZ, int radius) const;

    // The radius columns are actually held out to: one ring wider than the
    // render distance, because a section cannot be meshed until its eight
    // neighbours are in memory. **It is the ceiling on anything that waits for
    // the world to fill in** -- a cell further out than this is classified and
    // never read, so waiting for it would never end.
    int loadRadius() const { return loadRadius_; }

    // **A ceiling in bytes on what the resident columns may cost**, and the
    // backstop under the render distance.
    //
    // Residency is otherwise purely geometric: a cell is held if it is inside
    // the radius and dropped when it leaves, and nothing anywhere asks what
    // that costs. That is fine over ordinary ground and it is not fine
    // everywhere -- measured through the generator, a column costs 14.0 KB over
    // ordinary terrain and 21.7 KB at the Far Lands, so the same grid that fits
    // in 33 MB at render distance 24 needs 51 MB out there. The heap does not
    // grow to match, `operator new` fails, and with -fno-exceptions that is
    // `abort()` and a console dropped to the HOME menu with nothing to say for
    // itself. See crashlogs/006 and 007.
    //
    // So the render distance becomes a request rather than a promise. When the
    // resident bytes go over this, the admission radius shrinks a ring at a
    // time and everything outside it is dropped, furthest first; when they fall
    // comfortably under, it grows back. **The player gets a shorter view at the
    // Far Lands instead of a dead console**, and `Stats::admitRadius` says so
    // rather than leaving it to be guessed at.
    //
    // Zero disables it, which is the default and what every host measurement
    // runs with: a budget that bit mid-run would move numbers that documented
    // tables are written against.
    void setMemoryBudget(usize bytes);

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
    //     has a cell of its own to be classified in; and
    //   * **no column is generated until every cell a sweep for it could reach
    //     has been classified** -- the 7x7 around it, which is those same three
    //     rings. See classifiedAround.
    //
    // That second rule used to be "until the whole grid is classified", which
    // needed the answer for every cell to be available in the frame the cell
    // was exposed, which needed a `stat` on the render thread. The `stat` takes
    // the storage lock, the I/O thread holds it for the length of a chunk
    // write, and a row of newly exposed cells behind a flush is the second the
    // game stops for. The narrower rule is the same guarantee -- a sweep cannot
    // reach past three rings, so a cell further out than that cannot be one the
    // sweep silently fills -- and it lets an unclassified cell simply wait for
    // its group listing without stopping anything else.
    //
    // What is left is a set of owed columns read off the grid itself, asked for
    // nearest-first. It is not a queue and there is nothing in it that the grid
    // does not already say; see the note on slate_ for why the queue that used to
    // be here was doing harm.
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

        // **A tick has written into this column since it was last handed to the
        // cache.** Replaces a `std::vector` of coordinates that the block-change
        // callback scanned linearly for every changed block: the comment on it
        // said the set was "what one frame's ticks touched", but it was only
        // cleared at the autosave boundary, so it accumulated every column
        // touched since the last save -- and with autosave set to Off it was
        // never cleared at all. A flowing fluid changes hundreds of blocks a
        // tick, so the scan was quadratic and got worse the longer a session
        // ran. A flag on the cell is O(1), allocates nothing, and cannot grow.
        //
        // It also gives dropCell somewhere to look: a column that leaves the
        // grid with this set has edits the card has never seen.
        bool tickDirty = false;
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

    // One cell of progressGrid/progressWithin, with the worker's job already
    // read out under the lock -- `job` is the column it has in hand, or null.
    // Not `find()`, because `find` folds "no such cell" and "cell never asked
    // about" into one null and the square has to tell them apart.
    gui::ChunkState progressAt(i32 chunkX, i32 chunkZ, const std::pair<i32, i32>* job) const;

    // Asks the world whether it has this chunk, and records the answer. One
    // cheap existence check; the column itself is read later and only inside
    // the load radius. See the note on CellState for why this is its own step.
    //
    // **It can decline to answer**, and that is not a failure: the cache says
    // Unknown while the group listing it needs is still on its way, the cell
    // stays Empty, and the next frame asks again. Nothing here ever waits on a
    // card.
    void classifyCell(Cell& cell, i32 chunkX, i32 chunkZ);

    // Is everything a sweep for this column could touch classified?
    //
    // The 7x7 around it, because ChunkGenerator::provide sweeps terrain over
    // (cx-3..cx+2) and delivers finished columns from inside that. A cell in
    // there still Empty is one the sweep could fill before anybody asked
    // whether the world already had it -- and that answer decides whether the
    // column is generated, which decides the population order, which is the
    // world. So the sweep waits instead, which costs a frame or two of the
    // frontier and costs the player nothing.
    bool classifiedAround(i32 chunkX, i32 chunkZ) const;

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

    // Tells the generation thread where the player is, so it can let go of the
    // world behind them. Main thread, under queueLock_.
    void publishRetireCentre();

    // Makes the columns on the slate, if there is no worker to make them.
    void pumpGeneration(const Budget& budget);

    // Rewrites the slate from the grid: the nearest owed columns, in spiral
    // order, as of this frame. Main thread, once a frame, under queueLock_.
    void refreshSlate();

    // Takes the next column off the slate. **queueLock_ held**; the worker
    // calls it as well as the main thread.
    bool takeSlateLocked(std::pair<i32, i32>* out);

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

    // Every loaded column back through the publish gate, so all of it is
    // meshed again: the common half of setCubeFormat and setGreedy.
    void republishAll(ChunkRenderer& renderer);

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
    // ---------------------------------------------------------------------
    // **The slate: the nearest few columns that are owed, rewritten from the
    // grid every frame. There is no queue.**
    //
    // There was one, and it was doing two jobs badly. The first was ordering,
    // and it lost that argument already: strictly FIFO stranded a player who
    // outran the generator behind every column they had passed, so it became
    // nearest-first (the order a1.1.2 actually generates in -- `e` at bytecode
    // 686 sorts the renderers by distance before rebuilding them). The second
    // was membership, and that is what this replaces. **A queue of coordinates
    // is a second copy of something the grid already knows**, and every
    // property it needed was a defence of that copy: a cap so a sprinting
    // player could not grow it without bound, a refused counter for when the
    // cap bit, a membership set so nothing was queued twice, a stale count for
    // the entries that were no longer in range. A measured sprint at distance 8
    // ended with 702 columns on it, 341 of them ground the camera had left.
    //
    // The grid answers all of it for free. A cell inside the load radius in
    // state `Ungenerated` *is* a column that is owed; the spiral is already
    // sorted nearest-first, so the first few of them are the next few jobs;
    // walking out of range makes a cell somebody else's and takes its coordinate
    // out of the reckoning with it. Nothing to cap, nothing to dedupe, nothing
    // to go stale.
    //
    // What the slate is for is the gap between frames. The worker takes its own
    // next job the moment it finishes one -- see workerMain -- so it needs
    // somewhere to look that is not the grid, which is the main thread's. A
    // handful of entries is enough to keep it fed for a frame at any speed it
    // can actually generate.
    //
    // **What this gives up.** A column the camera passed too fast to reach is
    // no longer generated later; it is simply not generated until the player
    // comes back. That was the queue's one real property -- "the set of columns
    // the world ends up with is a function of the camera path alone" -- and it
    // is worth being plain that a1.1.2 does not have it either: `ft.b`
    // generates inline, for the chunk being asked for, and a chunk that never
    // comes into range is never asked for. So the set follows the camera on a
    // slower console too, and this is the more faithful of the two.
    // ---------------------------------------------------------------------
    static constexpr usize kSlateDepth = 8;
    std::vector<std::pair<i32, i32>> slate_;

    // The column the worker has in hand, and whether it has one. The main
    // thread reads it while rewriting the slate so a job in flight is not put
    // back on it -- which would be a second sweep for the same column, and the
    // sequence of sweeps is the world.
    std::pair<i32, i32> inFlight_{0, 0};

    // Coordinates the worker has finished, handed back so the main thread can
    // keep them off the slate until drainGenerated has put them in the grid --
    // between those two moments the cell still says `Ungenerated` and the
    // coordinate would otherwise look owed. A list rather than a single slot:
    // the worker takes its own next job, so more than one column can finish
    // between two frames.
    std::vector<std::pair<i32, i32>> completed_;

    // Stops the worker taking a new job. Held while something on the main
    // thread reaches into the generator itself -- growing its cache moves a
    // table the worker walks.
    bool queuePaused_ = false;

    bool jobActive_ = false;
    std::vector<std::unique_ptr<world::ChunkColumn>> finished_;

    // Sweeps that returned nothing, counted on whichever thread ran them and
    // read out under the same lock as the generator's own counters.
    u32 generationFailures_ = 0;

    // The generator's counters, copied out by the worker under queueLock_. The
    // generator itself knows nothing about threads and should not have to.
    u32 workerPeakLive_ = 0;
    u32 workerEvictedLive_ = 0;
    u32 workerUnlightable_ = 0;
    u32 workerIncomplete_ = 0;
    u32 workerRetiredLive_ = 0;

    // **Where the player is, for the generator's benefit**, published under
    // queueLock_ because the generator is the worker's and centreX_ is the main
    // thread's. generateColumn reads it and retires everything the player has
    // walked away from; without that the generator's live set grows with the
    // distance walked until it corrupts the world. See ChunkGenerator::retire.
    //
    // The radius travels with the centre rather than being read off
    // loadRadius_, for the same reason: the render distance is a live setting
    // and the worker must not read a number the main thread is changing.
    i32 retireCentreX_ = 0;
    i32 retireCentreZ_ = 0;
    int retireRadius_ = 0;
    bool retireCentreSet_ = false;

    world::LevelData level_;
    std::string path_;
    bool open_ = false;

    world::ChunkCache::Config cacheConfig_;
    world::ChunkCache::PlayerState player_;
    entity::EntityPools entityPools_;
    bool entitiesBound_ = false;
    void snapshotEntities();
    int prefetchRings_ = 0;
    int autosaveSeconds_ = 0;
    i64 lastSaveMillis_ = 0;

    int meshDistance_ = 0;

    // The world tick, created with the world and destroyed with it.
    std::unique_ptr<tick::TickWorld> tick_;

    // **Light after a block changes.** The tick writes blocks; nothing
    // recomputed light for them, so lava flowed through a world that stayed
    // dark. See core/world/light_update.hpp. Budgeted per frame rather than run
    // to completion, because a roof coming off relights a lot of cells at once
    // and a frame is 16.7 ms.
    std::unique_ptr<world::LightUpdater> light_;

    // Cells of light settled per frame.
    //
    // **A first estimate, and it is meant to be measured.** Each cell visits
    // six neighbours, and each of those is a column lookup, a palette decode
    // and a nibble read -- so this is the number that decides whether relighting
    // costs a frame. 1,024 is chosen to sit under a millisecond at a pessimistic
    // microsecond per cell on the ARM11; the debug page carries `light` so the
    // real figure replaces this one rather than being guessed at twice.
    static constexpr u32 kLightBudgetPerFrame = 1024;
    // Set during stepTicks so the block-changed callback can reach the
    // renderer. Null at every other moment, and asserted on.
    ChunkRenderer* tickRenderer_ = nullptr;
    // How many cells are carrying tick edits. Maintained alongside
    // Cell::tickDirty so the debug page costs nothing to draw.
    u32 tickDirtyCells_ = 0;

    // How often countResidency() adds up per-column memory usage, which is the
    // expensive half of it; see countResidency().
    static constexpr u32 kResidencyStride = 16;
    u32 residencyStride_ = 0;

    // **The budget is enforced on the strided pass and only there.** Adding up
    // `memoryUsage()` over every resident column is the expensive half of
    // countResidency(), which is why it already runs twice a second rather than
    // per frame -- and twice a second is far quicker than a player can walk
    // into tens of megabytes. Set when that pass ran, so enforcement never acts
    // on a figure from sixteen frames ago.
    bool blockBytesFresh_ = false;

    usize memoryBudget_ = 0;  // 0 disables; see setMemoryBudget

    // The radius columns are admitted to, clamped to [kMinAdmitRadius,
    // loadRadius_]. Shrinks a ring per over-budget pass rather than jumping, so
    // one heavy pass cannot collapse the view, and grows back only under the
    // low-water mark below -- **without which this thrashes**: evicting the
    // outer ring and immediately reading it back in is an eviction treadmill
    // that costs SD I/O for ever and frees nothing.
    int memoryRadius_ = 0;

    // Never shrink below this. A player must have ground under them and their
    // immediate neighbours whatever the accounting says; if the budget cannot
    // hold even this, no radius can help and the heap was mis-sized.
    static constexpr int kMinAdmitRadius = 3;

    // Grow back only under seven eighths, so a view sitting exactly on the
    // budget does not oscillate a ring every pass.
    static usize lowWater(usize budget) { return budget - budget / 8; }

    // loadRadius_, or less when the budget is biting. **Clamped on read rather
    // than trusted**: setMemoryBudget can be called before open(), when
    // loadRadius_ is still zero, and setMeshDistance moves loadRadius_ under a
    // radius that was chosen against the old one. Neither may be allowed to
    // admit nothing.
    int admitRadius() const
    {
        if (memoryBudget_ == 0) {
            return loadRadius_;
        }
        const int floor = kMinAdmitRadius;
        const int wanted = memoryRadius_ < floor ? floor : memoryRadius_;
        return wanted < loadRadius_ ? wanted : loadRadius_;
    }

    // Drops what is outside admitRadius(), furthest first, when the resident
    // bytes are over budget. Called from update() after countResidency().
    void enforceMemoryBudget(ChunkRenderer& renderer);

    static world::ChunkColumn* tickColumn(void* ctx, i32 chunkX, i32 chunkZ);
    static void tickBlockChanged(void* ctx, i32 x, int y, i32 z);
    static void lightSectionLit(void* ctx, i32 chunkX, int sectionY, i32 chunkZ);
    void flushTickDirty();
    int loadRadius_ = 0;

    // The cell grid, three rings wider than the load radius: that is a sweep's
    // reach, and every column the generator can write needs somewhere to be
    // classified. Cells beyond loadRadius_ are classification only -- never
    // read in, never published.
    int gridRadius_ = 0;

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

    // The same, one ring wider, for the group listings warmAndPrefetch asks
    // for. Nearest-first for the reason given where it is built: a deferred
    // classification is only cheap if the answers arrive in the order the cells
    // are needed.
    std::vector<Offset> warmSpiral_;

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
