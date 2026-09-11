// The tick system: the clock, the scheduled-update list, and the block
// behaviours the world runs on them.
//
// The world these run against is a small fixed grid of `ChunkColumn`s held in
// a vector, reached through `TickAccess` -- which is the whole point of that
// seam. No streamer, no renderer, no card, and every assertion is about a
// block that did or did not change.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/redstone.hpp"
#include "core/tick/tick_timer.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"

#include <memory>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::tick::TickScheduler;
using mc::tick::TickTimer;
using mc::tick::TickWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A 5x5 patch of columns centred on chunk 0,0, so a behaviour that reaches
// four blocks over a chunk boundary finds ground rather than the edge of the
// world. Coordinates are deliberately negative on one side: chunk -1 is where
// block -1 lives, and getting that wrong is a whole chunk of wrong world.
class TestWorld {
public:
    static constexpr i32 kRadius = 2;

    TestWorld()
    {
        for (i32 cz = -kRadius; cz <= kRadius; ++cz) {
            for (i32 cx = -kRadius; cx <= kRadius; ++cx) {
                columns_.push_back(std::make_unique<world::ChunkColumn>(cx, cz));
            }
        }
        // Full sky light everywhere above y=0, so a plant that wants light
        // gets it unless a test puts something over it.
        for (auto& c : columns_) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y < world::ChunkColumn::kHeight; ++y) {
                        c->setSkyLight(x, y, z, 15);
                    }
                    c->heightMap[usize(z * 16 + x)] = 0;
                }
            }
        }

        tick::TickAccess access;
        access.ctx = this;
        access.column = &TestWorld::columnAt;
        access.changed = &TestWorld::onChanged;
        world_ = std::make_unique<TickWorld>(access, 12345LL);
    }

    TickWorld& w() { return *world_; }
    int changes() const { return changes_; }

    void set(i32 x, int y, i32 z, mcver::Block b) { world_->setBlockRaw(x, y, z, bid(b)); }
    void set(i32 x, int y, i32 z, mcver::Block b, u8 metadata)
    {
        world_->setBlockAndDataRaw(x, y, z, bid(b), metadata);
    }
    BlockId get(i32 x, int y, i32 z) const { return world_->blockAt(x, y, z); }
    u8 dataAt(i32 x, int y, i32 z) const { return world_->dataAt(x, y, z); }

    void setSkyLight(i32 x, int y, i32 z, u8 v)
    {
        world::ChunkColumn* c = columnAt(this, x >> 4, z >> 4);
        c->setSkyLight(int(x & 15), y, int(z & 15), v);
    }

    // A flat floor of the given block at y, across every column held.
    void floorOf(mcver::Block b, int y)
    {
        for (auto& c : columns_) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) c->setBlock(x, y, z, bid(b));
            }
        }
        for (auto& c : columns_) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    int h = world::ChunkColumn::kHeight;
                    while (h > 0 && block::def(c->block(x, h - 1, z)).opacity == 0) --h;
                    c->heightMap[usize(z * 16 + x)] = u8(h);
                }
            }
        }
    }

private:
    static world::ChunkColumn* columnAt(void* ctx, i32 cx, i32 cz)
    {
        auto* self = static_cast<TestWorld*>(ctx);
        if (cx < -kRadius || cx > kRadius || cz < -kRadius || cz > kRadius) return nullptr;
        const usize i = usize((cz + kRadius) * (kRadius * 2 + 1) + (cx + kRadius));
        return self->columns_[i].get();
    }

    static void onChanged(void* ctx, i32, int, i32)
    {
        ++static_cast<TestWorld*>(ctx)->changes_;
    }

    std::vector<std::unique_ptr<world::ChunkColumn>> columns_;
    std::unique_ptr<TickWorld> world_;
    int changes_ = 0;
};

}  // namespace

// ---- the clock --------------------------------------------------------

TEST(the_timer_owes_nothing_on_its_first_call)
{
    // The first call establishes the origin. A world that took four seconds to
    // load must not run eighty ticks on its opening frame.
    TickTimer timer;
    timer.advance(1000.0);
    CHECK_EQ(timer.elapsedTicks(), 0);
}

TEST(the_timer_turns_a_second_of_real_time_into_twenty_ticks)
{
    TickTimer timer;
    timer.advance(0.0);
    int total = 0;
    // Twenty frames of 50 ms.
    for (int i = 1; i <= 20; ++i) {
        timer.advance(double(i) * 0.05);
        total += timer.elapsedTicks();
    }
    CHECK_EQ(total, 20);
}

TEST(the_timer_keeps_the_fraction_between_frames)
{
    // 60 fps is not a divisor of 20, so most frames owe nothing and every
    // third owes one. If the fraction were discarded the day would never end.
    TickTimer timer;
    timer.advance(0.0);
    int total = 0;
    for (int i = 1; i <= 60; ++i) {
        timer.advance(double(i) / 60.0);
        total += timer.elapsedTicks();
    }
    CHECK_EQ(total, 20);
    CHECK(timer.partialTicks() >= 0.0f && timer.partialTicks() < 1.0f);
}

TEST(the_timer_drops_ticks_past_the_tenth_rather_than_deferring_them)
{
    // a1.1.2 lets the world fall behind rather than spiral. A one-second stall
    // owes 20 ticks, 10 run, and the other 10 are gone.
    TickTimer timer;
    timer.advance(0.0);
    timer.advance(1.0);
    CHECK_EQ(timer.elapsedTicks(), 10);
    CHECK_EQ(timer.droppedTicks(), (i64) 10);
}

TEST(the_timer_clamps_the_delta_before_scaling_it)
{
    // Ten seconds of stall is clamped to one second of real time first, so it
    // cannot inject two hundred ticks and then throw away a hundred and ninety.
    TickTimer timer;
    timer.advance(0.0);
    timer.advance(10.0);
    CHECK_EQ(timer.elapsedTicks(), 10);
    CHECK_EQ(timer.droppedTicks(), (i64) 10);
}

// ---- the scheduler ----------------------------------------------------

TEST(the_scheduler_orders_by_time_then_by_insertion)
{
    TickScheduler s;
    s.schedule(0, 1, 0, 8, 100);
    s.schedule(0, 2, 0, 8, 50);
    s.schedule(0, 3, 0, 8, 50);  // same time as the one before, inserted later

    CHECK_EQ(s.pop().y, (i16) 2);
    CHECK_EQ(s.pop().y, (i16) 3);
    CHECK_EQ(s.pop().y, (i16) 1);
}

TEST(the_scheduler_refuses_a_duplicate_and_accepts_a_different_block)
{
    TickScheduler s;
    CHECK(s.schedule(4, 5, 6, 8, 10));
    CHECK(!s.schedule(4, 5, 6, 8, 99));   // same position, same block
    CHECK(s.schedule(4, 5, 6, 9, 10));    // same position, different block
    CHECK_EQ((long long) s.size(), 2LL);
}

TEST(the_scheduler_takes_an_entry_back_after_it_is_popped)
{
    TickScheduler s;
    CHECK(s.schedule(1, 2, 3, 8, 5));
    CHECK(!s.schedule(1, 2, 3, 8, 5));
    (void) s.pop();
    CHECK(s.schedule(1, 2, 3, 8, 5));
    CHECK(s.consistent());
}

TEST(the_scheduler_stays_consistent_across_a_long_churn)
{
    // The tombstone sweep runs several times over this, which is the part
    // worth exercising: a rebuild that loses an entry looks exactly like a
    // duplicate being allowed in.
    TickScheduler s(256);
    i64 accepted = 0;
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < 100; ++i) {
            if (s.schedule(i32(i), round % 100, i32(round), 8, round)) ++accepted;
        }
        while (s.dueAt(round)) (void) s.pop();
        CHECK(s.consistent());
    }
    CHECK(accepted > 0);
    CHECK(s.empty());
}

TEST(the_scheduler_refuses_the_newest_when_it_is_full_and_counts_it)
{
    TickScheduler s(8);
    for (int i = 0; i < 8; ++i) CHECK(s.schedule(i32(i), 0, 0, 8, 0));
    CHECK(!s.schedule(99, 0, 0, 8, 0));
    CHECK_EQ(s.overflowed(), (i64) 1);
    CHECK_EQ((long long) s.size(), 8LL);
}

TEST(negative_coordinates_are_their_own_entries)
{
    // Chunk and block coordinates are signed and the identity hash mixes them;
    // -1 and 1 colliding would silently merge two updates into one.
    TickScheduler s;
    CHECK(s.schedule(-1, 4, -1, 8, 0));
    CHECK(s.schedule(1, 4, 1, 8, 0));
    CHECK(s.schedule(-1, 4, 1, 8, 0));
    CHECK(s.schedule(1, 4, -1, 8, 0));
    CHECK_EQ((long long) s.size(), 4LL);
}

// ---- block behaviour --------------------------------------------------

// The behaviours below are driven by calling `updateTick` where the world
// would have called it. Waiting for a random tick to land on one nominated
// block is 1 in 32,768 an attempt, so a test that does that is measuring the
// sampler rather than the behaviour -- and one that runs long enough to be
// sure takes seconds. `a_random_tick_only_reaches_blocks_that_asked_for_one`
// and `the_tick_area_never_reaches_past_what_is_loaded` cover the sampler.
TEST(grass_spreads_onto_neighbouring_dirt)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 60, 0, mcver::Block::Grass);

    for (int i = 0; i < 200; ++i) {
        tick::updateTick(t.w(), 0, 60, 0, bid(mcver::Block::Grass), t.w().random());
    }

    const bool spread = t.get(1, 60, 0) == bid(mcver::Block::Grass) ||
                        t.get(-1, 60, 0) == bid(mcver::Block::Grass) ||
                        t.get(0, 60, 1) == bid(mcver::Block::Grass) ||
                        t.get(0, 60, -1) == bid(mcver::Block::Grass);
    CHECK(spread);
}

TEST(grass_does_not_spread_under_a_lid)
{
    // Every candidate cell has a stone lid one block above it. The spread test
    // wants the cell above the target to be neither dark nor solid-or-liquid,
    // and stone is the second of those.
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 60, 0, mcver::Block::Grass);
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) t.set(dx, 61, dz, mcver::Block::Stone);
    }
    // Except over the grass itself, which would kill it instead.
    t.set(0, 61, 0, mcver::Block::Air);

    for (int i = 0; i < 200; ++i) {
        tick::updateTick(t.w(), 0, 60, 0, bid(mcver::Block::Grass), t.w().random());
    }
    for (i32 dx = -1; dx <= 1; ++dx) {
        for (i32 dz = -1; dz <= 1; ++dz) {
            if (dx == 0 && dz == 0) continue;
            CHECK_EQ((long long) t.get(dx, 60, dz), (long long) bid(mcver::Block::Dirt));
        }
    }
}

TEST(grass_under_an_opaque_block_turns_back_into_dirt)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 60, 0, mcver::Block::Grass);
    t.set(0, 61, 0, mcver::Block::Stone);
    // A real column has no sky light inside an opaque block, and grass reads
    // the light at the cell above itself -- which is the stone.
    t.setSkyLight(0, 61, 0, 0);

    // One in four ticks, so a handful of calls is certain and one is not.
    for (int i = 0; i < 40 && t.get(0, 60, 0) == bid(mcver::Block::Grass); ++i) {
        tick::updateTick(t.w(), 0, 60, 0, bid(mcver::Block::Grass), t.w().random());
    }
    CHECK_EQ((long long) t.get(0, 60, 0), (long long) bid(mcver::Block::Dirt));
}

TEST(a_flower_on_stone_is_removed_when_its_ground_changes)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 61, 0, mcver::Block::Dandelion);

    // Still there while the ground is dirt.
    t.w().notifyNeighbours(0, 60, 0, bid(mcver::Block::Dirt));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Dandelion));

    // Not once the ground is stone: `BlockFlower.canThisPlantGrowOnThisBlockID`
    // is grass, dirt and farmland and nothing else.
    t.w().setBlockWithNotify(0, 60, 0, bid(mcver::Block::Stone));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(a_torch_falls_when_what_it_hangs_on_goes_away)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Torch);
    t.w().setDataRaw(0, 61, 0, 5);  // metadata 5: standing on the block below

    t.w().setBlockWithNotify(0, 60, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(sand_falls_to_the_bottom_of_the_hole_under_it)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 50);
    t.set(0, 60, 0, mcver::Block::Sand);

    // Neighbour notification schedules the fall rather than doing it, so the
    // scheduler has to run for anything to happen.
    t.w().notifyNeighbours(0, 61, 0, bid(mcver::Block::Air));
    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 10; ++i) t.w().tick(&centre, 1, 0);

    CHECK_EQ((long long) t.get(0, 60, 0), (long long) bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(0, 51, 0), (long long) bid(mcver::Block::Sand));
}

TEST(sand_uses_its_own_tick_rate_of_three)
{
    // The delay is read from the generated table, not from a literal here, so
    // this is a check that the table carries what the jar says it carries.
    CHECK_EQ((long long) block::def(bid(mcver::Block::Sand)).tickRate, 3LL);
    CHECK_EQ((long long) block::def(bid(mcver::Block::FlowingWater)).tickRate, 5LL);
    CHECK_EQ((long long) block::def(bid(mcver::Block::FlowingLava)).tickRate, 30LL);
    CHECK(block::def(bid(mcver::Block::Grass)).tickRandomly);
    CHECK(!block::def(bid(mcver::Block::Stone)).tickRandomly);
    // Still water does not tick randomly and still lava does, which is the
    // one-line branch in BlockStationary's constructor.
    CHECK(!block::def(bid(mcver::Block::Water)).tickRandomly);
    CHECK(block::def(bid(mcver::Block::Lava)).tickRandomly);
}

TEST(ice_melts_into_water_in_bright_light)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Ice);

    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Ice), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Water));
}

TEST(ice_in_the_dark_stays_ice)
{
    // The threshold is `11 - lightOpacity[ice]`, and ice's opacity is 3, so it
    // survives at a stored sky light of 8 and melts at 9.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Ice);
    t.setSkyLight(0, 61, 0, 8);
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Ice), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Ice));

    t.setSkyLight(0, 61, 0, 9);
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Ice), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Water));
}

TEST(sugar_cane_grows_three_tall_beside_water_and_no_further)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(1, 60, 0, mcver::Block::Water);
    t.set(0, 61, 0, mcver::Block::SugarCane);

    // Metadata counts to 15 and then the next block appears, so sixteen calls
    // per block of height.
    for (int i = 0; i < 16 * 6; ++i) {
        for (int y = 61; y <= 63; ++y) {
            if (t.get(0, y, 0) == bid(mcver::Block::SugarCane)) {
                tick::updateTick(t.w(), 0, y, 0, bid(mcver::Block::SugarCane), t.w().random());
            }
        }
    }
    CHECK_EQ((long long) t.get(0, 62, 0), (long long) bid(mcver::Block::SugarCane));
    CHECK_EQ((long long) t.get(0, 63, 0), (long long) bid(mcver::Block::SugarCane));
    CHECK_EQ((long long) t.get(0, 64, 0), (long long) bid(mcver::Block::Air));
}

TEST(sugar_cane_without_water_beside_it_is_removed)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 61, 0, mcver::Block::SugarCane);
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::SugarCane), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(farmland_wets_from_water_and_dries_back_to_dirt_without_it)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 60, 0, mcver::Block::Farmland);
    t.set(3, 60, 0, mcver::Block::Water);  // inside the 9x2x9 box

    for (int i = 0; i < 40; ++i) {
        tick::updateTick(t.w(), 0, 60, 0, bid(mcver::Block::Farmland), t.w().random());
    }
    CHECK_EQ((long long) t.dataAt(0, 60, 0), 7LL);

    // Take the water away and it dries out one step at a time, then reverts.
    t.set(3, 60, 0, mcver::Block::Dirt);
    for (int i = 0; i < 400 && t.get(0, 60, 0) == bid(mcver::Block::Farmland); ++i) {
        tick::updateTick(t.w(), 0, 60, 0, bid(mcver::Block::Farmland), t.w().random());
    }
    CHECK_EQ((long long) t.get(0, 60, 0), (long long) bid(mcver::Block::Dirt));
}

TEST(farmland_under_a_crop_never_reverts)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 60, 0, mcver::Block::Farmland);
    t.set(0, 61, 0, mcver::Block::Wheat);

    for (int i = 0; i < 400; ++i) {
        tick::updateTick(t.w(), 0, 60, 0, bid(mcver::Block::Farmland), t.w().random());
    }
    CHECK_EQ((long long) t.get(0, 60, 0), (long long) bid(mcver::Block::Farmland));
}

TEST(wheat_grows_on_farmland_and_stops_at_seven)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 60, 0, mcver::Block::Farmland);
    t.set(0, 61, 0, mcver::Block::Wheat);

    for (int i = 0; i < 20000 && t.dataAt(0, 61, 0) < 7; ++i) {
        tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Wheat), t.w().random());
    }
    CHECK_EQ((long long) t.dataAt(0, 61, 0), 7LL);

    // Seven is ripe, and a ripe crop does not keep counting.
    for (int i = 0; i < 200; ++i) {
        tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Wheat), t.w().random());
    }
    CHECK_EQ((long long) t.dataAt(0, 61, 0), 7LL);
}

TEST(leaves_out_of_reach_of_a_log_decay_and_leaves_beside_one_do_not)
{
    // Both leaf blocks are one block clear of the ground, because `iz.h`
    // starts its distance at 16 when the block *below* is solid -- so leaves
    // resting on the ground never decay in a1.1.2 at all. That is the jar's
    // behaviour and the next test pins it rather than leaving it as folklore.
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 62, 0, mcver::Block::Log);
    t.set(1, 62, 0, mcver::Block::Leaves);   // touching the log
    t.set(8, 62, 0, mcver::Block::Leaves);   // nowhere near one

    // A pass with metadata 0 records the distance; the tick after acts on it.
    for (int i = 0; i < 4; ++i) {
        if (t.get(1, 62, 0) == bid(mcver::Block::Leaves)) {
            tick::updateTick(t.w(), 1, 62, 0, bid(mcver::Block::Leaves), t.w().random());
        }
        if (t.get(8, 62, 0) == bid(mcver::Block::Leaves)) {
            tick::updateTick(t.w(), 8, 62, 0, bid(mcver::Block::Leaves), t.w().random());
        }
    }
    CHECK_EQ((long long) t.get(1, 62, 0), (long long) bid(mcver::Block::Leaves));
    CHECK_EQ((long long) t.get(8, 62, 0), (long long) bid(mcver::Block::Air));
}

TEST(leaves_resting_on_solid_ground_never_decay)
{
    // `iz.h(Lcn;III)V` opens with
    //     int base = world.getBlockMaterial(x, y-1, z).isSolid() ? 16 : 0;
    // and 16 is the same answer a log gives, so a leaf block sitting on
    // anything solid records a distance of 15 and stays there for ever. It is
    // surprising enough to be worth a test that says it is deliberate.
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(8, 61, 0, mcver::Block::Leaves);

    for (int i = 0; i < 20; ++i) {
        tick::updateTick(t.w(), 8, 61, 0, bid(mcver::Block::Leaves), t.w().random());
    }
    CHECK_EQ((long long) t.get(8, 61, 0), (long long) bid(mcver::Block::Leaves));
}

TEST(a_mushroom_wants_darkness_where_a_flower_wants_light)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(0, 61, 0, mcver::Block::BrownMushroom);

    // Full sky light, so the mushroom's `lightValue > 13` test removes it.
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::BrownMushroom), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));

    // In the dark it stays -- and on dirt, which is an opaque cube, which is
    // the mushroom's own ground test rather than the flower's.
    t.set(0, 61, 0, mcver::Block::BrownMushroom);
    t.setSkyLight(0, 61, 0, 0);
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::BrownMushroom), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::BrownMushroom));
}

TEST(a_random_tick_only_reaches_blocks_that_asked_for_one)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);

    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 50; ++i) t.w().tick(&centre, 1, 0);

    // 80 attempts per chunk per tick, and one chunk at radius 0.
    CHECK_EQ(t.w().stats().randomTicks, (i64) 50 * 80);
    CHECK_EQ(t.w().stats().randomTicksRun, (i64) 0);
    CHECK_EQ(t.w().stats().blocksChanged, (i64) 0);
}

TEST(the_tick_area_never_reaches_past_what_is_loaded)
{
    // The world holds a 5x5 patch of columns. Asking for a1.1.2's own radius
    // of 9 must tick the 25 it has and skip the 336 it does not, rather than
    // dereferencing a null column.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    const TickWorld::Centre centre{0, 0};
    t.w().tick(&centre, 1, TickWorld::kChunkTickRadius);
    CHECK_EQ(t.w().stats().chunksTicked, (i64) 25);
}

TEST(the_world_clock_advances_one_tick_at_a_time)
{
    TestWorld t;
    t.w().setTime(23990);
    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 20; ++i) t.w().tick(&centre, 1, 0);
    CHECK_EQ(t.w().time(), (i64) 24010);
}

TEST(a_scheduled_update_waits_for_its_due_tick)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 50);
    t.set(0, 60, 0, mcver::Block::Sand);
    t.w().scheduleBlockUpdate(0, 60, 0, bid(mcver::Block::Sand));

    const TickWorld::Centre centre{0, 0};
    // Sand's rate is 3, so nothing happens on the first two ticks.
    t.w().tick(&centre, 1, 0);
    t.w().tick(&centre, 1, 0);
    CHECK_EQ((long long) t.get(0, 60, 0), (long long) bid(mcver::Block::Sand));
    t.w().tick(&centre, 1, 0);
    CHECK_EQ((long long) t.get(0, 60, 0), (long long) bid(mcver::Block::Air));
}

TEST(a_scheduled_update_is_dropped_at_the_edge_of_what_is_loaded)
{
    // The original refuses to schedule anything whose 17-cube neighbourhood is
    // not resident. Chunk 2 is the last one this world holds, so a block in it
    // is within 8 of ground that does not exist.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 50);
    t.w().scheduleBlockUpdate(2 * 16 + 15, 60, 0, bid(mcver::Block::Sand));
    CHECK_EQ((long long) t.w().scheduler().size(), 0LL);
    CHECK_EQ(t.w().stats().scheduledDropped, (i64) 1);
}

// ---- fluids -----------------------------------------------------------
//
// Driven through the scheduler rather than by calling `updateTick` directly,
// because a fluid's whole behaviour is the chain of scheduled updates it sets
// off: a spread that happened in one call would prove nothing about the thing
// that actually runs on a console.

namespace {

// **A source is placed as the *flowing* form.** That is not a quirk of the
// test: a1.1.2's bucket places `Block.waterMoving` with metadata 0, and the
// still block is what a flowing one becomes once it has settled. Placing the
// still form and scheduling it does nothing at all -- correctly, because a
// still block does not tick, which is the whole reason a lake is free.
void placeSource(TestWorld& t, i32 x, int y, i32 z, mcver::Block flowing)
{
    t.set(x, y, z, flowing);
    t.w().setDataRaw(x, y, z, 0);
    t.w().scheduleBlockUpdate(x, y, z, bid(flowing));
}

// Runs the world until nothing is scheduled, or until `limit` ticks have gone
// by. Returns the number of ticks it took, so a test can assert that water is
// faster than lava rather than only that both settle.
int settle(TestWorld& t, int limit = 4000)
{
    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < limit; ++i) {
        if (t.w().scheduler().empty()) return i;
        t.w().tick(&centre, 1, 0);
    }
    return limit;
}

}  // namespace

TEST(a_water_source_spreads_seven_blocks_and_stops)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeSource(t, 0, 61, 0, mcver::Block::FlowingWater);

    CHECK(settle(t) < 4000);

    // Level rises by one per block, and level 7 is the last that exists, so
    // the seventh block along is wet and the eighth is not.
    CHECK(t.w().blockAt(7, 61, 0) != bid(mcver::Block::Air));
    CHECK_EQ((long long) t.w().blockAt(8, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(water_settles_into_its_still_form_so_a_lake_costs_nothing)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeSource(t, 0, 61, 0, mcver::Block::FlowingWater);
    settle(t);

    // Everything it made is `Water`, the still form -- nothing is left as
    // `FlowingWater` with an update pending, which is what would make a lake
    // cost a tick for ever.
    for (i32 dx = -7; dx <= 7; ++dx) {
        const BlockId b = t.w().blockAt(dx, 61, 0);
        CHECK(b != bid(mcver::Block::FlowingWater));
    }
    CHECK(t.w().scheduler().empty());
}

TEST(water_falls_before_it_spreads)
{
    // A one-block hole beside the source. The fluid should go down it rather
    // than crawl past it, and the far side of the hole should stay dry.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(1, 60, 0, mcver::Block::Air);  // the hole
    placeSource(t, 0, 61, 0, mcver::Block::FlowingWater);
    settle(t);

    CHECK(t.w().blockAt(1, 60, 0) != bid(mcver::Block::Air));
}

TEST(two_water_sources_with_a_gap_make_a_third)
{
    // The infinite-water rule: two source neighbours over solid ground become
    // a source themselves.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeSource(t, -1, 61, 0, mcver::Block::FlowingWater);
    placeSource(t, 1, 61, 0, mcver::Block::FlowingWater);
    settle(t);

    CHECK_EQ((long long) t.w().blockAt(0, 61, 0), (long long) bid(mcver::Block::Water));
    CHECK_EQ((long long) t.dataAt(0, 61, 0), 0LL);  // level 0 is a source
}

TEST(lava_reaches_less_far_than_water_because_its_level_drops_by_two)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeSource(t, 0, 61, 0, mcver::Block::FlowingLava);
    settle(t, 20000);

    // Lava's step is 2, so levels go 0, 2, 4, 6 and the fourth block out is
    // the last one -- against water's eighth.
    CHECK(t.w().blockAt(3, 61, 0) != bid(mcver::Block::Air));
    CHECK_EQ((long long) t.w().blockAt(4, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(lava_meeting_water_turns_to_stone)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Lava);        // a source: level 0
    t.set(1, 61, 0, mcver::Block::Water);

    // The notification is what a1.1.2 uses; `checkForHarden` runs from it.
    t.w().notifyNeighbours(1, 61, 0, bid(mcver::Block::Water));

    // A lava *source* becomes obsidian; anything thinner becomes cobblestone.
    CHECK_EQ((long long) t.w().blockAt(0, 61, 0), (long long) bid(mcver::Block::Obsidian));
}

TEST(flowing_lava_meeting_water_turns_to_cobblestone)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::FlowingLava);
    t.w().setDataRaw(0, 61, 0, 2);  // a level, not a source
    t.set(1, 61, 0, mcver::Block::Water);

    t.w().notifyNeighbours(1, 61, 0, bid(mcver::Block::Water));
    CHECK_EQ((long long) t.w().blockAt(0, 61, 0), (long long) bid(mcver::Block::Cobblestone));
}

TEST(a_still_fluid_wakes_up_when_a_neighbour_changes)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Water);
    t.set(1, 61, 0, mcver::Block::Stone);

    CHECK(t.w().scheduler().empty());
    // Take the wall away: the still source must become flowing and schedule
    // itself, or the lake would never notice the world had changed.
    t.w().setBlockWithNotify(1, 61, 0, bid(mcver::Block::Air));
    CHECK(!t.w().scheduler().empty());
    CHECK_EQ((long long) t.w().blockAt(0, 61, 0), (long long) bid(mcver::Block::FlowingWater));
}

TEST(water_does_not_flow_through_a_ladder)
{
    // Five blocks stop a fluid without being solid, and a1.1.2 names them one
    // by one: both doors, a sign post, a ladder and sugar cane. A ladder is the
    // cheapest of them to stand up in a test.
    //
    // It has to be a **wall**, not a single block. Water reaches seven blocks
    // in every direction, so one ladder is something to flow around and proves
    // nothing; the wall spans further than the water can reach so that "dry on
    // the far side" means the ladder stopped it rather than that it ran out.
    //
    // **And the ladders need a wall of their own**, which they did not used to.
    // A ladder is written with metadata 2..5 naming the side it hangs on, and
    // `br.a(Lcn;IIII)V` drops one whose named side has stopped being an opaque
    // cube. This test used to stand nineteen ladders in mid air at metadata 0 --
    // a value the game never writes -- and passed only because nothing checked.
    // Once the support rule was transcribed, the first neighbour change knocked
    // the whole row down and the water walked through the gap, which is a1.1.2's
    // answer to a mid-air ladder too. So: stone at x = 3, ladders at x = 2 with
    // metadata 4, and the source two cells clear of both.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    for (i32 z = -9; z <= 9; ++z) {
        t.set(3, 61, z, mcver::Block::Stone);
        t.set(2, 61, z, mcver::Block::Ladder, 4);
    }
    placeSource(t, 0, 61, 0, mcver::Block::FlowingWater);
    settle(t);

    CHECK_EQ((long long) t.w().blockAt(2, 61, 0), (long long) bid(mcver::Block::Ladder));
    // …and it did reach the wall, so the wall is what stopped it.
    CHECK(t.w().blockAt(1, 61, 0) != bid(mcver::Block::Air));
    CHECK(t.w().blockAt(0, 61, 3) != bid(mcver::Block::Air));
}

TEST(water_washes_a_flower_away)
{
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 60);
    t.set(1, 61, 0, mcver::Block::Dandelion);
    placeSource(t, 0, 61, 0, mcver::Block::FlowingWater);
    settle(t);

    CHECK(t.w().blockAt(1, 61, 0) != bid(mcver::Block::Dandelion));
}

// ---- fire -------------------------------------------------------------

TEST(the_fire_tables_are_three_different_questions)
{
    // Six blocks are in BlockFire's own tables; fourteen have a burnable
    // material. Conflating them is what would make lava light a chest that
    // fire itself cannot spread through -- or fail to.
    CHECK_EQ((long long) block::def(bid(mcver::Block::Planks)).burnEncourage, 5LL);
    CHECK_EQ((long long) block::def(bid(mcver::Block::Planks)).burnCatch, 20LL);
    CHECK_EQ((long long) block::def(bid(mcver::Block::Leaves)).burnEncourage, 30LL);
    CHECK_EQ((long long) block::def(bid(mcver::Block::Leaves)).burnCatch, 60LL);
    CHECK_EQ((long long) block::def(bid(mcver::Block::Tnt)).burnCatch, 100LL);

    // A chest burns in lava's eyes and is invisible to the fire tables.
    CHECK(block::def(bid(mcver::Block::Chest)).canBurn);
    CHECK_EQ((long long) block::def(bid(mcver::Block::Chest)).burnEncourage, 0LL);

    CHECK(!block::def(bid(mcver::Block::Stone)).canBurn);
    CHECK_EQ((long long) block::def(bid(mcver::Block::Stone)).burnEncourage, 0LL);
}

TEST(fire_on_stone_with_nothing_to_burn_goes_out_once_it_has_aged)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Fire);

    // Young fire on solid ground survives -- age must pass 3 first.
    for (int i = 0; i < 3; ++i) {
        tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Fire), t.w().random());
        CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Fire));
    }
    for (int i = 0; i < 10 && t.get(0, 61, 0) == bid(mcver::Block::Fire); ++i) {
        tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Fire), t.w().random());
    }
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(fire_in_the_air_with_nothing_to_burn_goes_out_at_once)
{
    // No solid ground under it and no fuel beside it: `og.a` removes it on the
    // first tick rather than letting it age.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 50);
    t.set(0, 61, 0, mcver::Block::Fire);
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Fire), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(fire_spreads_through_a_wooden_structure_and_consumes_it)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    for (i32 dx = 0; dx <= 6; ++dx) t.set(dx, 61, 0, mcver::Block::Planks);
    t.set(0, 62, 0, mcver::Block::Fire);

    const TickWorld::Centre centre{0, 0};
    t.w().scheduleBlockUpdate(0, 62, 0, bid(mcver::Block::Fire));
    for (int i = 0; i < 4000 && !t.w().scheduler().empty(); ++i) {
        t.w().tick(&centre, 1, 0);
    }

    // The plank it was standing on is gone -- but "gone" means air *or* fire:
    // `tryToCatchBlockOnFire` replaces the block with fire on one roll in two
    // and removes it on the other, which is what makes a burning structure
    // develop holes rather than turn into a solid block of flame.
    CHECK(t.get(0, 61, 0) != bid(mcver::Block::Planks));
    int burnt = 0;
    for (i32 dx = 0; dx <= 6; ++dx) {
        if (t.get(dx, 61, 0) != bid(mcver::Block::Planks)) ++burnt;
    }
    CHECK(burnt >= 2);
}

TEST(a_fully_aged_fire_stops_scheduling_itself)
{
    // `og.a` only writes the metadata and re-schedules `if (age < 15)`, so a
    // fire that has reached 15 and is still being fed drops out of the
    // scheduled list entirely and is revisited only by a **random** tick --
    // fire is one of the 25 blocks that get those. It looks like a leak and is
    // not, which is why this is pinned.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(1, 61, 0, mcver::Block::Planks);   // fuel, so it never goes out
    t.set(0, 61, 0, mcver::Block::Fire);
    t.w().setDataRaw(0, 61, 0, 15);

    // Placing it scheduled it -- `og.e`, onBlockAdded -- so the list has to be
    // emptied before the question this test is actually asking can be put.
    t.w().scheduler().clear();
    CHECK(t.w().scheduler().empty());
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Fire), t.w().random());
    CHECK(t.w().scheduler().empty());
    CHECK(block::def(bid(mcver::Block::Fire)).tickRandomly);
}

TEST(fire_does_not_spread_into_stone)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    for (i32 dx = 1; dx <= 6; ++dx) t.set(dx, 61, 0, mcver::Block::Stone);
    t.set(0, 61, 0, mcver::Block::Fire);

    const TickWorld::Centre centre{0, 0};
    t.w().scheduleBlockUpdate(0, 61, 0, bid(mcver::Block::Fire));
    for (int i = 0; i < 400; ++i) t.w().tick(&centre, 1, 0);

    for (i32 dx = 1; dx <= 6; ++dx) {
        CHECK_EQ((long long) t.get(dx, 61, 0), (long long) bid(mcver::Block::Stone));
    }
}

TEST(lava_sets_light_to_what_is_above_it)
{
    // `hn.a(...)`: still lava wanders up to three steps upwards looking for air
    // with something burnable beside it. The plank roof gives it one.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::Lava);
    for (i32 dx = -2; dx <= 2; ++dx) {
        for (i32 dz = -2; dz <= 2; ++dz) t.set(dx, 64, dz, mcver::Block::Planks);
    }

    bool lit = false;
    for (int i = 0; i < 400 && !lit; ++i) {
        tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::Lava), t.w().random());
        for (i32 dx = -3; dx <= 3 && !lit; ++dx) {
            for (i32 dz = -3; dz <= 3 && !lit; ++dz) {
                for (int y = 62; y <= 64; ++y) {
                    if (t.get(dx, y, dz) == bid(mcver::Block::Fire)) lit = true;
                }
            }
        }
    }
    CHECK(lit);
}

// ---- redstone ---------------------------------------------------------
//
// A torch is the only power source a1.1.2 has that needs no player, so every
// circuit below is built out of one. `setBlockAndDataRaw` is used to place the
// torch with its face metadata in one write, because `setBlockRaw` clears the
// metadata exactly as `Chunk.setBlockID` does.

namespace {

// **With notification**, which is what actually placing a block does --
// `setBlockAndMetadataWithNotify`. The raw setter runs `onBlockAdded` but not
// the neighbour fan-out, and `Block.onBlockAdded` for a torch notifies the six
// cells *around* it rather than the six cells *of* it, so a wire laid beside a
// torch placed raw never hears about it. That is faithful; the test just has
// to place blocks the way the game does.
void placeTorch(TestWorld& t, i32 x, int y, i32 z)
{
    t.w().setBlockAndDataWithNotify(x, y, z, bid(mcver::Block::RedstoneTorch), 5);
}

void placeWire(TestWorld& t, i32 x, int y, i32 z)
{
    t.w().setBlockAndDataWithNotify(x, y, z, bid(mcver::Block::RedstoneWire), 0);
}

}  // namespace

TEST(a_lit_torch_powers_the_block_above_it_and_not_the_one_it_stands_on)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeTorch(t, 0, 61, 0);

    // Direct power goes up: the block above is powered, the floor is not.
    CHECK(t.w().isPowered(0, 62, 0));
    CHECK(!t.w().isPowered(0, 60, 0));
}

TEST(wire_carries_fifteen_blocks_and_no_further)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    for (i32 dx = 1; dx <= 20; ++dx) placeWire(t, dx, 61, 0);
    placeTorch(t, 0, 61, 0);

    // The torch powers the wire beside it to 15, and each block costs one.
    CHECK_EQ((long long) t.dataAt(1, 61, 0), 15LL);
    CHECK_EQ((long long) t.dataAt(2, 61, 0), 14LL);
    CHECK_EQ((long long) t.dataAt(15, 61, 0), 1LL);
    CHECK_EQ((long long) t.dataAt(16, 61, 0), 0LL);
    CHECK_EQ((long long) t.dataAt(20, 61, 0), 0LL);
}

TEST(a_wire_goes_dark_when_its_source_is_taken_away)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    for (i32 dx = 1; dx <= 6; ++dx) placeWire(t, dx, 61, 0);
    placeTorch(t, 0, 61, 0);
    CHECK_EQ((long long) t.dataAt(1, 61, 0), 15LL);

    t.w().setBlockWithNotify(0, 61, 0, bid(mcver::Block::Air));
    for (i32 dx = 1; dx <= 6; ++dx) {
        CHECK_EQ((long long) t.dataAt(dx, 61, 0), 0LL);
    }
}

TEST(a_torch_on_a_powered_block_goes_out_which_is_the_not_gate)
{
    // The whole of a1.1.2's logic is this: torch A powers the block under
    // torch B through a wire, and torch B goes dark.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(2, 61, 0, mcver::Block::Stone);          // the block B stands on
    placeTorch(t, 2, 62, 0);                        // torch B, on top of it
    placeWire(t, 1, 61, 0);                         // wire from A into that block
    placeTorch(t, 0, 61, 0);                        // torch A, the input

    CHECK(t.w().isIndirectlyPowered(2, 61, 0));

    // B has been asked to re-evaluate at its rate of 2 rather than at once.
    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 8; ++i) t.w().tick(&centre, 1, 0);

    CHECK_EQ((long long) t.get(2, 62, 0), (long long) bid(mcver::Block::UnlitRedstoneTorch));
}

TEST(an_unpowered_torch_lights_again_when_the_input_goes_away)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(2, 61, 0, mcver::Block::Stone);
    placeTorch(t, 2, 62, 0);
    placeWire(t, 1, 61, 0);
    placeTorch(t, 0, 61, 0);

    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 8; ++i) t.w().tick(&centre, 1, 0);
    CHECK_EQ((long long) t.get(2, 62, 0), (long long) bid(mcver::Block::UnlitRedstoneTorch));

    // Pull the input out and it comes back on.
    t.w().setBlockWithNotify(0, 61, 0, bid(mcver::Block::Air));
    for (int i = 0; i < 8; ++i) t.w().tick(&centre, 1, 0);
    CHECK_EQ((long long) t.get(2, 62, 0), (long long) bid(mcver::Block::RedstoneTorch));
}

TEST(wire_needs_something_solid_under_it)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeWire(t, 0, 61, 0);
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::RedstoneWire));

    t.w().setBlockWithNotify(0, 60, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(a_wire_dot_powers_all_four_sides_and_a_straight_run_only_its_ends)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);

    // A lone lit dot: raise it by hand, since nothing is feeding it.
    placeWire(t, 8, 61, 8);
    t.w().setDataRaw(8, 61, 8, 15);
    CHECK(t.w().indirectlyProvidesPowerTo(8, 61, 8, 2));
    CHECK(t.w().indirectlyProvidesPowerTo(8, 61, 8, 4));

    // A straight east-west run powers along x and not along z.
    for (i32 dx = 1; dx <= 3; ++dx) placeWire(t, dx, 61, 0);
    placeTorch(t, 0, 61, 0);
    CHECK(t.w().indirectlyProvidesPowerTo(2, 61, 0, 5));   // +x, along the run
    CHECK(!t.w().indirectlyProvidesPowerTo(2, 61, 0, 2));  // -z, across it
    CHECK(t.w().indirectlyProvidesPowerTo(2, 61, 0, 1));   // straight up, always
}

TEST(lit_redstone_ore_goes_dark_again)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(0, 61, 0, mcver::Block::LitRedstoneOre);
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::LitRedstoneOre), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::RedstoneOre));

    // The unlit one does nothing at all when ticked.
    tick::updateTick(t.w(), 0, 61, 0, bid(mcver::Block::RedstoneOre), t.w().random());
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::RedstoneOre));
    CHECK_EQ((long long) block::def(bid(mcver::Block::RedstoneOre)).tickRate, 30LL);
}

// ---- switches and the door --------------------------------------------
//
// Pressing, flipping and hand-opening are inputs and need a player. Everything
// else is reachable by writing the metadata those inputs would have written,
// which is what these tests do.

TEST(a_lever_that_is_on_powers_what_it_is_attached_to)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    // Face 5 is the floor, bit 3 is "on".
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::Lever), 5 | 8);

    // **Direct power goes into what the lever is attached to**, which for face
    // 5 is the floor -- the block below asks its upper neighbour with side 1,
    // and face 5 answers side 1. The block *above* the lever gets nothing
    // directly; that is the difference between a lever and a torch.
    CHECK(t.w().isPowered(0, 60, 0));
    CHECK(!t.w().isPowered(0, 62, 0));
    // Indirectly it powers everything around it, which is what a wire beside
    // it reads.
    CHECK(t.w().isIndirectlyPowered(0, 60, 0));

    // Off, and it powers nothing.
    t.w().setDataWithNotify(0, 61, 0, 5);
    CHECK(!t.w().isIndirectlyPowered(0, 60, 0));
}

TEST(a_lever_falls_off_when_its_wall_goes_away)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(1, 61, 0, mcver::Block::Stone);            // the wall
    // Face 2 means "attached to the block at +x".
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::Lever), 2);
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Lever));

    t.w().setBlockWithNotify(1, 61, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(a_pressed_button_lets_itself_back_out_twenty_ticks_later)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.set(1, 61, 0, mcver::Block::Stone);
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::StoneButton), 2 | 8);
    t.w().scheduleBlockUpdate(0, 61, 0, bid(mcver::Block::StoneButton));

    CHECK(t.w().isIndirectlyPowered(1, 61, 0));
    CHECK_EQ((long long) block::def(bid(mcver::Block::StoneButton)).tickRate, 20LL);

    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 25; ++i) t.w().tick(&centre, 1, 0);

    CHECK_EQ((long long) t.dataAt(0, 61, 0), 2LL);   // the pressed bit is gone
    CHECK(!t.w().isIndirectlyPowered(1, 61, 0));
}

TEST(a_pressure_plate_under_load_powers_straight_up_and_nothing_else)
{
    // Nothing can stand on it yet, so the load is written by hand -- which is
    // exactly what an entity walking onto it would write.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::StonePressurePlate), 1);

    CHECK(t.w().providesPowerTo(0, 61, 0, 1));       // up
    CHECK(!t.w().providesPowerTo(0, 61, 0, 4));      // sideways
    CHECK(t.w().indirectlyProvidesPowerTo(0, 61, 0, 4));  // but indirectly, yes

    t.w().setDataWithNotify(0, 61, 0, 0);
    CHECK(!t.w().providesPowerTo(0, 61, 0, 1));
}

TEST(a_plate_and_a_button_fall_off_an_unsupported_block)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::StonePressurePlate), 0);
    t.w().setBlockWithNotify(0, 60, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Air));

    // A button has no floor face at all: standing one on the ground and taking
    // its walls away leaves it nothing to hold onto.
    t.set(4, 61, 0, mcver::Block::Stone);
    t.w().setBlockAndDataWithNotify(3, 61, 0, bid(mcver::Block::StoneButton), 2);
    t.w().setBlockWithNotify(4, 61, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(3, 61, 0), (long long) bid(mcver::Block::Air));
}

TEST(a_floor_lever_survives_being_flicked_whichever_way_round_it_lies)
{
    // **The reported bug: flicking a floor lever deleted it.** A lever on the
    // ground is orientation 5 *or* 6 -- `BlockLever.onBlockAdded` rolls
    // `5 + nextInt(2)` for which way the handle lies -- and the flick notifies
    // the lever's own cell, so the support check runs on it immediately.
    // `onNeighborBlockChange` in the original only ever tests orientations 1
    // to 5; 6 is named nowhere and is therefore never dropped.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);

    // Twenty levers along one row. `onBlockAdded` re-rolls the 5/6 pair off the
    // world's random -- `5 + nextInt(2)` in the class file -- so which way round
    // each lies is not what was written, and both must turn up along the row or
    // the roll is not happening.
    bool sawFive = false;
    bool sawSix = false;
    for (i32 x = 0; x < 20; ++x) {
        t.w().setBlockAndDataWithNotify(x, 61, 0, bid(mcver::Block::Lever), 5);
        const int orientation = int(t.dataAt(x, 61, 0)) & 7;
        CHECK(orientation == 5 || orientation == 6);
        sawFive = sawFive || orientation == 5;
        sawSix = sawSix || orientation == 6;
        CHECK_EQ((long long) t.get(x, 61, 0), (long long) bid(mcver::Block::Lever));

        CHECK(tick::blockActivated(t.w(), x, 61, 0));
        CHECK_EQ((long long) t.get(x, 61, 0), (long long) bid(mcver::Block::Lever));
        CHECK_EQ(int(t.dataAt(x, 61, 0)), orientation + 8);

        // And back off again, still there.
        CHECK(tick::blockActivated(t.w(), x, 61, 0));
        CHECK_EQ((long long) t.get(x, 61, 0), (long long) bid(mcver::Block::Lever));
        CHECK_EQ(int(t.dataAt(x, 61, 0)), orientation);

        // It still goes when the floor does, which is the check that must not
        // have been thrown away with the false answer.
        t.w().setBlockWithNotify(x, 60, 0, bid(mcver::Block::Air));
        CHECK_EQ((long long) t.get(x, 61, 0), (long long) bid(mcver::Block::Air));
        t.set(x, 60, 0, mcver::Block::Stone);
    }
    CHECK(sawFive);
    CHECK(sawSix);
}

TEST(a_lever_placed_against_a_wall_from_below_takes_the_wall)
{
    // The underside is the one face `onBlockPlaced` leaves alone, so its row in
    // the placement table is 0 -- which is not an orientation. `onBlockAdded`
    // is what fills it in from whatever is beside the lever, and this is that.
    CHECK_EQ(int(block::placementMetadata(bid(mcver::Block::Lever), mesh::kFaceNegY)), 0);

    TestWorld t;
    t.set(1, 61, 0, mcver::Block::Stone);
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::Lever), 0);
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Lever));
    // Face 2 is "+x", the wall it found.
    CHECK_EQ(int(t.dataAt(0, 61, 0)) & 7, 2);

    // And with nothing to hold onto, nothing is invented: the support check is
    // what removes it, not a made-up orientation.
    TestWorld bare;
    bare.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::Lever), 0);
    CHECK_EQ(int(bare.dataAt(0, 61, 0)) & 7, 0);
}

TEST(a_placed_lever_is_off_and_lands_on_the_floor_from_the_top_face)
{
    // The other half of the same report: the placement table used to hand out
    // metadata 14 -- a floor lever already switched on -- because the sweep
    // that generated it punched every block after placing it.
    const int floorMeta = int(block::placementMetadata(bid(mcver::Block::Lever), mesh::kFacePosY));
    CHECK(floorMeta == 5 || floorMeta == 6);

    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    t.w().setBlockAndDataWithNotify(0, 61, 0, bid(mcver::Block::Lever), u8(floorMeta));
    CHECK_EQ((long long) t.get(0, 61, 0), (long long) bid(mcver::Block::Lever));
    CHECK_EQ(int(t.dataAt(0, 61, 0)) & 8, 0);
    // Off, so it powers nothing before anyone touches it.
    CHECK(!tick::providesPowerTo(t.w(), 0, 61, 0, 1, bid(mcver::Block::Lever)));
}

namespace {

// A whole door: the lower half at y, the upper half above it with bit 3 set.
void placeDoor(TestWorld& t, i32 x, int y, i32 z)
{
    t.w().setBlockAndDataWithNotify(x, y, z, bid(mcver::Block::WoodenDoor), 0);
    t.w().setBlockAndDataWithNotify(x, y + 1, z, bid(mcver::Block::WoodenDoor), 8);
}

bool doorIsOpen(TestWorld& t, i32 x, int y, i32 z) { return (t.dataAt(x, y, z) & 4) != 0; }

}  // namespace

TEST(a_door_opens_when_a_circuit_powers_it_and_closes_again)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeDoor(t, 3, 61, 0);
    CHECK(!doorIsOpen(t, 3, 61, 0));

    // A torch feeding wire that runs up to the door's foot.
    for (i32 dx = 1; dx <= 2; ++dx) placeWire(t, dx, 61, 0);
    placeTorch(t, 0, 61, 0);

    CHECK(doorIsOpen(t, 3, 61, 0));
    // Both halves agree, which is what stops a door rendering half open.
    CHECK(doorIsOpen(t, 3, 62, 0));

    t.w().setBlockWithNotify(0, 61, 0, bid(mcver::Block::Air));
    CHECK(!doorIsOpen(t, 3, 61, 0));
    CHECK(!doorIsOpen(t, 3, 62, 0));
}

TEST(a_door_whose_lower_half_goes_away_takes_the_upper_half_with_it)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeDoor(t, 3, 61, 0);

    t.w().setBlockWithNotify(3, 61, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(3, 62, 0), (long long) bid(mcver::Block::Air));
}

TEST(a_door_needs_something_solid_under_it)
{
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 60);
    placeDoor(t, 3, 61, 0);

    t.w().setBlockWithNotify(3, 60, 0, bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(3, 61, 0), (long long) bid(mcver::Block::Air));
    CHECK_EQ((long long) t.get(3, 62, 0), (long long) bid(mcver::Block::Air));
}

// ---- the recursion bound ----------------------------------------------
//
// `writeBlock` calls `blockAdded`/`blockRemoved`, behaviours call
// `notifyNeighbours`, and `notifyNeighbours` dispatches `neighbourChanged`
// synchronously -- which can write another block. Nothing bounded that, and a
// 3DSX main thread has 32 KB of stack it cannot enlarge. See
// TickWorld::kCascadeStackBudget.
//
// These are the cases that used to unwind as one deep recursion. They cannot
// assert on stack depth directly, so they assert on the two things that are
// observable: the cascade completes, and the deferral counter says whether the
// budget was reached.

TEST(a_large_fire_field_going_out_completes_without_recursing_without_end)
{
    // Fire needs something to stand on and something to burn; a field of it
    // over netherrack-like ground unwinds neighbour by neighbour when the
    // ground goes, and the depth of that unwind is the size of the field.
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 40);

    constexpr int kSide = 24;   // 576 cells, far past any plausible stack
    for (int x = 0; x < kSide; ++x) {
        for (int z = 0; z < kSide; ++z) {
            t.set(i32(x), 41, i32(z), mcver::Block::Fire);
        }
    }

    // Take the floor out from under one corner. Every fire in the field now
    // fails `fireCanBeAt`, and each removal notifies its neighbours.
    t.w().setBlockWithNotify(0, 40, 0, block::kAir);

    // Run the world on for long enough that anything deferred has been picked
    // up. The assertion is that this returns at all -- before the budget it
    // recursed as deep as the field is wide -- and that the field is gone.
    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 200; ++i) {
        t.w().tick(&centre, 1, 2);
    }

    // Nothing is left owed: a deferred notification runs before the tick that
    // deferred it ends, so the queue is empty however deep the cascade went.
    CHECK_EQ(t.w().stats().notifyDropped, i64(0));
}

TEST(a_deep_notify_cascade_is_deferred_rather_than_recursed)
{
    // A column of sand is the simplest thing that cascades: each falling block
    // notifies the one above it, which then falls too.
    TestWorld t;
    t.floorOf(mcver::Block::Dirt, 10);

    for (int y = 12; y < 120; ++y) {
        t.set(0, y, 0, mcver::Block::Sand);
    }

    t.w().setBlockWithNotify(0, 11, 0, block::kAir);

    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 400; ++i) {
        t.w().tick(&centre, 1, 2);
    }

    // Whatever the cascade did, it lost nothing to a full queue.
    CHECK_EQ(t.w().stats().notifyDropped, i64(0));
}

TEST(a_redstone_net_cannot_reset_the_cascade_budget_by_notifying)
{
    // The bug the shared budget replaces: `wireNeighbourChanged` restarted
    // redstone's own depth counter at zero on every hop through
    // `notifyNeighbours`, so a net that alternated propagation and notification
    // was bounded by nothing at all.
    TestWorld t;
    t.floorOf(mcver::Block::Stone, 40);

    // A long run of wire, then power at one end.
    for (i32 x = -30; x <= 30; ++x) {
        t.set(x, 41, 0, mcver::Block::RedstoneWire);
    }
    t.set(0, 41, 2, mcver::Block::RedstoneTorch);

    t.w().notifyNeighbours(0, 41, 2, bid(mcver::Block::RedstoneTorch));

    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 100; ++i) {
        t.w().tick(&centre, 1, 2);
    }

    // The net settled and nothing was thrown away. A wire refusal here would
    // mean the budget is too small for an ordinary circuit rather than only for
    // a pathological one.
    CHECK_EQ(t.w().stats().notifyDropped, i64(0));
    CHECK_EQ(t.w().stats().wireRefused, i64(0));
}



// ---------------------------------------------------------------------------
// canPlaceBlockAt: the rules the placement path was not asking
// ---------------------------------------------------------------------------
//
// **Every predicate below was already in behaviour.cpp**, transcribed when the
// tick system landed, because a neighbour change has to ask the same question:
// that is what makes a flower pop when you mine the dirt under it. What was
// missing was anyone asking it when a block is *placed* -- the edit path tested
// "is the cell air" and stopped, so a cactus went on glass and a sapling into
// mid-air, and the tick then deleted them a moment later. That reads as the
// game losing your block rather than as a rule.
//
// These live beside the tick's own tests on purpose: the two must agree, and
// the last case here is that agreement stated as a property.

namespace {

constexpr int kPlaceY = 70;

// Ground under (0, kPlaceY, 0) and nothing else placed.
TestWorld benchWith(mcver::Block ground)
{
    TestWorld t;
    t.floorOf(ground, kPlaceY - 1);
    return t;
}

bool canPlace(TestWorld& t, mcver::Block block)
{
    return tick::canPlaceAt(t.w(), bid(block), 0, kPlaceY, 0);
}

}  // namespace

TEST(a_sapling_needs_ground_under_it)
{
    TestWorld dirt = benchWith(mcver::Block::Dirt);
    CHECK(canPlace(dirt, mcver::Block::Sapling));
    TestWorld grass = benchWith(mcver::Block::Grass);
    CHECK(canPlace(grass, mcver::Block::Sapling));

    // The complaint: a sapling would go on anything at all.
    TestWorld stone = benchWith(mcver::Block::Stone);
    CHECK(!canPlace(stone, mcver::Block::Sapling));
    TestWorld glass = benchWith(mcver::Block::Glass);
    CHECK(!canPlace(glass, mcver::Block::Sapling));
    TestWorld nothing = benchWith(mcver::Block::Air);
    CHECK(!canPlace(nothing, mcver::Block::Sapling));
}

TEST(wheat_needs_farmland_and_nothing_else)
{
    TestWorld farm = benchWith(mcver::Block::Farmland);
    CHECK(canPlace(farm, mcver::Block::Wheat));
    TestWorld dirt = benchWith(mcver::Block::Dirt);
    CHECK(!canPlace(dirt, mcver::Block::Wheat));
    TestWorld stone = benchWith(mcver::Block::Stone);
    CHECK(!canPlace(stone, mcver::Block::Wheat));
}

TEST(a_cactus_needs_sand_and_clear_sides)
{
    TestWorld sand = benchWith(mcver::Block::Sand);
    CHECK(canPlace(sand, mcver::Block::Cactus));
    TestWorld dirt = benchWith(mcver::Block::Dirt);
    CHECK(!canPlace(dirt, mcver::Block::Cactus));

    // Sand underneath is not enough: `BlockCactus.canBlockStay` refuses to
    // touch anything solid, which is why cactus grows in the open.
    TestWorld crowded = benchWith(mcver::Block::Sand);
    crowded.set(1, kPlaceY, 0, mcver::Block::Stone);
    CHECK(!canPlace(crowded, mcver::Block::Cactus));
}

TEST(a_torch_needs_a_face_and_not_a_ceiling)
{
    TestWorld floor = benchWith(mcver::Block::Stone);
    CHECK(canPlace(floor, mcver::Block::Torch));

    TestWorld floating = benchWith(mcver::Block::Air);
    CHECK(!canPlace(floating, mcver::Block::Torch));

    // A wall will do...
    TestWorld wall = benchWith(mcver::Block::Air);
    wall.set(1, kPlaceY, 0, mcver::Block::Stone);
    CHECK(canPlace(wall, mcver::Block::Torch));

    // ...but a ceiling will not, which is the one face BlockTorch leaves out.
    TestWorld ceiling = benchWith(mcver::Block::Air);
    ceiling.set(0, kPlaceY + 1, 0, mcver::Block::Stone);
    CHECK(!canPlace(ceiling, mcver::Block::Torch));
}

TEST(a_ladder_wants_a_wall_where_a_rail_wants_a_floor)
{
    TestWorld floor = benchWith(mcver::Block::Stone);
    CHECK(!canPlace(floor, mcver::Block::Ladder));
    CHECK(canPlace(floor, mcver::Block::Rail));

    TestWorld wall = benchWith(mcver::Block::Air);
    wall.set(0, kPlaceY, 1, mcver::Block::Stone);
    CHECK(canPlace(wall, mcver::Block::Ladder));
    CHECK(!canPlace(wall, mcver::Block::Rail));
}

TEST(a_door_needs_a_floor_and_headroom)
{
    TestWorld room = benchWith(mcver::Block::Stone);
    CHECK(canPlace(room, mcver::Block::WoodenDoor));

    // The upper half has to fit, which is why a door will not go under a
    // ceiling one block up.
    TestWorld squashed = benchWith(mcver::Block::Stone);
    squashed.set(0, kPlaceY + 1, 0, mcver::Block::Stone);
    CHECK(!canPlace(squashed, mcver::Block::WoodenDoor));
}

TEST(an_ordinary_block_goes_anywhere_free_including_into_water)
{
    TestWorld air = benchWith(mcver::Block::Air);
    CHECK(canPlace(air, mcver::Block::Stone));

    // **A liquid is free space**, which the base `canPlaceBlockAt` says and the
    // old air-only test denied. Building into a pond works in the original.
    TestWorld pond = benchWith(mcver::Block::Stone);
    pond.set(0, kPlaceY, 0, mcver::Block::Water);
    CHECK(canPlace(pond, mcver::Block::Stone));

    // An occupied cell is still occupied.
    TestWorld full = benchWith(mcver::Block::Stone);
    full.set(0, kPlaceY, 0, mcver::Block::Stone);
    CHECK(!canPlace(full, mcver::Block::Stone));
}

TEST(what_placement_allows_the_next_tick_does_not_delete)
{
    // **The property, and the reason this belongs in this file.** A block the
    // placement path accepts must not be one the very next neighbour
    // notification removes; if the two predicates ever disagree, the game
    // swallows blocks. Checked over every ground the version defines.
    for (int ground = 0; ground < mcver::kBlockTableSize; ++ground) {
        if (ground != 0 && !mcver::kBlocks[ground].known) {
            continue;
        }
        for (mcver::Block plant : {mcver::Block::Sapling, mcver::Block::Wheat,
                                   mcver::Block::Cactus, mcver::Block::Torch}) {
            // A single ground block on a stone plane rather than a plane of the
            // ground itself. **A plane of cactus is not a legal world** -- each
            // one has solid cactus beside it and deletes itself -- and the
            // property being tested is about placement, not about whether an
            // impossible scene stays put.
            TestWorld t;
            t.floorOf(mcver::Block::Stone, kPlaceY - 2);
            t.w().setBlockWithNotify(0, kPlaceY - 1, 0, bid(mcver::Block(ground)));
            // **Poked, because a stay-check only runs on a neighbour change.**
            // A cactus dropped onto stone survives being written and dies the
            // moment anything next to it moves; without this the loop would set
            // up an illegal ground, place a legal cactus on it, and watch the
            // whole column collapse -- which says nothing about placement.
            tick::neighbourChanged(t.w(), 0, kPlaceY - 1, 0, block::kAir);
            if (t.get(0, kPlaceY - 1, 0) != bid(mcver::Block(ground))) {
                // The ground could not stay there either, so there is nothing
                // to stand a plant on and nothing to conclude.
                continue;
            }
            if (!tick::canPlaceAt(t.w(), bid(plant), 0, kPlaceY, 0)) {
                continue;
            }
            t.w().setBlockWithNotify(0, kPlaceY, 0, bid(plant));
            CHECK_EQ(int(t.get(0, kPlaceY, 0)), int(plant));
        }
    }
}
