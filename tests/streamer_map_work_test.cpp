#include "framework.hpp"

#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/frustum.hpp"
#include "core/world/chunk.hpp"
#include "version_slots.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <utility>

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
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_mapwork_XXXXXX");
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

void frame(WorldStreamer& streamer, ChunkRenderer& renderer, u32 n, i32 cx, i32 cz,
           const WorldStreamer::Budget& budget)
{
    renderer.beginFrame(n, openFrustum(), cx, 4, cz);
    streamer.update(renderer, cx, cz, budget);
}

int settle(WorldStreamer& streamer, ChunkRenderer& renderer, i32 cx, i32 cz,
           const WorldStreamer::Budget& budget, u32* counter, int maxFrames)
{
    int n = 0;
    for (; n < maxFrames; ++n) {
        frame(streamer, renderer, (*counter)++, cx, cz, budget);
        if (streamer.stats().pendingColumns == 0 && streamer.stats().pendingGeneration == 0
            && streamer.stats().pendingReads == 0 && streamer.generationIdle()
            && streamer.storageIdle() && renderer.meshQueue().empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return n;
}

// Everything a test world needs, so the four cases below read as the thing they
// are about rather than as setup.
struct World {
    TestAllocator allocator;
    ChunkRenderer renderer;
    TempDir temp;
    WorldStreamer streamer;
    WorldStreamer::Budget budget;
    u32 counter = 1;

    bool open()
    {
        ChunkRendererConfig config;
        config.meshDistance = 2;
        config.budget = {0, 8 * 1024 * 1024};
        config.meshBudgetPerFrame = 8;
        renderer.reset(&allocator, config);

        if (temp.path[0] == '\0') {
            return false;
        }
        const std::string dir = temp.world("World");
        {
            io::PosixFileSystem fs;
            mcver::Storage storage(fs);
            if (storage.create(dir.c_str(), 1337LL, kNow) != world::OpenResult::Ok) {
                return false;
            }
            if (!storage.close(kNow)) {
                return false;
            }
        }
        streamer.setGenerateMissing(true);
        streamer.setAutosaveSeconds(0);
        if (!streamer.open(dir.c_str(), 2, kNow)) {
            return false;
        }
        budget.columnsPerFrame = 2;
        budget.generatedPerFrame = 2;
        budget.meshesPerFrame = 8;
        return true;
    }

    void step(i32 cx, i32 cz) { frame(streamer, renderer, counter++, cx, cz, budget); }
    int settleAt(i32 cx, i32 cz)
    {
        return settle(streamer, renderer, cx, cz, budget, &counter, 6000);
    }
};

// What the offered work saw, recorded on the worker thread and read on the main
// thread once the offer has been reclaimed -- which is the only moment either
// of those threads is allowed to look at it.
struct Seen {
    int calls = 0;
    int indices[WorldStreamer::kColumnWorkMax] = {};
    i32 x[WorldStreamer::kColumnWorkMax] = {};
    i32 z[WorldStreamer::kColumnWorkMax] = {};

    static void visit(void* ctx, int index, const world::ChunkColumn& column)
    {
        auto* self = static_cast<Seen*>(ctx);
        if (self->calls < WorldStreamer::kColumnWorkMax) {
            self->indices[self->calls] = index;
            self->x[self->calls] = column.x;
            self->z[self->calls] = column.z;
        }
        ++self->calls;
    }
};

}  // namespace

TEST(every_block_a_tick_writes_puts_its_column_on_the_change_list_exactly_once)
{
    World w;
    CHECK(w.open());
    if (!w.streamer.isOpen()) {
        return;
    }

    // Negative on both axes, per CONTRIBUTING.
    constexpr i32 kChunkX = -1;
    constexpr i32 kChunkZ = -2;
    w.settleAt(kChunkX, kChunkZ);

    // Adoption is itself a change -- it is ground the map has never drawn -- so
    // the list is not empty here, and draining it is what a reader does every
    // frame.
    CHECK(w.streamer.changedColumns() > 0);
    std::set<std::pair<i32, i32>> adopted;
    i32 cx = 0;
    i32 cz = 0;
    while (w.streamer.takeChangedColumn(&cx, &cz)) {
        CHECK(adopted.insert({cx, cz}).second);  // never twice
    }
    CHECK(adopted.count({kChunkX, kChunkZ}) == 1);
    CHECK(w.streamer.changedColumns() == 0);
    CHECK(!w.streamer.changedColumnsOverflowed());

    tick::TickWorld* world = w.streamer.worldTick();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }

    // **Two hundred blocks in one chunk**, which is the shape of a fluid
    // spreading or a fire burning out, and the whole reason the list dedupes:
    // the reader must be handed one coordinate, not two hundred.
    const block::BlockId fill = block::BlockId(mcver::Block::Cobblestone);
    for (int i = 0; i < 200; ++i) {
        const i32 x = kChunkX * 16 + (i % 16);
        const i32 z = kChunkZ * 16 + ((i / 16) % 16);
        CHECK(world->setBlockWithNotify(x, 40 + (i % 8), z, fill));
    }
    CHECK(w.streamer.changedColumns() == 1);
    CHECK(w.streamer.takeChangedColumn(&cx, &cz));
    CHECK(cx == kChunkX);
    CHECK(cz == kChunkZ);
    CHECK(!w.streamer.takeChangedColumn(&cx, &cz));

    // ...and once it has been taken off, the next write puts it back on. The
    // list is work outstanding, not a record of what has ever happened.
    CHECK(world->setBlockWithNotify(kChunkX * 16 + 3, 41, kChunkZ * 16 + 4, fill));
    CHECK(w.streamer.changedColumns() == 1);

    // A second chunk is a second entry, and the player's own edit goes on the
    // same list a tick's write does.
    CHECK(w.streamer.setBlock(w.renderer, (kChunkX + 1) * 16 + 2, 41, kChunkZ * 16 + 2, fill, 0));
    CHECK(w.streamer.changedColumns() == 2);

    w.streamer.close(kNow);
}

TEST(offered_column_work_runs_on_the_generation_worker_when_it_has_nothing_to_generate)
{
    World w;
    CHECK(w.open());
    if (!w.streamer.isOpen()) {
        return;
    }
    constexpr i32 kChunkX = -1;
    constexpr i32 kChunkZ = -2;
    w.settleAt(kChunkX, kChunkZ);

    // Without a worker there is nothing to offload to and this test has no
    // subject; the streamer says so rather than the test guessing.
    CHECK(w.streamer.stats().workerRunning);
    CHECK(w.streamer.columnWorkAvailable());
    if (!w.streamer.columnWorkAvailable()) {
        w.streamer.close(kNow);
        return;
    }

    i32 x[4] = {kChunkX, kChunkX + 1, kChunkX, kChunkX + 1};
    i32 z[4] = {kChunkZ, kChunkZ, kChunkZ + 1, kChunkZ + 1};
    Seen seen;
    CHECK(w.streamer.offerColumnWork(x, z, 4, &Seen::visit, &seen) == 4);

    // An offer is outstanding, so nothing else may be offered over it.
    CHECK(!w.streamer.columnWorkAvailable());
    i32 other[1] = {kChunkX};
    i32 otherZ[1] = {kChunkZ};
    CHECK(w.streamer.offerColumnWork(other, otherZ, 1, &Seen::visit, &seen) == 0);

    // **Collected only after the streamer has withdrawn it**, which is what
    // update() does at its top -- and what makes the borrowed columns safe.
    // Before that there is no answer to give.
    int done = -1;
    CHECK(!w.streamer.takeColumnWork(&done));
    CHECK(done == -1);

    // The world is settled, so the worker is idle and takes it at once; the
    // sleep is slack, not synchronisation.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    w.step(kChunkX, kChunkZ);

    CHECK(w.streamer.takeColumnWork(&done));
    CHECK(done == 4);
    CHECK(seen.calls == 4);
    for (int i = 0; i < 4 && i < seen.calls; ++i) {
        // The index is the position in the offered array, and the column it was
        // handed is the one that coordinate names.
        CHECK(seen.indices[i] == i);
        CHECK(seen.x[i] == x[i]);
        CHECK(seen.z[i] == z[i]);
    }

    // Taken means gone: the slot is free for the next frame's batch.
    CHECK(w.streamer.columnWorkAvailable());
    CHECK(!w.streamer.takeColumnWork(&done));

    w.streamer.close(kNow);
}

TEST(a_coordinate_whose_column_is_not_resident_is_dropped_by_the_offer)
{
    World w;
    CHECK(w.open());
    if (!w.streamer.isOpen()) {
        return;
    }

    constexpr i32 kChunkX = -1;
    constexpr i32 kChunkZ = -2;
    w.settleAt(kChunkX, kChunkZ);
    if (!w.streamer.columnWorkAvailable()) {
        w.streamer.close(kNow);
        return;
    }

    // Resolved on the main thread, where the grid is settled, so the worker
    // never looks a coordinate up and can never be handed a column that has
    // gone. The survivors are compacted to the front.
    i32 x[4] = {kChunkX + 400, kChunkX, kChunkX + 900, kChunkX + 1};
    i32 z[4] = {kChunkZ + 400, kChunkZ, kChunkZ + 900, kChunkZ};
    Seen seen;
    CHECK(w.streamer.offerColumnWork(x, z, 4, &Seen::visit, &seen) == 2);
    CHECK(x[0] == kChunkX);
    CHECK(z[0] == kChunkZ);
    CHECK(x[1] == kChunkX + 1);
    CHECK(z[1] == kChunkZ);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    w.step(kChunkX, kChunkZ);

    int done = 0;
    CHECK(w.streamer.takeColumnWork(&done));
    CHECK(done == 2);
    CHECK(seen.calls == 2);
    CHECK(seen.x[0] == kChunkX);
    CHECK(seen.x[1] == kChunkX + 1);

    // Nothing resident at all is refused outright, and refusing costs the
    // caller nothing to recover from -- there was no column to read.
    i32 gone[2] = {kChunkX + 400, kChunkX + 401};
    i32 goneZ[2] = {kChunkZ + 400, kChunkZ + 401};
    CHECK(w.streamer.offerColumnWork(gone, goneZ, 2, &Seen::visit, &seen) == 0);
    CHECK(w.streamer.columnWorkAvailable());

    w.streamer.close(kNow);
}

TEST(an_offer_made_while_the_world_is_still_being_generated_never_holds_generation_up)
{
    World w;
    CHECK(w.open());
    if (!w.streamer.isOpen()) {
        return;
    }

    constexpr i32 kChunkX = -1;
    constexpr i32 kChunkZ = -2;
    // Enough frames for a column to exist to offer, and nowhere near enough for
    // the world to be finished: the worker has a slate to work through for the
    // whole of the rest of this test.
    for (int i = 0; i < 12; ++i) {
        w.step(kChunkX, kChunkZ);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(w.streamer.stats().pendingGeneration > 0);
    if (!w.streamer.stats().workerRunning) {
        w.streamer.close(kNow);
        return;
    }

    // Offer on every frame while the frontier fills in. Whatever the worker
    // does with them, the accounting has to hold: every offer comes back, and
    // it comes back having run over no more than it was given.
    int offers = 0;
    int ran = 0;
    for (int i = 0; i < 400 && w.streamer.stats().pendingGeneration > 0; ++i) {
        i32 x[2] = {kChunkX, kChunkX};
        i32 z[2] = {kChunkZ, kChunkZ + 1};
        Seen seen;
        const int taken = w.streamer.offerColumnWork(x, z, 2, &Seen::visit, &seen);
        w.step(kChunkX, kChunkZ);
        if (taken > 0) {
            ++offers;
            int done = -1;
            CHECK(w.streamer.takeColumnWork(&done));
            CHECK(done >= 0);
            CHECK(done <= taken);
            CHECK(seen.calls == done);
            ran += done;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(offers > 0);

    // **And the world still finishes**, which is the claim that matters: the
    // offers were work done in the gaps, not a queue generation had to get
    // past. `settleAt` gives up rather than looping for ever, so a settled
    // world is the assertion and the frame count is only the guard on it.
    const int frames = w.settleAt(kChunkX, kChunkZ);
    CHECK(frames < 6000);
    CHECK(w.streamer.stats().pendingGeneration == 0);
    CHECK(w.streamer.generationIdle());
    CHECK(ran >= 0);

    w.streamer.close(kNow);
}
