#include "framework.hpp"

#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/util/frustum.hpp"
#include "core/world/chunk.hpp"
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
void fillWorld(const std::string& dir, i64 seed, bool threaded)
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
    streamer.setGenerationThreaded(threaded);
    if (!streamer.open(dir.c_str(), 1, kNow)) {
        return;
    }

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    // **The path is the input.** Both runs walk it identically; only how long
    // each column takes to appear differs between them.
    //
    // The camera moves *while generation is outstanding*, which is the case
    // worth testing rather than the easy one: waiting for each waypoint to
    // settle before moving on would hide any dependence on how long a column
    // took, and that dependence is precisely what must not exist.
    const i32 path[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}};
    u32 n = 0;
    for (const auto& at : path) {
        for (int i = 0; i < 40; ++i) {
            frame(streamer, renderer, n++, at[0], at[1], budget);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Then stand still until the world catches up, so both runs finish the same
    // work rather than one being cut off mid-column.
    runUntilSettled(streamer, renderer, 0, 0, budget, 40000);
    streamer.close(kNow);
    renderer.shutdown();
}

}  // namespace

// **The worker must not change the world**, and that is not a property that can
// be reasoned about -- population order *is* the world in a1.1.2, so a sweep
// that happens in a different order produces different blocks wherever two
// chunks' passes reach the same ground.
//
// So: the same seed and the same path, filled once with generation on a worker
// thread and once on the calling thread, compared chunk file for chunk file.
// The threaded run finishes a column over some number of frames and the
// synchronous one inside a single frame, which is exactly the difference that
// must not matter.
//
// The hazard this is really guarding is subtle and was real: a column can be
// queued as missing and then written by a neighbour's sweep before the worker
// reaches it. Sweeping for it anyway reaches three rings further out and
// populates ground the synchronous path never touches -- and how often that
// happens depends on how many frames a generation took. Making such a job a
// no-op is what this test holds in place.
TEST(a_worker_thread_produces_the_same_world_as_generating_inline)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');

    const std::string threadedDir = temp.world("Threaded");
    const std::string inlineDir = temp.world("Inline");

    fillWorld(threadedDir, 20260822LL, true);
    fillWorld(inlineDir, 20260822LL, false);

    const WorldSnapshot threaded = snapshot(threadedDir);
    const WorldSnapshot inl = snapshot(inlineDir);

    // Both actually made a world, or the comparison below is vacuous.
    CHECK(threaded.columns.size() >= 25);
    CHECK_EQ(int(threaded.columns.size()), int(inl.columns.size()));

    int missing = 0;
    int differing = 0;
    int firstBadX = 0;
    int firstBadZ = 0;
    for (const auto& entry : threaded.columns) {
        auto other = inl.columns.find(entry.first);
        if (other == inl.columns.end()) {
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
