// The streamer's multiplayer mode: columns that come from a server instead of a
// card, a world that does not tick, and edits that are only provisional.

#include "framework.hpp"

#include "core/render/chunk_renderer.hpp"
#include "core/render/world_streamer.hpp"
#include "core/util/frustum.hpp"
#include "core/world/chunk.hpp"

#include <cstdlib>
#include <memory>

using namespace mc;
using render::ChunkRenderer;
using render::ChunkRendererConfig;
using render::VboAllocator;
using render::VboTier;
using render::WorldStreamer;

namespace {

constexpr block::BlockId kStone = 1;
constexpr int kGround = 60;

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

std::unique_ptr<world::ChunkColumn> flatColumn(i32 cx, i32 cz)
{
    auto column = std::make_unique<world::ChunkColumn>(cx, cz);
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < kGround; ++y) {
                column->setBlock(x, y, z, kStone);
            }
            for (int y = kGround; y < world::ChunkColumn::kHeight; ++y) {
                column->setSkyLight(x, y, z, 15);
            }
            column->heightMap[(z << 4) | x] = u8(kGround);
        }
    }
    return column;
}

struct Fixture {
    TestAllocator allocator;
    ChunkRenderer renderer;
    WorldStreamer streamer;
    WorldStreamer::Budget budget;
    u32 counter = 0;

    explicit Fixture(int distance)
    {
        ChunkRendererConfig config;
        config.meshDistance = distance;
        config.budget = {0, 8 * 1024 * 1024};
        config.meshBudgetPerFrame = 64;
        renderer.reset(&allocator, config);
        budget.meshesPerFrame = 64;
    }

    void frame(i32 cx, i32 cz)
    {
        renderer.beginFrame(counter++, openFrustum(), cx, 4, cz);
        streamer.update(renderer, cx, cz, budget);
    }

    void settle(i32 cx, i32 cz)
    {
        for (int i = 0; i < 200; ++i) {
            frame(cx, cz);
            if (renderer.meshQueue().empty() && i > 2) {
                break;
            }
        }
    }
};

}  // namespace

TEST(a_multiplayer_world_draws_what_the_server_sent_and_nothing_else)
{
    constexpr int kDistance = 3;
    Fixture f(kDistance);
    CHECK(f.streamer.openRemote(kDistance));
    CHECK(f.streamer.remote());

    // Nothing sent yet: nothing to draw, and nothing is made up.
    f.settle(0, 0);
    CHECK(f.streamer.residentColumn(0, 0) == nullptr);
    CHECK(!f.renderer.hasColumn(0, 0));

    for (i32 cz = -4; cz <= 4; ++cz) {
        for (i32 cx = -4; cx <= 4; ++cx) {
            f.streamer.supplyColumn(flatColumn(cx, cz));
        }
    }
    f.settle(0, 0);
    CHECK(f.streamer.residentColumn(0, 0) != nullptr);
    CHECK(f.renderer.hasColumn(0, 0));
    CHECK(f.renderer.hasColumn(-2, 3));

    // A Block Change lands without being recorded as the client's own.
    CHECK(f.streamer.applyServerBlock(f.renderer, 5, kGround - 1, 5, 0, 0));
    CHECK_EQ(f.streamer.residentColumn(0, 0)->block(5, kGround - 1, 5), block::BlockId(0));
    CHECK_EQ(f.streamer.pendingEdits(), 0);
}

TEST(a_multiplayer_edit_is_put_back_after_eighty_ticks_unless_the_server_confirms_it)
{
    constexpr int kDistance = 3;
    Fixture f(kDistance);
    CHECK(f.streamer.openRemote(kDistance));
    for (i32 cz = -4; cz <= 4; ++cz) {
        for (i32 cx = -4; cx <= 4; ++cx) {
            f.streamer.supplyColumn(flatColumn(cx, cz));
        }
    }
    f.settle(0, 0);

    const int y = kGround - 1;
    CHECK(f.streamer.setBlock(f.renderer, 6, y, 6, 0, 0));
    CHECK(f.streamer.setBlock(f.renderer, 8, y, 8, 0, 0));
    CHECK_EQ(f.streamer.pendingEdits(), 2);

    // The server echoes one of them, as 0.2.1 answers every dig.
    CHECK(f.streamer.applyServerBlock(f.renderer, 8, y, 8, 0, 0));
    CHECK_EQ(f.streamer.pendingEdits(), 1);

    f.streamer.stepTicks(f.renderer, 79);
    CHECK_EQ(f.streamer.residentColumn(0, 0)->block(6, y, 6), block::BlockId(0));
    f.streamer.stepTicks(f.renderer, 1);
    CHECK_EQ(f.streamer.residentColumn(0, 0)->block(6, y, 6), kStone);
    CHECK_EQ(f.streamer.residentColumn(0, 0)->block(8, y, 8), block::BlockId(0));
    CHECK_EQ(f.streamer.pendingEdits(), 0);

    // Time is the only other thing that moves.
    const i64 before = f.streamer.level().time;
    f.streamer.stepTicks(f.renderer, 20);
    CHECK_EQ(f.streamer.level().time, before + 20);
    f.streamer.setRemoteTime(6000);
    CHECK_EQ(f.streamer.level().time, i64(6000));
}

TEST(a_multiplayer_column_left_behind_is_held_until_the_server_lets_it_go)
{
    constexpr int kDistance = 3;
    Fixture f(kDistance);
    CHECK(f.streamer.openRemote(kDistance));
    for (i32 cz = -4; cz <= 4; ++cz) {
        for (i32 cx = -4; cx <= 4; ++cx) {
            f.streamer.supplyColumn(flatColumn(cx, cz));
        }
    }
    f.settle(0, 0);
    CHECK(f.renderer.hasColumn(0, 0));

    // Walk far enough that the grid holds none of them. The server has sent
    // nothing new and will not resend these, so they must still exist.
    f.settle(40, 0);
    CHECK(f.streamer.residentColumn(0, 0) == nullptr);
    CHECK_EQ(f.streamer.remoteHeld(), usize(81));

    f.settle(0, 0);
    CHECK(f.streamer.residentColumn(0, 0) != nullptr);
    CHECK(f.renderer.hasColumn(0, 0));

    // A render distance change moves the grid; nothing in it is lost either.
    f.streamer.setMeshDistance(2, f.renderer);
    f.streamer.setMeshDistance(3, f.renderer);
    f.settle(0, 0);
    CHECK(f.streamer.residentColumn(-4, 4) != nullptr);

    // Pre-Chunk mode 0 is the only thing that frees one.
    f.streamer.unloadColumn(1, 1);
    f.frame(0, 0);
    CHECK(f.streamer.residentColumn(1, 1) == nullptr);
    f.settle(40, 0);
    CHECK_EQ(f.streamer.remoteHeld(), usize(80));

    f.streamer.close(0);
    CHECK(!f.streamer.remote());
    CHECK(!f.streamer.isOpen());
}
