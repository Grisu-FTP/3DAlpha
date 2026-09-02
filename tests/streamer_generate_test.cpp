#include "framework.hpp"

#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/util/frustum.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
#include "version_slots.hpp"

#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace mc;
using render::ChunkRenderer;
using render::ChunkRendererConfig;
using render::VboAllocator;
using render::VboTier;
using render::WorldStreamer;
using mc::ClipRange;
using mc::Frustum;
using mc::Mat4;

namespace {

constexpr i64 kNow = 1284768000000LL;

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_gen_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }

    std::string world(const char* name) const { return std::string(path) + "/" + name; }
};

class TestAllocator : public VboAllocator {
public:
    void* allocate(usize bytes, VboTier) override { return std::malloc(bytes); }
    void release(void* pointer, usize, VboTier) override { std::free(pointer); }
};

Frustum openFrustum()
{
    Frustum f;
    f.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);
    return f;
}

// One frame of the console's loop, in the order the console runs it.
void frame(WorldStreamer& streamer, ChunkRenderer& renderer, u32 n, i32 cx, i32 cz,
           const WorldStreamer::Budget& budget)
{
    renderer.beginFrame(n, openFrustum(), cx, 4, cz);
    streamer.update(renderer, cx, cz, budget);
}

// **Frames, not iterations.** Generation runs on its own thread now, so a test
// that spins the loop a fixed number of times measures how fast this host can
// run an empty frame and nothing else. Running until the streamer says it has
// nothing outstanding is the only honest stop condition, and the sleep is what
// stops the loop from being a spin that starves the very thread it is waiting
// for.
//
// The frame cap is a deadlock guard rather than a budget: if it is ever
// reached, something has stopped making progress and the assertions that follow
// will say what.
int runUntilSettled(WorldStreamer& streamer, ChunkRenderer& renderer, i32 cx, i32 cz,
                    const WorldStreamer::Budget& budget, int maxFrames)
{
    int n = 0;
    for (; n < maxFrames; ++n) {
        frame(streamer, renderer, u32(n), cx, cz, budget);
        if (streamer.stats().pendingColumns == 0 && streamer.stats().pendingGeneration == 0
            && streamer.generationIdle()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return n;
}

}  // namespace

// **The seam M4 was waiting for**: a world with no chunks in it, and a streamer
// that fills it in.
//
// Everything below this has its own oracle -- the generator is compared column
// for column against a real a1.1.2 World in tests/generate_test.cpp. What this
// covers is the wiring, and the three things about it that can be wrong without
// anything else noticing: that a missing chunk reaches the generator at all,
// that what comes back is written to the save rather than only held, and that a
// generated column goes through the same publish gate a loaded one does and so
// actually reaches the renderer.
TEST(a_streamer_fills_an_empty_world_and_saves_what_it_makes)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Fresh");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 1234567890LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 2;
    config.budget = {0, 4 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    CHECK(streamer.open(dir.c_str(), 2, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    const int frames = runUntilSettled(streamer, renderer, 0, 0, budget, 20000);
    CHECK(frames < 20000);

    const WorldStreamer::Stats& stats = streamer.stats();

    // The generation really did happen on another thread. If it did not, the
    // rest of this still passes and the game still stutters, so it is asserted
    // rather than assumed.
    CHECK(stats.workerRunning);

    // Nothing was on the card, so every column came from the generator.
    CHECK(stats.columnsResident > 0);
    CHECK_EQ(stats.columnsMissing, 0);

    // The whole grid, and it is (2 * (2 + 1) + 1)^2 -- the load radius is one
    // ring wider than the render distance.
    CHECK_EQ(stats.columnsResident, 49);

    // The generator's own invariant, carried out to where a debug page can see
    // it. A non-zero count here means the cache was too small and columns came
    // back as bare terrain.
    CHECK_EQ(int(stats.generatorEvictedLive), 0);

    // **Written, not just held.** The generator evicts what it hands over and
    // reads it back through the streamer, so a column that never reached the
    // card would come back as bare terrain later. Reopening is the only honest
    // way to check that, since the in-memory grid would answer either way.
    streamer.close(kNow);

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == world::OpenResult::Ok);
        int found = 0;
        for (i32 x = -3; x <= 3; ++x) {
            for (i32 z = -3; z <= 3; ++z) {
                if (storage.hasChunk(x, z)) {
                    ++found;
                }
            }
        }
        CHECK_EQ(found, 49);

        // And they are real columns, not empty ones: a generated chunk has
        // bedrock at the bottom and sky at the top.
        world::ChunkColumn column;
        CHECK(storage.loadChunk(0, 0, &column));
        CHECK(column.terrainPopulated);
        CHECK_EQ(int(column.block(0, 0, 0)), 7);  // bedrock
        CHECK_EQ(int(column.block(0, 127, 0)), 0);
        CHECK(storage.close(kNow));
    }
}

// A second session over the same world must **read** it, not make it again.
// Regenerating would not merely be slow: population order follows the sequence
// of loads, so a second pass over the same ground is a different world.
// **What a save screen is told while it waits.**
//
// close() writes every dirty column, and on a card that is long enough to look
// like a hang -- so it reports as it drains. What matters is that the report is
// monotonic, that it ends at everything it started with, and that it happens at
// all when there is nothing to write, because "already saved" is the answer the
// pause menu's own save leaves behind and the shell prints it from here.
TEST(closing_a_world_reports_what_it_is_writing)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Progress");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 99LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 1;
    config.budget = {0, 4 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    CHECK(streamer.open(dir.c_str(), 1, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;
    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);

    struct Report {
        int calls = 0;
        u32 owed = 0;
        u32 lastWritten = 0;
        bool monotonic = true;
        bool withinOwed = true;
    };
    Report report;

    streamer.close(kNow, &report, [](void* context, u32 written, u32 owed) {
        Report& seen = *static_cast<Report*>(context);
        if (seen.calls > 0) {
            seen.monotonic = seen.monotonic && written >= seen.lastWritten && owed == seen.owed;
        }
        seen.withinOwed = seen.withinOwed && written <= owed;
        seen.owed = owed;
        seen.lastWritten = written;
        ++seen.calls;
    });

    // Called at least once even with nothing owed -- that is the "already
    // saved" answer rather than the absence of one.
    CHECK(report.calls >= 1);
    CHECK(report.monotonic);
    CHECK(report.withinOwed);
    // Whatever it started with, it finished.
    CHECK_EQ(int(report.lastWritten), int(report.owed));

    // And the world really is on the card, which is what the count was about.
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == world::OpenResult::Ok);
        CHECK(storage.hasChunk(0, 0));
        CHECK(storage.close(kNow));
    }
}

TEST(a_second_session_reads_what_the_first_one_generated)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Twice");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 99LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRendererConfig config;
    config.meshDistance = 1;
    config.budget = {0, 2 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    world::ChunkColumn first;
    {
        ChunkRenderer renderer;
        renderer.reset(&allocator, config);
        WorldStreamer streamer;
        streamer.setGenerateMissing(true);
        CHECK(streamer.open(dir.c_str(), 1, kNow));
        CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);
        CHECK(streamer.stats().columnsResident > 0);
        streamer.close(kNow);
        renderer.shutdown();
    }
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == world::OpenResult::Ok);
        CHECK(storage.loadChunk(0, 0, &first));
        CHECK(storage.close(kNow));
    }

    {
        ChunkRenderer renderer;
        renderer.reset(&allocator, config);
        WorldStreamer streamer;
        streamer.setGenerateMissing(true);
        CHECK(streamer.open(dir.c_str(), 1, kNow));
        CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);

        // The columns were all on the card, so the generator was never reached.
        CHECK_EQ(streamer.stats().generatedThisFrame, 0);
        CHECK_EQ(streamer.stats().pendingGeneration, 0);
        CHECK(streamer.stats().columnsResident > 0);
        streamer.close(kNow);
        renderer.shutdown();
    }

    io::PosixFileSystem fs;
    mcver::Storage storage(fs);
    CHECK(storage.open(dir.c_str(), kNow) == world::OpenResult::Ok);
    world::ChunkColumn second;
    CHECK(storage.loadChunk(0, 0, &second));
    CHECK(storage.close(kNow));

    int differences = 0;
    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            for (int y = 0; y < 128; ++y) {
                if (first.block(lx, y, lz) != second.block(lx, y, lz)) {
                    ++differences;
                }
            }
        }
    }
    CHECK_EQ(differences, 0);
}

// With generation off -- which is the default, and what the measurement
// harnesses rely on -- a world with no chunks stays a world with no chunks.
TEST(generation_is_off_by_default_and_an_empty_world_stays_empty)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Untouched");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 5LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 1;
    config.budget = {0, 1024 * 1024};
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    CHECK(!streamer.generateMissing());
    CHECK(streamer.open(dir.c_str(), 1, kNow));

    WorldStreamer::Budget budget;
    for (int i = 0; i < 40; ++i) {
        frame(streamer, renderer, u32(i), 0, 0, budget);
    }

    CHECK(!streamer.stats().workerRunning);
    CHECK_EQ(streamer.stats().columnsResident, 0);
    CHECK_EQ(streamer.stats().generatedThisFrame, 0);
    CHECK_EQ(streamer.stats().columnsMissing, 25);
    streamer.close(kNow);

    io::PosixFileSystem fs;
    mcver::Storage storage(fs);
    CHECK(storage.open(dir.c_str(), kNow) == world::OpenResult::Ok);
    CHECK(!storage.hasChunk(0, 0));
    CHECK(storage.close(kNow));
    renderer.shutdown();
}


namespace {

// Every chunk file the world holds, keyed by coordinate, as raw block ids.
// Comparing two of these is comparing two worlds.
struct WorldSnapshot {
    std::map<std::pair<i32, i32>, std::vector<u8>> columns;
};

bool visitChunk(void* context, i32 chunkX, i32 chunkZ)
{
    auto* found = static_cast<std::vector<std::pair<i32, i32>>*>(context);
    found->push_back({chunkX, chunkZ});
    return true;
}

WorldSnapshot snapshot(const std::string& dir)
{
    WorldSnapshot out;
    io::PosixFileSystem fs;
    mcver::Storage storage(fs);
    if (storage.open(dir.c_str(), kNow) != world::OpenResult::Ok) {
        return out;
    }
    std::vector<std::pair<i32, i32>> found;
    storage.forEachChunk(&found, &visitChunk);
    for (const auto& at : found) {
        world::ChunkColumn column;
        if (!storage.loadChunk(at.first, at.second, &column)) {
            continue;
        }
        std::vector<u8> blocks;
        blocks.reserve(32768);
        for (int lx = 0; lx < 16; ++lx) {
            for (int lz = 0; lz < 16; ++lz) {
                for (int y = 0; y < 128; ++y) {
                    blocks.push_back(u8(column.block(lx, y, lz)));
                }
            }
        }
        out.columns.emplace(at, std::move(blocks));
    }
    storage.close(kNow);
    return out;
}

// Fills a world by walking the same fixed path, with generation either on the
// worker or on the calling thread.
// How the world under test is filled. The three differ only in *when* work
// happens -- never in what it produces, which is the whole point.
enum class FillMode {
    Inline,        // generation on the calling thread, cache unthreaded
    Worker,        // generation on its own thread, cache unthreaded
    WorkerCached,  // the console's configuration: both threaded, reading ahead
};

void fillWorld(const std::string& dir, i64 seed, FillMode mode)
{
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        if (storage.create(dir.c_str(), seed, kNow) != world::OpenResult::Ok) {
            return;
        }
        storage.close(kNow);
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 1;
    config.budget = {0, 2 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    streamer.setGenerationThreaded(mode != FillMode::Inline);

    // **The cache is the third axis, and it is the one that could quietly move
    // the world.** It answers `hasChunk` from an index rather than a stat, it
    // serves reads from a table instead of the card, and it defers writes to
    // another thread -- and classification is what feeds the generation queue,
    // which *is* the population order. If any of those answered differently,
    // this arm's tree would diverge from the other two.
    world::ChunkCache::Config cache;
    cache.threaded = mode == FillMode::WorkerCached;
    cache.cleanCapBytes = 1u << 20;  // small on purpose: eviction has to happen
    streamer.setCacheConfig(cache);
    streamer.setPrefetchRings(mode == FillMode::WorkerCached ? 2 : 0);

    if (!streamer.open(dir.c_str(), 1, kNow)) {
        return;
    }

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    // **The path is the input, and the generator is never behind on it.**
    //
    // This used to move on after a fixed number of frames whether or not the
    // generator had caught up, on the grounds that dependence on how long a
    // column took was "precisely what must not exist". That is no longer the
    // claim, and the reason is worth having here rather than only in the docs.
    //
    // The queue takes the column nearest the camera, not the oldest -- see
    // WorldStreamer::pumpGeneration. So once a backlog exists, *which* column
    // is nearest depends on how far behind the generator got, and a slower
    // machine makes a different world. That is not a regression against
    // a1.1.2: a1.1.2 keeps no queue at all, and would have the same property
    // the moment its generation stopped being synchronous. What it has instead
    // is synchrony -- it cannot fall behind, so the question never arises.
    //
    // What still holds, and is what this test now pins down: **while the
    // generator keeps up, the order is a function of the path alone**, so all
    // three arms produce the same world byte for byte. That is the regime the
    // game is in whenever the player is moving at a speed a person moves at,
    // and it is still enough to catch the ordering bugs this test was written
    // for -- a column handed out twice, a sweep that reaches ground the
    // synchronous path never would.
    const i32 path[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}};
    for (const auto& at : path) {
        runUntilSettled(streamer, renderer, at[0], at[1], budget, 40000);
    }
    // And once more where it started, so all three finish the same work rather
    // than one being cut off mid-column.
    runUntilSettled(streamer, renderer, 0, 0, budget, 40000);
    streamer.close(kNow);
    renderer.shutdown();
}

// Compares two filled worlds column for column, and says *where* they part
// rather than only that they did -- a coordinate is the difference between a
// failure you can chase and one you can only rerun.
void compareWorlds(const WorldSnapshot& a, const WorldSnapshot& b)
{
    int missing = 0;
    int differing = 0;
    int firstBadX = 0;
    int firstBadZ = 0;
    for (const auto& entry : a.columns) {
        auto other = b.columns.find(entry.first);
        if (other == b.columns.end()) {
            ++missing;
            continue;
        }
        if (other->second != entry.second) {
            if (differing == 0) {
                firstBadX = entry.first.first;
                firstBadZ = entry.first.second;
            }
            ++differing;
        }
    }
    CHECK_EQ(missing, 0);
    if (differing != 0) {
        CHECK_EQ(firstBadX, -99999);  // reports where the two worlds part
        CHECK_EQ(firstBadZ, -99999);
    }
    CHECK_EQ(differing, 0);
}

}  // namespace

// **The worker must not change the world**, and that is not a property that can
// be reasoned about -- population order *is* the world in a1.1.2, so a sweep
// that happens in a different order produces different blocks wherever two
// chunks' passes reach the same ground.
//
// So: the same seed and the same path, filled three ways and compared chunk
// file for chunk file -- generation on the calling thread, generation on a
// worker, and the console's own configuration with the chunk cache threaded and
// reading ahead. The threaded runs finish a column over some number of frames
// and the synchronous one inside a single frame, which is exactly the
// difference that must not matter **while the generator is keeping up**.
//
// That qualifier is new and it is not a weakening of the code, it is a
// correction of what was being claimed. The generation queue takes the column
// nearest the camera rather than the oldest, so once a player outruns the
// generator the choice depends on how far behind it got. a1.1.2 has no queue
// to be faithful to here -- it generates inline and therefore never falls
// behind -- so the unconditional form of this test was pinning down a property
// of our own asynchrony, and the price of it was a border of chunks that never
// filled. See docs/status.md 0g.
//
// **The third arm is what holds the chunk cache honest.** It answers `hasChunk`
// from a directory index instead of a stat, serves reads from a table instead
// of the card, and writes on another thread some time later -- and
// classification is what feeds the generation queue, so an existence answer
// that arrived a moment early or late would reorder the sweeps and produce a
// different world. Its cache cap is set small enough that eviction happens
// during the run, because an entry evicted and re-read is the case where a
// stale or missing answer would show up.
//
// The hazard this is really guarding is subtle and was real: a column can be
// queued as missing and then written by a neighbour's sweep before the worker
// reaches it. Sweeping for it anyway reaches three rings further out and
// populates ground the synchronous path never touches -- and how often that
// happens depends on how many frames a generation took. Making such a job a
// no-op is what this test holds in place.
TEST(a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');

    const std::string threadedDir = temp.world("Threaded");
    const std::string inlineDir = temp.world("Inline");
    const std::string cachedDir = temp.world("Cached");

    fillWorld(threadedDir, 20260822LL, FillMode::Worker);
    fillWorld(inlineDir, 20260822LL, FillMode::Inline);
    fillWorld(cachedDir, 20260822LL, FillMode::WorkerCached);

    const WorldSnapshot threaded = snapshot(threadedDir);
    const WorldSnapshot inl = snapshot(inlineDir);
    const WorldSnapshot cached = snapshot(cachedDir);

    // All three actually made a world, or the comparisons below are vacuous.
    CHECK(threaded.columns.size() >= 25);
    CHECK_EQ(int(threaded.columns.size()), int(inl.columns.size()));
    CHECK_EQ(int(threaded.columns.size()), int(cached.columns.size()));

    compareWorlds(threaded, inl);
    compareWorlds(threaded, cached);
}

// ---------------------------------------------------------------------------
// The memory budget
// ---------------------------------------------------------------------------
// Residency was purely geometric: a cell is held if it is inside the radius,
// and nothing asked what that cost. Measured through the generator a column is
// 14.0 KB over ordinary terrain and 21.7 KB at the Far Lands, so the same grid
// that fits in 33 MB at render distance 24 needs 51 MB out there -- past a heap
// that does not grow, into a failed `operator new`, into `abort()`. See
// crashlogs/007 and WorldStreamer::setMemoryBudget.
//
// These run the budget deliberately tight so the mechanism is exercised at a
// render distance a test can settle in, rather than needing the Far Lands.

TEST(a_memory_budget_shrinks_the_admission_radius_and_evicts)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Budget");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 1234567890LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 4;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    CHECK(streamer.open(dir.c_str(), 4, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    // Fill first, with no budget, and record what the full grid costs.
    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);
    const usize full = streamer.stats().blockBytes;
    const int fullColumns = streamer.stats().columnsResident;
    CHECK(full > 0);
    CHECK_EQ(streamer.stats().admitRadius, streamer.loadRadius());
    CHECK_EQ(int(streamer.stats().evictedForMemory), 0);

    // Now ask for half of it. The radius has to come in, and columns have to go.
    streamer.setMemoryBudget(full / 2);
    for (u32 n = 0; n < 4000; ++n) {
        frame(streamer, renderer, n + 100000, 0, 0, budget);
        if (streamer.stats().blockBytes <= full / 2
            && streamer.stats().admitRadius < streamer.loadRadius()) {
            break;
        }
    }

    const WorldStreamer::Stats& tight = streamer.stats();
    CHECK(tight.admitRadius < streamer.loadRadius());
    CHECK(tight.admitRadius >= 3);          // never below the floor
    CHECK(tight.evictedForMemory > 0);
    CHECK(tight.columnsResident < fullColumns);

    // **Under the budget, not merely smaller.** The point is the bound.
    CHECK(tight.blockBytes <= full / 2);

    // And the grid it kept is the one around the player, so the count matches
    // the radius rather than being whatever eviction happened to leave.
    const int edge = tight.admitRadius * 2 + 1;
    CHECK(tight.columnsResident <= edge * edge);

    streamer.close(kNow);
}

TEST(lifting_the_budget_lets_the_radius_grow_back)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Regrow");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 1234567890LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 4;
    config.budget = {0, 8 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    CHECK(streamer.open(dir.c_str(), 4, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);
    const usize full = streamer.stats().blockBytes;
    const int fullColumns = streamer.stats().columnsResident;

    streamer.setMemoryBudget(full / 2);
    for (u32 n = 0; n < 4000 && streamer.stats().admitRadius >= streamer.loadRadius(); ++n) {
        frame(streamer, renderer, n + 100000, 0, 0, budget);
    }
    CHECK(streamer.stats().admitRadius < streamer.loadRadius());

    // A budget nothing can exceed. The radius must come back, and it must come
    // back *gradually* -- a ring per strided pass, not in one jump.
    streamer.setMemoryBudget(0);
    CHECK_EQ(streamer.stats().admitRadius, streamer.loadRadius());

    // With the budget off entirely admitRadius() is loadRadius_ immediately;
    // the interesting case is a budget that is merely generous, which must also
    // come back to the full radius rather than sitting narrowed.
    //
    // **Frames, not runUntilSettled.** The radius grows one ring per strided
    // residency pass, and between passes there is nothing pending -- so a
    // settle-based loop stops at the first narrow radius that is fully loaded
    // and never sees the regrowth at all. That was this test's first failure
    // and it was the test that was wrong, not the code.
    streamer.setMemoryBudget(full * 4);
    int grew = 0;
    for (; grew < 8000; ++grew) {
        frame(streamer, renderer, u32(grew) + 300000, 0, 0, budget);
        if (streamer.stats().admitRadius >= streamer.loadRadius()) {
            break;
        }
    }
    CHECK(grew < 8000);
    CHECK_EQ(streamer.stats().admitRadius, streamer.loadRadius());

    // And the world it dropped comes back, which is what "grow back" has to
    // mean for a player: the far ring is readable again, not merely permitted.
    // `columnsResident` is the check, because it is recounted every frame --
    // `blockBytes` is the strided half of countResidency() and settling can
    // land on a frame that did not refresh it.
    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);
    CHECK_EQ(streamer.stats().columnsResident, fullColumns);

    // Then far enough past a strided pass that the byte total is this grid's
    // and not the narrowed one's.
    for (u32 n = 0; n < 64; ++n) {
        frame(streamer, renderer, n + 400000, 0, 0, budget);
    }
    CHECK(streamer.stats().blockBytes >= full);

    streamer.close(kNow);
}

TEST(no_budget_changes_nothing)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("NoBudget");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 1234567890LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 2;
    config.budget = {0, 4 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer streamer;
    streamer.setGenerateMissing(true);
    CHECK(streamer.open(dir.c_str(), 2, kNow));

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);

    // The same 49 the older test asserts, and nothing evicted. This is the
    // guard on every documented --fly number: the budget ships off.
    CHECK_EQ(streamer.stats().columnsResident, 49);
    CHECK_EQ(int(streamer.stats().evictedForMemory), 0);
    CHECK_EQ(streamer.stats().admitRadius, streamer.loadRadius());

    streamer.close(kNow);
}
