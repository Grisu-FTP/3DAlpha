// Survival's break loop -- `nj`'s click, damage and reset -- over a scene.
//
// The tick counts here are the controller's own arithmetic run forward in
// float, not a number from a running game; the relative hardness each tick adds
// is checked separately in tests/tool_rules_test.cpp. What these pin is the
// shape around it: the tick spent recording a new cell, the five-tick pause
// after a break, harvest decided after the wear, and what a click does that
// a held button does not.

#include "core/block/registry.hpp"
#include "core/item/block_breaking.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/item/tool_rules.hpp"
#include "core/item/use.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

using namespace mc;
using mc::item::BlockBreaker;
using mc::item::BreakContext;
using mc::test::DropCatcher;
using mc::test::SceneWorld;
using mcver::Block;
using mcver::Item;

namespace {

constexpr i32 kX = 8;
constexpr int kY = 80;
constexpr i32 kZ = 8;

struct Fixture {
    DropCatcher catcher;  // outlives the world it watches
    SceneWorld scene{0, 0};
    item::Inventory inventory;
    item::Effects effects;
    BlockBreaker breaker;

    Fixture() { catcher.watch(scene.w()); }

    BreakContext context() { return BreakContext{scene.w(), inventory, effects}; }

    void put(Block id, i32 x = kX, int y = kY, i32 z = kZ)
    {
        scene.place(x, y, z, block::BlockId(id), 0);
    }

    void hold(Item id, i16 damage = 0)
    {
        inventory.set(0, item::ItemId(id), 1);
        inventory.main[0].damage = damage;
        inventory.selected = 0;
    }

    // One tick of the button held on (x, y, z): `nj.c()` then `nj.c(IIII)`.
    bool tick(i32 x = kX, int y = kY, i32 z = kZ)
    {
        BreakContext ctx = context();
        breaker.update();
        return breaker.damage(ctx, x, y, z, 1);
    }

    // Ticks until the block breaks, or -1 after `limit`.
    int ticksToBreak(int limit = 1000)
    {
        for (int t = 1; t <= limit; ++t) {
            if (tick()) {
                return t;
            }
        }
        return -1;
    }
};

// How many ticks `nj.c` needs on a fresh cell at `perTick` a tick: one to
// record the cell, then accumulations in float until the sum reaches 1.
int expectedTicks(float perTick)
{
    float progress = 0.0f;
    int ticks = 1;
    while (progress < 1.0f) {
        progress += perTick;
        ++ticks;
    }
    return ticks;
}

block::BlockId blockAt(Fixture& f) { return f.scene.w().blockAt(kX, kY, kZ); }

}  // namespace

TEST(the_first_tick_on_a_block_only_remembers_which_block_it_is)
{
    Fixture f;
    f.put(Block::Dirt);
    f.tick();
    CHECK_EQ(double(f.breaker.progress()), 0.0);
    f.tick();
    CHECK_EQ(double(f.breaker.progress()),
             double(item::relativeHardness(0, block::BlockId(Block::Dirt), false, true)));
}

TEST(dirt_by_hand_takes_its_hardness_in_ticks_and_drops_dirt)
{
    Fixture f;
    f.put(Block::Dirt);
    const float perTick = item::relativeHardness(0, block::BlockId(Block::Dirt), false, true);
    CHECK_EQ(f.ticksToBreak(), expectedTicks(perTick));
    CHECK_EQ(int(blockAt(f)), int(block::kAir));
    CHECK_EQ(f.catcher.countOf(u16(Block::Dirt)), 1);
}

TEST(stone_by_hand_comes_out_and_leaves_nothing)
{
    Fixture f;
    f.put(Block::Stone);
    const float perTick = item::relativeHardness(0, block::BlockId(Block::Stone), false, true);
    CHECK_EQ(f.ticksToBreak(), expectedTicks(perTick));
    CHECK_EQ(int(blockAt(f)), int(block::kAir));
    CHECK_EQ(f.catcher.total(), 0);
}

TEST(a_wooden_pickaxe_breaks_stone_into_cobblestone_and_wears_by_one)
{
    Fixture f;
    f.put(Block::Stone);
    f.hold(Item::WoodenPickaxe);
    const float perTick = item::relativeHardness(item::ItemId(Item::WoodenPickaxe),
                                                 block::BlockId(Block::Stone), false, true);
    CHECK_EQ(f.ticksToBreak(), expectedTicks(perTick));
    CHECK_EQ(f.catcher.countOf(u16(Block::Cobblestone)), 1);
    CHECK_EQ(int(f.inventory.main[0].damage), 1);
}

TEST(a_pickaxe_used_up_on_the_block_is_gone_and_the_block_drops_nothing)
{
    Fixture f;
    f.put(Block::Stone);
    const int durability = item::def(item::ItemId(Item::WoodenPickaxe)).durability;
    f.hold(Item::WoodenPickaxe, i16(durability));
    CHECK(f.ticksToBreak() > 0);
    CHECK(f.inventory.main[0].empty());
    CHECK_EQ(f.catcher.total(), 0);
}

TEST(five_ticks_pass_after_a_break_before_the_next_block_takes_any)
{
    Fixture f;
    f.put(Block::Dirt);
    f.put(Block::Dirt, kX + 1);
    CHECK(f.ticksToBreak() > 0);
    // Five ticks of delay, then the tick that records the new cell.
    for (int i = 0; i < 6; ++i) {
        f.tick(kX + 1);
        CHECK_EQ(double(f.breaker.progress()), 0.0);
    }
    f.tick(kX + 1);
    CHECK(f.breaker.progress() > 0.0f);
}

TEST(moving_to_another_block_starts_the_break_over)
{
    Fixture f;
    f.put(Block::Stone);
    f.put(Block::Stone, kX + 1);
    for (int i = 0; i < 20; ++i) {
        f.tick();
    }
    CHECK(f.breaker.progress() > 0.0f);
    f.tick(kX + 1);
    CHECK_EQ(double(f.breaker.progress()), 0.0);
}

TEST(letting_go_resets_the_progress)
{
    Fixture f;
    f.put(Block::Stone);
    for (int i = 0; i < 20; ++i) {
        f.tick();
    }
    CHECK(f.breaker.progress() > 0.0f);
    f.breaker.reset();
    CHECK_EQ(double(f.breaker.progress()), 0.0);
}

TEST(one_click_breaks_a_torch)
{
    Fixture f;
    f.put(Block::Stone, kX, kY - 1);
    f.put(Block::Torch);
    BreakContext ctx = f.context();
    CHECK(f.breaker.click(ctx, kX, kY, kZ, 1));
    CHECK_EQ(int(blockAt(f)), int(block::kAir));
    CHECK_EQ(f.catcher.countOf(u16(Block::Torch)), 1);
}

TEST(a_click_on_stone_breaks_nothing)
{
    Fixture f;
    f.put(Block::Stone);
    BreakContext ctx = f.context();
    CHECK(!f.breaker.click(ctx, kX, kY, kZ, 1));
    CHECK_EQ(int(blockAt(f)), int(Block::Stone));
}

TEST(a_left_click_puts_out_fire_on_the_struck_face)
{
    Fixture f;
    f.put(Block::Stone);
    f.put(Block::Fire, kX, kY + 1);
    CHECK(item::extinguishFireOnFace(f.scene.w(), kX, kY, kZ, 1));
    CHECK_EQ(int(f.scene.w().blockAt(kX, kY + 1, kZ)), int(block::kAir));
    // Nothing on the other faces.
    CHECK(!item::extinguishFireOnFace(f.scene.w(), kX, kY, kZ, 0));
}

TEST(the_crack_interpolates_between_the_last_two_ticks)
{
    Fixture f;
    f.put(Block::Dirt);
    CHECK_EQ(f.breaker.crackStage(1.0f), -1);
    f.tick();  // records the cell
    for (int i = 0; i < 5; ++i) {
        f.tick();
    }
    const float perTick = item::relativeHardness(0, block::BlockId(Block::Dirt), false, true);
    float now = 0.0f;
    for (int i = 0; i < 5; ++i) {
        now += perTick;
    }
    const float before = now - perTick;
    CHECK_EQ(f.breaker.crackStage(1.0f), int(now * 10.0f));
    CHECK_EQ(f.breaker.crackStage(0.0f), int(before * 10.0f));
}

// ---- somebody else's click -------------------------------------------------
//
// `in.c(III)Z` in `srv/a0.2.1.jar` -- the host running a guest's finished dig.
// The tool it asks about is the digger's, which is the whole point: the guest
// has no stack sink of its own, so a break that drops nothing here drops
// nothing anywhere.

TEST(a_guests_break_drops_what_the_guests_tool_earns)
{
    Fixture f;
    f.put(Block::Stone);
    CHECK(item::harvestBlockFor(f.scene.w(), kX, kY, kZ,
                                item::ItemId(Item::StonePickaxe), f.effects));
    CHECK_EQ(int(blockAt(f)), int(block::kAir));
    CHECK_EQ(f.catcher.countOf(u16(Block::Cobblestone)), 1);
}

TEST(a_guests_bare_handed_break_leaves_nothing_behind)
{
    Fixture f;
    f.put(Block::Stone);
    CHECK(item::harvestBlockFor(f.scene.w(), kX, kY, kZ, 0, f.effects));
    CHECK_EQ(int(blockAt(f)), int(block::kAir));
    CHECK_EQ(f.catcher.total(), 0);
}

TEST(a_guests_break_on_air_is_not_a_break)
{
    Fixture f;
    CHECK(!item::harvestBlockFor(f.scene.w(), kX, kY, kZ,
                                 item::ItemId(Item::StonePickaxe), f.effects));
    CHECK_EQ(f.catcher.total(), 0);
}
