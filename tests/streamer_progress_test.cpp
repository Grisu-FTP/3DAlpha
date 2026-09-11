#include "framework.hpp"

#include "core/gui/progress.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/frustum.hpp"
#include "version_slots.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace mc;
using gui::ChunkState;
using render::ChunkRenderer;
using render::ChunkRendererConfig;
using render::VboAllocator;
using render::VboTier;
using render::WorldStreamer;

namespace {

constexpr i64 kNow = 1284768000000LL;

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_prog_XXXXXX");
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

ChunkState cellAt(const std::vector<ChunkState>& grid, int radius, int dx, int dz)
{
    const int edge = radius * 2 + 1;
    return grid[usize((dz + radius) * edge + (dx + radius))];
}

}  // namespace

// **What the generation screen draws, checked against a streamer rather than
// against a mock.** The square is the only thing in the game that reports the
// grid's internal state to a player, so the mapping from `CellState` to a
// colour is the part that can be wrong without anything else noticing: a square
// that filled a ring early, or one that never filled at all, would be a screen
// that lies about when the world is ready.
TEST(the_progress_square_fills_out_to_the_render_distance_and_no_further)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Progress");

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

    // One ring wider than the render distance, which is what the generation
    // wait relies on: requiring the ring at the render distance to be drawable
    // is what makes the ring beyond it get generated.
    CHECK_EQ(streamer.loadRadius(), 3);

    // **Before a single frame, nothing has been asked about.** The square is
    // black, which is the state that says "not even started" rather than "there
    // is nothing here".
    std::vector<ChunkState> grid(usize(5 * 5));
    streamer.progressGrid(0, 0, 2, grid.data());
    for (const ChunkState state : grid) {
        CHECK(state == ChunkState::Unstarted);
    }
    CHECK_EQ(streamer.progressWithin(0, 0, 2).done, 0);
    CHECK_EQ(streamer.progressWithin(0, 0, 2).total, 25);
    CHECK(!streamer.progressWithin(0, 0, 2).finished());

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    int frames = 0;
    for (; frames < 20000; ++frames) {
        renderer.beginFrame(u32(frames), openFrustum(), 0, 4, 0);
        streamer.update(renderer, 0, 0, budget);
        if (streamer.progressWithin(0, 0, 2).finished()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(frames < 20000);

    // Every column inside the render distance is drawable, which is the
    // condition the console's generation screen waits on.
    const WorldStreamer::ProgressCount inside = streamer.progressWithin(0, 0, 2);
    CHECK_EQ(inside.done, 25);
    CHECK_EQ(inside.total, 25);
    CHECK(inside.finished());

    streamer.progressGrid(0, 0, 2, grid.data());
    for (const ChunkState state : grid) {
        CHECK(state == ChunkState::Done);
    }

    // **The ring beyond it is loaded and not drawable, and that is the reason
    // the wait counts at the render distance rather than at the load radius.**
    // Those columns exist -- they had to, for the ring inside them to be meshed
    // -- but the renderer's field is only as wide as the render distance, so
    // they can never be published and a square drawn out to them would have a
    // border that never turned green.
    std::vector<ChunkState> wider(usize(7 * 7));
    streamer.progressGrid(0, 0, 3, wider.data());
    CHECK(cellAt(wider, 3, 0, -3) == ChunkState::Ready);
    CHECK(cellAt(wider, 3, 3, 3) == ChunkState::Ready);
    CHECK(cellAt(wider, 3, 0, 0) == ChunkState::Done);

    const WorldStreamer::ProgressCount ring = streamer.progressWithin(0, 0, 3);
    CHECK_EQ(ring.done, 25);
    CHECK_EQ(ring.total, 49);
    CHECK(!ring.finished());

    // **Row-major from the north-west corner**, which is the one thing about
    // this call a drawing bug and an indexing bug look identical from. The
    // square is asked for around a centre the streamer has never been to: the
    // half of it that overlaps the world made above is finished, and the half
    // past the edge of the grid has never been asked about. If the rows and
    // columns were transposed or mirrored, the two halves would swap.
    std::vector<ChunkState> offset(usize(9 * 9));
    streamer.progressGrid(4, 0, 4, offset.data());
    // Due west of the new centre, which is the origin of the world just made.
    CHECK(cellAt(offset, 4, -4, 0) == ChunkState::Done);
    // Due east of it, four chunks past anything the grid holds.
    CHECK(cellAt(offset, 4, 4, 0) == ChunkState::Unstarted);
    // ...and the same asymmetry does not appear along the other axis, where the
    // centre has not moved.
    CHECK(cellAt(offset, 4, -4, -4) == cellAt(offset, 4, -4, 4));

    streamer.close(kNow);
}

// The world is being made *now*, and the square has to say so while it is
// happening rather than only once it has. Inline generation, so the count is
// the calling thread's and there is no worker to race with.
TEST(a_square_part_way_through_holds_every_state_on_the_ramp)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Partial");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 42LL, kNow) == world::OpenResult::Ok);
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
    streamer.setGenerationThreaded(false);
    CHECK(streamer.open(dir.c_str(), 2, kNow));
    CHECK(!streamer.stats().workerRunning);

    WorldStreamer::Budget budget;
    budget.columnsPerFrame = 1;
    budget.generatedPerFrame = 1;
    budget.meshesPerFrame = 8;

    // Far enough in that columns have been made and short of the whole square,
    // which is where every colour on the ramp is on screen at once.
    bool sawOwed = false;
    bool sawReady = false;
    bool sawDone = false;
    std::vector<ChunkState> grid(usize(7 * 7));

    for (int n = 0; n < 200; ++n) {
        renderer.beginFrame(u32(n), openFrustum(), 0, 4, 0);
        streamer.update(renderer, 0, 0, budget);

        streamer.progressGrid(0, 0, 3, grid.data());
        for (const ChunkState state : grid) {
            sawOwed = sawOwed || state == ChunkState::Owed;
            sawReady = sawReady || state == ChunkState::Ready;
            sawDone = sawDone || state == ChunkState::Done;
        }
        if (sawOwed && sawReady && sawDone) {
            break;
        }
    }

    CHECK(sawOwed);
    CHECK(sawReady);
    CHECK(sawDone);

    // The count only ever moves forward while the centre stays put, which is
    // what the console's stall detector reads. A count that went backwards
    // would make a healthy generator look stalled.
    const int settled = streamer.progressWithin(0, 0, 3).done;
    renderer.beginFrame(1000u, openFrustum(), 0, 4, 0);
    streamer.update(renderer, 0, 0, budget);
    CHECK(streamer.progressWithin(0, 0, 3).done >= settled);

    streamer.close(kNow);
}

// Generation off is the harnesses' configuration, and it is also every world
// that is already complete: there is nothing owed anywhere, so nothing may sit
// on the square as though there were.
TEST(a_world_that_generates_nothing_never_shows_a_column_as_owed)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Fixed");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 7LL, kNow) == world::OpenResult::Ok);
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
    CHECK(streamer.open(dir.c_str(), 2, kNow));

    WorldStreamer::Budget budget;
    std::vector<ChunkState> grid(usize(5 * 5));

    for (int n = 0; n < 200; ++n) {
        renderer.beginFrame(u32(n), openFrustum(), 0, 4, 0);
        streamer.update(renderer, 0, 0, budget);
        if (streamer.progressWithin(0, 0, 2).finished()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Every cell is the edge of a finite world, which is `Done` -- nothing is
    // owed there and nothing is coming. A sixth colour for it would be a part
    // of the square that a player would watch, waiting.
    streamer.progressGrid(0, 0, 2, grid.data());
    for (const ChunkState state : grid) {
        CHECK(state == ChunkState::Done);
    }
    CHECK(streamer.progressWithin(0, 0, 2).finished());

    streamer.close(kNow);
}

// **The save screen draws the world, so close() may not have taken it away
// yet.** `close(nowMillis, ctx, progress)` calls back once per pumped write,
// and on the 3DS that callback draws a whole frame of the world behind the
// progress bar -- including the minecart pass, which is the one entity draw
// that borrows the `TickWorld` (a cart leans along the track, not along its own
// motion). `tick_` used to be destroyed before the drain started, so every one
// of those frames read a freed object; the console jumped into newlib's malloc
// bin array. See crashlogs/009-save-with-a-minecart/.
//
// The callback here does exactly what that frame does and no more: ask the
// streamer for its tick world and read a block through it.
TEST(the_save_progress_callback_can_still_read_the_world)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    const std::string dir = temp.world("Saving");

    {
        io::PosixFileSystem fs;
        mcver::Storage storage(fs);
        CHECK(storage.create(dir.c_str(), 4242LL, kNow) == world::OpenResult::Ok);
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
    for (int n = 0; n < 20000; ++n) {
        renderer.beginFrame(u32(n), openFrustum(), 0, 4, 0);
        streamer.update(renderer, 0, 0, budget);
        if (streamer.progressWithin(0, 0, 2).finished()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(streamer.progressWithin(0, 0, 2).finished());

    struct Watcher {
        WorldStreamer* streamer;
        int calls = 0;
        int worldLost = 0;
    } watcher{&streamer};

    streamer.close(kNow, &watcher, [](void* context, u32, u32) {
        Watcher& w = *static_cast<Watcher*>(context);
        ++w.calls;
        const tick::TickWorld* world = w.streamer->worldTick();
        if (world == nullptr) {
            ++w.worldLost;
            return;
        }
        // The read the minecart pass makes: what block is the cart sitting on.
        (void)world->blockAt(0, 64, 0);
    });

    // The loop reports before it tests, so it always runs at least once --
    // "this world owed nothing" is an answer the screen gives rather than a
    // case it skips.
    CHECK(watcher.calls > 0);
    CHECK_EQ(watcher.worldLost, 0);
}
