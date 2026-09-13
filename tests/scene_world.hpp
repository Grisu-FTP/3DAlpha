#pragma once

// A patch of empty chunk columns with blocks put where a test wants them.
//
// Shared by the tests that need a world rather than a fixture: the player body
// walks through one, the ray trace looks across one. It is the same shape as
// the fixture inside tests/tick_test.cpp -- plain `ChunkColumn`s reached
// through `TickAccess`, no streamer, no renderer, no card -- but centred on an
// arbitrary chunk, because half the point of both suites is running the same
// scene again in deep negative coordinates.
//
// It starts as air. The generators that produce the fixtures clear their own
// play volume to air before building anything, which is what makes the two
// worlds the same world.

#include "core/block/block_def.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/chunk.hpp"

#include <memory>
#include <vector>

namespace mc::test {

class SceneWorld {
public:
    static constexpr i32 kRadius = 3;

    SceneWorld(i32 centreChunkX, i32 centreChunkZ)
        : centreX_(centreChunkX), centreZ_(centreChunkZ)
    {
        for (i32 cz = -kRadius; cz <= kRadius; ++cz) {
            for (i32 cx = -kRadius; cx <= kRadius; ++cx) {
                columns_.push_back(
                    std::make_unique<world::ChunkColumn>(centreX_ + cx, centreZ_ + cz));
            }
        }
        tick::TickAccess access;
        access.ctx = this;
        access.column = &SceneWorld::columnAt;
        access.changed = &SceneWorld::onChanged;
        world_ = std::make_unique<tick::TickWorld>(access, 12345LL);
    }

    tick::TickWorld& w() { return *world_; }

    // Raw, deliberately: the fixtures record the world's blocks *after* the
    // game finished arguing about them, so replaying them through the placement
    // rules would rewrite metadata that is already final.
    void place(i32 x, int y, i32 z, block::BlockId id, u8 metadata)
    {
        world_->setBlockAndDataRaw(x, y, z, id, metadata);
    }

    // **Sky light, which nothing here computes.** A scene starts pitch dark
    // because no lighting pass has run over it, and that is invisible until
    // something reads the light: a mushroom that will not stay, an animal that
    // will not spawn. Tests that care put the light where they want it.
    void setSkyLight(i32 x, int y, i32 z, u8 level)
    {
        world::ChunkColumn* column = columnAt(this, x >> 4, z >> 4);
        if (column != nullptr && y >= 0 && y < world::ChunkColumn::kHeight) {
            column->setSkyLight(int(x & 15), y, int(z & 15), level);
        }
    }

    // Daylight over a rectangle: every cell from `y` up is lit.
    void lightColumnsFrom(i32 x0, i32 x1, i32 z0, i32 z1, int y)
    {
        for (i32 x = x0; x <= x1; ++x) {
            for (i32 z = z0; z <= z1; ++z) {
                for (int cy = y; cy < world::ChunkColumn::kHeight; ++cy) {
                    setSkyLight(x, cy, z, 15);
                }
            }
        }
    }

private:
    static world::ChunkColumn* columnAt(void* ctx, i32 cx, i32 cz)
    {
        auto* self = static_cast<SceneWorld*>(ctx);
        const i32 dx = cx - self->centreX_;
        const i32 dz = cz - self->centreZ_;
        if (dx < -kRadius || dx > kRadius || dz < -kRadius || dz > kRadius) {
            return nullptr;
        }
        const usize i = usize((dz + kRadius) * (kRadius * 2 + 1) + (dx + kRadius));
        return self->columns_[i].get();
    }

    static void onChanged(void*, i32, int, i32) {}

    i32 centreX_;
    i32 centreZ_;
    std::vector<std::unique_ptr<world::ChunkColumn>> columns_;
    std::unique_ptr<tick::TickWorld> world_;
};

}  // namespace mc::test
