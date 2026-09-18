#include "framework.hpp"

#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/settings/world_settings.hpp"
#include "core/tick/tick_world.hpp"
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

// **The Extra Settings switches are read off the world, not handed in.**
//
// `WorldStreamer::open` loads `<world>/3dalpha.ini` and fills the two
// generator options from it, so a world carries its own answer wherever the
// card goes and the menu, the harness and a test cannot disagree about it.
// What this covers is that wiring: the same seed, the same chunks, one world
// with the bedrock fix written into its settings file and one without.
//
// Bedrock rather than ore because the floor is countable without a fixture --
// a1.1.2 leaves about a sixth of its columns open at y = 0, and the fix leaves
// none.
TEST(the_generator_reads_the_worlds_own_extra_settings)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');

    // CHECK returns from the enclosing function, so this counts into an
    // out-parameter rather than returning: a lambda that returned int could
    // not use the framework's macros.
    auto holesIn = [&](const char* name, bool fixBedrock, int* out) {
        *out = -1;
        const std::string dir = temp.world(name);
        io::PosixFileSystem fs;
        {
            mcver::Storage storage(fs);
            CHECK(storage.create(dir.c_str(), 4242LL, kNow) == world::OpenResult::Ok);
            CHECK(storage.close(kNow));
        }

        settings::WorldSettings worldSettings;
        worldSettings.fixBedrockHole = fixBedrock;
        CHECK(settings::saveWorldSettings(fs, dir, worldSettings));

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

        int holes = 0;
        for (i32 cx = -1; cx <= 1; ++cx) {
            for (i32 cz = -1; cz <= 1; ++cz) {
                const world::ChunkColumn* column = streamer.residentColumn(cx, cz);
                CHECK(column != nullptr);
                if (column == nullptr) {
                    continue;
                }
                for (int x = 0; x < 16; ++x) {
                    for (int z = 0; z < 16; ++z) {
                        if (column->block(x, 0, z) != block::BlockId(mcver::Block::Bedrock)) {
                            ++holes;
                        }
                    }
                }
            }
        }
        streamer.close(kNow);
        *out = holes;
    };

    // Nine chunks is 2,304 columns; a sixth of them is around 380.
    int vanilla = -1;
    holesIn("Vanilla", false, &vanilla);
    CHECK(vanilla > 0);

    // And with the switch written into the world's own settings file, the same
    // seed over the same chunks leaves none.
    int fixed = -1;
    holesIn("Fixed", true, &fixed);
    CHECK_EQ(fixed, 0);
}

// ---- served areas ----------------------------------------------------------
//
// **What the host can serve used to be what the host had loaded.** The grid is
// a square around this console's camera and wraps modulo its own width, so a
// guest who walked out of it was a player standing at the edge of nothing --
// `WorldServer::streamColumns` asked `ServerWorld::column` for their ground,
// got null, and marked them starved for the rest of the session.
//
// A served area is the fix: ground held outside the grid for somebody else, on
// the same cache and the same generator, edited through the same TickWorld and
// saved by the same autosave. What is checked here is all four of those, plus
// the one invariant that would quietly corrupt a world if it were wrong --
// that the grid and the served set never both hold the same column.
// Two guests at once, which is the case the flat cursor in `pumpServed` exists
// for: one budget and one pass over both squares, so neither guest can starve
// the other and a slow frame does not leave one of them permanently behind.
TEST(two_served_areas_are_both_filled_from_one_budget)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("TwoGuests");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 24680LL, kNow) == world::OpenResult::Ok);
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

    WorldStreamer::ServedArea areas[2];
    areas[0].chunkX = 60;
    areas[0].chunkZ = 60;
    areas[0].radius = 1;
    areas[1].chunkX = -60;
    areas[1].chunkZ = -60;
    areas[1].radius = 1;
    streamer.setServedAreas(areas, 2);

    int frames = 0;
    for (; frames < 20000; ++frames) {
        frame(streamer, renderer, u32(frames), 0, 0, budget);
        // Both, and nothing still owed -- `servedOwed` is published on a whole
        // pass, so it settles a frame or two after the last column lands.
        if (streamer.servedColumns() == 18 && streamer.servedOwed() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(frames < 20000);
    CHECK(streamer.servedColumn(60, 60) != nullptr);
    CHECK(streamer.servedColumn(-60, -60) != nullptr);
    CHECK_EQ(streamer.servedOwed(), 0);

    streamer.close(kNow);
}

// Closing while a served area is still being generated. The worker may be
// halfway through a sweep for a chunk no cell will ever hold, and `close()` has
// to join it and put the work away like any other -- so this is here as the
// one ordering the served path adds to a function that already had several.
// **The one thing about serving a second centre that can corrupt a world.**
//
// `ChunkGenerator::cacheColumnsFor` is fitted to a single player's frontier and
// `kRetireSlackChunks` leaves a flat 32 columns of headroom on it. A guest's
// square being swept has a frontier of its own -- a finished square leaves its
// whole perimeter live, those columns never getting their outward neighbours --
// and when the cache runs out `acquire` takes a *live* column, which comes back
// as bare terrain with its neighbours' trees missing and never becomes final
// again. `generatorEvictedLive` is the counter that says so; it must be zero.
//
// So this fills a served square at the host's own view distance and reads the
// counters out. It is the measurement `servedGeneratorSlack` is answerable to,
// and it runs on the host, where the generator is exactly the console's.
TEST(a_served_area_at_a_full_view_distance_does_not_overrun_the_generator)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Frontier");
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 777001LL, kNow) == world::OpenResult::Ok);
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
    // **Eight columns a frame, not one.** 225 chunks asked about at one a frame
    // is 225 frames of nothing but asking before a single sweep starts, and
    // what this measures is the generator's live set rather than the streamer's
    // pacing. The generator still takes one job at a time whatever this says.
    budget.columnsPerFrame = 8;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;
    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 40000) < 40000);

    // A guest at the host's own view distance, far enough out that not one
    // column of it is the grid's.
    WorldStreamer::ServedArea area;
    area.chunkX = 200;
    area.chunkZ = 200;
    area.radius = 7;
    streamer.setServedAreas(&area, 1);

    const int wanted = (area.radius * 2 + 1) * (area.radius * 2 + 1);
    int frames = 0;
    for (; frames < 200000; ++frames) {
        frame(streamer, renderer, u32(frames), 0, 0, budget);
        if (streamer.servedColumns() >= wanted && streamer.servedOwed() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(frames < 200000);
    CHECK_EQ(streamer.servedColumns(), wanted);

    // **The three counters that must all be zero**, and the peak the slack is
    // sized against. A non-zero `generatorEvictedLive` here is a corrupted
    // world, not a slow one.
    const WorldStreamer::Stats& stats = streamer.stats();
    std::printf("      served frontier: peakLive=%u evictedLive=%u failures=%u\n",
                stats.generatorPeakLive, stats.generatorEvictedLive,
                stats.generationFailures);
    CHECK_EQ(int(stats.generatorEvictedLive), 0);
    CHECK_EQ(int(stats.generationFailures), 0);
    CHECK_EQ(int(stats.generationIncomplete), 0);
    CHECK_EQ(int(stats.generationUnlightable), 0);

    // And the ground really is there, at the far corner as well as the middle.
    CHECK(streamer.servedColumn(200, 200) != nullptr);
    CHECK(streamer.servedColumn(207, 207) != nullptr);
    CHECK(streamer.servedColumn(193, 193) != nullptr);

    streamer.close(kNow);
}

TEST(closing_while_a_served_area_is_still_arriving_is_safe)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("ClosedEarly");
    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 5150LL, kNow) == world::OpenResult::Ok);
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

    WorldStreamer::ServedArea area;
    area.chunkX = 100;
    area.chunkZ = 100;
    area.radius = 3;
    streamer.setServedAreas(&area, 1);

    // A handful of frames and no more: the area is 49 columns of ground that
    // has never existed, so this is certain to be in the middle of it.
    for (int i = 0; i < 12; ++i) {
        frame(streamer, renderer, u32(i), 0, 0, budget);
    }
    CHECK(streamer.servedOwed() > 0 || streamer.servedColumns() > 0);
    streamer.close(kNow);
}

// **The three rings between the grid's reach and the grid's size.**
//
// `buildGrid` makes the cell array `loadRadius_ + 3` wide so a generation sweep
// has cells to classify around its edge, and nothing out there is ever read,
// generated or adopted. The served walk used to skip everything inside the
// *array*, so a guest standing in those rings was ground neither half of the
// world would make: `ServerWorld::column` answered null from `residentColumn`
// and from `servedColumn` both, and a session had a three-chunk band at the end
// of the host's render distance where nothing generated. Reported from play.
//
// The second half of the same bug is the save. The dirty marks chose between
// the grid and the served set by asking whether a *cell* existed, and in these
// three rings one does -- so a served column edited here was left clean and
// never written. That is what the reopen at the end is for.
TEST(a_guest_just_past_the_hosts_render_distance_is_served_and_not_starved)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Band");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 24680LL, kNow) == world::OpenResult::Ok);
        CHECK(storage.close(kNow));
    }

    TestAllocator allocator;
    ChunkRenderer renderer;
    ChunkRendererConfig config;
    config.meshDistance = 2;
    config.budget = {0, 4 * 1024 * 1024};
    config.meshBudgetPerFrame = 8;
    renderer.reset(&allocator, config);

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    // A mesh distance of 2 is a load radius of 3 and a cell array of 6, so
    // chunk 5 is inside the array and outside anything the grid will ever fill.
    constexpr i32 kBandX = 5;
    constexpr i32 kBandZ = 0;
    constexpr i32 kBlockX = kBandX * 16 + 7;
    constexpr i32 kBlockZ = kBandZ * 16 + 7;

    WorldStreamer::ServedArea area;
    area.chunkX = kBandX;
    area.chunkZ = kBandZ;
    area.radius = 0;

    {
        WorldStreamer streamer;
        streamer.setGenerateMissing(true);
        CHECK(streamer.open(dir.c_str(), 2, kNow));

        // The camera settles first, as it does in the test below: `refreshSlate`
        // serves nobody on a frame where the camera's own walk is blocked.
        CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);

        // The grid does not have it and never will. This is the band.
        CHECK(streamer.residentColumn(kBandX, kBandZ) == nullptr);
        CHECK(streamer.servedColumn(kBandX, kBandZ) == nullptr);

        streamer.setServedAreas(&area, 1);

        int frames = 0;
        for (; frames < 20000; ++frames) {
            frame(streamer, renderer, u32(frames), 0, 0, budget);
            if (streamer.servedColumn(kBandX, kBandZ) != nullptr) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(frames < 20000);

        const world::ChunkColumn* served = streamer.servedColumn(kBandX, kBandZ);
        CHECK(served != nullptr);
        if (served == nullptr) {
            streamer.close(kNow);
            return;
        }
        // Real ground rather than an empty column: swept and populated, with
        // a1.1.2's bedrock under the corner. (Only the corner: the floor's own
        // thickness is `y <= nextInt(5)`, so the rest of y = 0 is stone as
        // often as not.)
        CHECK(served->terrainPopulated);
        CHECK(served->block(0, 0, 0) == block::BlockId(mcver::Block::Bedrock));

        // A guest's dig, which reaches the world through `tickColumn`.
        tick::TickWorld* world = streamer.worldTick();
        CHECK(world != nullptr);
        if (world != nullptr) {
            CHECK(world->setBlockWithNotify(kBlockX, 40, kBlockZ,
                                            block::BlockId(mcver::Block::Glass)));
            CHECK(world->blockAt(kBlockX, 40, kBlockZ)
                  == block::BlockId(mcver::Block::Glass));
        }
        streamer.close(kNow);
    }

    // **And it was written.** Nothing is settled first: the column is on the
    // card now, so serving it is a read rather than a sweep.
    WorldStreamer reopened;
    reopened.setGenerateMissing(true);
    CHECK(reopened.open(dir.c_str(), 2, kNow));
    reopened.setServedAreas(&area, 1);

    int frames = 0;
    for (; frames < 20000; ++frames) {
        frame(reopened, renderer, u32(frames), 0, 0, budget);
        if (reopened.servedColumn(kBandX, kBandZ) != nullptr) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(frames < 20000);

    const world::ChunkColumn* back = reopened.servedColumn(kBandX, kBandZ);
    CHECK(back != nullptr);
    if (back != nullptr) {
        CHECK(back->block(7, 40, 7) == block::BlockId(mcver::Block::Glass));
    }
    reopened.close(kNow);
}

TEST(a_served_area_loads_ground_the_camera_cannot_reach)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Served");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 987654321LL, kNow) == world::OpenResult::Ok);
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

    // The camera settles at the origin first, so nothing below is competing
    // with the ground under the player holding the console -- which is the
    // order `refreshSlate` enforces and this test relies on.
    CHECK(runUntilSettled(streamer, renderer, 0, 0, budget, 20000) < 20000);

    // Forty chunks away: far outside a load radius of three, and ground the
    // world has never had.
    constexpr i32 kGuestX = 40;
    constexpr i32 kGuestZ = -25;
    CHECK(streamer.residentColumn(kGuestX, kGuestZ) == nullptr);
    CHECK(streamer.servedColumn(kGuestX, kGuestZ) == nullptr);

    WorldStreamer::ServedArea area;
    area.chunkX = kGuestX;
    area.chunkZ = kGuestZ;
    area.radius = 1;
    streamer.setServedAreas(&area, 1);

    // It is generated, not merely asked for. Nine columns, and the frames are
    // a deadlock guard rather than a measurement.
    int frames = 0;
    for (; frames < 20000; ++frames) {
        frame(streamer, renderer, u32(frames), 0, 0, budget);
        if (streamer.servedColumns() == 9 && streamer.servedOwed() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(frames < 20000);
    CHECK_EQ(streamer.servedColumns(), 9);

    const world::ChunkColumn* served = streamer.servedColumn(kGuestX, kGuestZ);
    CHECK(served != nullptr);
    if (served == nullptr) {
        streamer.close(kNow);
        return;
    }
    // Real ground rather than an empty column: a1.1.2 puts bedrock at y 0.
    CHECK(served->block(0, 0, 0) == block::BlockId(mcver::Block::Bedrock));

    // **And the tick can see it**, which is the line that makes a served column
    // part of the world rather than a buffer of bytes to post. A guest's dig
    // forty chunks from the host goes through exactly this.
    tick::TickWorld* world = streamer.worldTick();
    CHECK(world != nullptr);
    if (world != nullptr) {
        const i32 bx = kGuestX * 16 + 3;
        const i32 bz = kGuestZ * 16 + 3;
        CHECK(world->blockAt(bx, 0, bz) == block::BlockId(mcver::Block::Bedrock));
        CHECK(world->setBlockWithNotify(bx, 40, bz, block::BlockId(mcver::Block::Glass)));
        CHECK(world->blockAt(bx, 40, bz) == block::BlockId(mcver::Block::Glass));
    }

    // **The camera and the guest never both own a column.** Standing the camera
    // on top of the served area hands every one of them back; the grid is
    // authoritative for everything it can reach.
    CHECK(runUntilSettled(streamer, renderer, kGuestX, kGuestZ, budget, 20000) < 20000);
    CHECK_EQ(streamer.servedColumns(), 0);
    CHECK(streamer.residentColumn(kGuestX, kGuestZ) != nullptr);

    // ...and the edit made through the served copy survived the hand-over, so
    // nothing a guest did was read back over by the grid.
    const world::ChunkColumn* resident = streamer.residentColumn(kGuestX, kGuestZ);
    CHECK(resident != nullptr);
    if (resident != nullptr) {
        CHECK(resident->block(3, 40, 3) == block::BlockId(mcver::Block::Glass));
    }

    // An empty list lets the rest go.
    streamer.setServedAreas(nullptr, 0);
    frame(streamer, renderer, 1, kGuestX, kGuestZ, budget);
    CHECK_EQ(streamer.servedColumns(), 0);

    streamer.close(kNow);

    // **Saved, not just held.** Reopening is the only honest check: the grid
    // would answer either way.
    {
        WorldStreamer again;
        again.setGenerateMissing(false);
        CHECK(again.open(dir.c_str(), 2, kNow));
        ChunkRenderer second;
        second.reset(&allocator, config);
        CHECK(runUntilSettled(again, second, kGuestX, kGuestZ, budget, 20000) < 20000);
        const world::ChunkColumn* back = again.residentColumn(kGuestX, kGuestZ);
        CHECK(back != nullptr);
        if (back != nullptr) {
            CHECK(back->block(3, 40, 3) == block::BlockId(mcver::Block::Glass));
        }
        again.close(kNow);
    }
}
