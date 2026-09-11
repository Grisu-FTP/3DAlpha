// What a block leaves behind, and the block that is on its way down.
//
// Two things that had names and no bodies until now. `dropBlockAsItem` was a
// stub with nine call sites, so knocking the support out from under a torch
// deleted the torch; `fallingTick` moved sand to its resting place in one tick,
// so a collapsing pile blinked rather than fell.
//
// The drop table is generated (`tools/genref.java --drops`) and this is where
// the *rules* over it are checked: the arithmetic that replays
// `quantityDropped`, the one block that rolls for which item it leaves, the two
// that answer differently per metadata, and the nine behaviours that call it.

#include "core/block/registry.hpp"
#include "core/entity/falling_block.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "low_heap.hpp"
#include "scene_world.hpp"

#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A floor of stone at y = 63, with a catcher wired to the world.
struct Ground {
    SceneWorld scene{0, 0};
    mc::test::DropCatcher caught;

    Ground()
    {
        for (i32 x = -6; x <= 6; ++x) {
            for (i32 z = -6; z <= 6; ++z) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
        caught.watch(scene.w());
    }

    tick::TickWorld& w() { return scene.w(); }
    int id(i32 x, int y, i32 z) { return int(scene.w().blockAt(x, y, z)); }
};

}  // namespace

// ---------------------------------------------------------------------------
// The table and the arithmetic over it
// ---------------------------------------------------------------------------

TEST(a_plain_block_drops_one_of_itself)
{
    Ground g;
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Planks), 0);
    CHECK_EQ(int(g.caught.drops.size()), 1);
    CHECK_EQ(int(g.caught.drops[0].item), int(mcver::Block::Planks));
    CHECK_EQ(g.caught.drops[0].count, 1);
}

TEST(stone_drops_cobblestone_and_grass_drops_dirt)
{
    // Two of the twenty-four `idDropped` overrides, and the two a player meets
    // first. Read out of the generated table rather than written here, so this
    // is a check on the table having been generated at all.
    Ground g;
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Stone), 0);
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Grass), 0);
    CHECK_EQ(int(g.caught.drops.size()), 2);
    CHECK_EQ(int(g.caught.drops[0].item), int(mcver::Block::Cobblestone));
    CHECK_EQ(int(g.caught.drops[1].item), int(mcver::Block::Dirt));
}

TEST(glass_and_ice_leave_nothing)
{
    // `quantityDropped` returns zero for both, so the loop never runs and no
    // `idDropped` is asked for at all.
    Ground g;
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Glass), 0);
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Ice), 0);
    CHECK_EQ(int(g.caught.drops.size()), 0);
}

TEST(gravel_sometimes_leaves_flint_instead)
{
    // The one block in a1.1.2 whose `idDropped` rolls: `nextInt(10) == 0` picks
    // flint. Over enough draws both answers have to appear, and nothing else
    // may.
    Ground g;
    for (int i = 0; i < 400; ++i) {
        tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Gravel), 0);
    }
    int gravel = 0;
    int flint = 0;
    for (const mc::test::Drop& d : g.caught.drops) {
        if (int(d.item) == int(mcver::Block::Gravel)) ++gravel;
        else ++flint;
    }
    CHECK_EQ(int(g.caught.drops.size()), 400);
    CHECK(flint > 0);
    CHECK(gravel > 0);
    // Roughly one in ten, with room for the tail: this is a real random stream
    // and a bound that is too tight is a test that fails on a Tuesday.
    CHECK(flint > 400 / 40);
    CHECK(flint < 400 / 3);
}

TEST(leaves_leave_a_sapling_about_one_time_in_twenty)
{
    // The other shape `quantityDropped` takes: `nextInt(20) == 0 ? 1 : 0`,
    // which is not a range and would come out as 0..1 uniform if it were
    // modelled as one.
    Ground g;
    for (int i = 0; i < 400; ++i) {
        tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::Leaves), 0);
    }
    CHECK(g.caught.total() > 0);
    CHECK(g.caught.total() < 400 / 5);
    for (const mc::test::Drop& d : g.caught.drops) {
        CHECK_EQ(int(d.item), int(mcver::Block::Sapling));
    }
}

TEST(redstone_ore_drops_four_or_five)
{
    // `4 + nextInt(2)`, the third shape. Both values have to show up, and one
    // world is used for all of them: a fresh `SceneWorld` is seeded the same
    // way every time, so sixty of them would draw the same number sixty times.
    Ground g;
    bool sawFour = false;
    bool sawFive = false;
    for (int i = 0; i < 60; ++i) {
        g.caught.drops.clear();
        tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::RedstoneOre), 0);
        const int n = int(g.caught.drops.size());
        CHECK(n == 4 || n == 5);
        sawFour = sawFour || n == 4;
        sawFive = sawFive || n == 5;
    }
    CHECK(sawFour);
    CHECK(sawFive);
}

TEST(only_the_lower_half_of_a_door_drops_the_door)
{
    // The generated per-metadata row: bit 3 is "upper half" and answers 0,
    // which is what makes a door come back as one item however it is knocked
    // down.
    Ground g;
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::WoodenDoor), 0);
    CHECK_EQ(int(g.caught.drops.size()), 1);
    const int lower = int(g.caught.drops[0].item);
    CHECK(lower >= 256);   // the door *item*, not the door block

    g.caught.drops.clear();
    tick::dropBlockAsItem(g.w(), 0, 64, 0, bid(mcver::Block::WoodenDoor), 8);
    CHECK_EQ(int(g.caught.drops.size()), 0);
}

TEST(a_drop_lands_inside_the_cell_it_came_from)
{
    // `nextFloat() * 0.7 + 0.15` on each axis, so every drop is inside the
    // middle 70 % of the block and never on its face.
    Ground g;
    for (int i = 0; i < 100; ++i) {
        tick::dropBlockAsItem(g.w(), 3, 64, -2, bid(mcver::Block::Planks), 0);
    }
    for (const mc::test::Drop& d : g.caught.drops) {
        CHECK(d.x > 3.14 && d.x < 3.86);
        CHECK(d.y > 64.14 && d.y < 64.86);
        CHECK(d.z > -1.86 && d.z < -1.14);
    }
}

TEST(a_world_with_no_sink_still_takes_the_same_random_path)
{
    // The property the sink is *for*: a headless tool has no pool, and a world
    // that dropped nothing must not therefore generate different flowers. Two
    // worlds, the same seed, one with a sink and one without -- their randoms
    // have to agree afterwards.
    Ground withSink;
    Ground without;
    without.w().setDropSink(nullptr, nullptr);

    for (int i = 0; i < 50; ++i) {
        tick::dropBlockAsItem(withSink.w(), 0, 64, 0, bid(mcver::Block::Gravel), 0);
        tick::dropBlockAsItem(without.w(), 0, 64, 0, bid(mcver::Block::Gravel), 0);
    }
    CHECK(withSink.caught.drops.size() > 0);
    CHECK_EQ(int(without.caught.drops.size()), 0);
    CHECK_EQ((long long) withSink.w().random().nextInt(),
             (long long) without.w().random().nextInt());
}

// ---------------------------------------------------------------------------
// The behaviours that call it
// ---------------------------------------------------------------------------

TEST(a_torch_whose_wall_goes_leaves_a_torch_on_the_floor)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    // Metadata 1 is "on the -x side of the block at +x", which is the wall at
    // (0, 64, 0) seen from (-1, 64, 0).
    g.w().setBlockAndDataWithNotify(-1, 64, 0, bid(mcver::Block::Torch), 2);
    g.caught.drops.clear();

    g.w().setBlockWithNotify(0, 64, 0, block::kAir);
    CHECK_EQ(g.id(-1, 64, 0), int(mcver::Block::Air));
    CHECK_EQ(int(g.caught.drops.size()), 1);
    CHECK_EQ(int(g.caught.drops[0].item), int(mcver::Block::Torch));
}

TEST(a_ladder_whose_wall_goes_leaves_a_ladder)
{
    // **This behaviour did not exist**: `Ladder` had no `neighbourChanged`
    // entry at all, so a ladder whose wall was knocked away stayed hanging in
    // the air for ever.
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    // Metadata 4 hangs on the +x wall.
    g.w().setBlockAndDataWithNotify(-1, 64, 0, bid(mcver::Block::Ladder), 4);
    g.caught.drops.clear();

    g.w().setBlockWithNotify(0, 64, 0, block::kAir);
    CHECK_EQ(g.id(-1, 64, 0), int(mcver::Block::Air));
    CHECK_EQ(int(g.caught.drops.size()), 1);
    CHECK_EQ(int(g.caught.drops[0].item), int(mcver::Block::Ladder));
}

TEST(a_ladder_keeps_hanging_while_its_own_wall_is_there)
{
    // The other half: a ladder on the +x wall does not care what happens on the
    // -x side, which is the difference between reading the metadata and reading
    // "is there any wall at all".
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    g.scene.place(-2, 64, 0, bid(mcver::Block::Stone), 0);
    g.w().setBlockAndDataWithNotify(-1, 64, 0, bid(mcver::Block::Ladder), 4);

    g.w().setBlockWithNotify(-2, 64, 0, block::kAir);
    CHECK_EQ(g.id(-1, 64, 0), int(mcver::Block::Ladder));
}

TEST(a_lever_whose_wall_goes_leaves_a_lever)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    g.w().setBlockAndDataWithNotify(-1, 64, 0, bid(mcver::Block::Lever), 2);
    g.caught.drops.clear();

    g.w().setBlockWithNotify(0, 64, 0, block::kAir);
    CHECK_EQ(g.id(-1, 64, 0), int(mcver::Block::Air));
    CHECK_EQ(int(g.caught.drops.size()), 1);
    CHECK_EQ(int(g.caught.drops[0].item), int(mcver::Block::Lever));
}

TEST(redstone_wire_whose_floor_goes_leaves_redstone)
{
    Ground g;
    g.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::RedstoneWire), 0);
    g.caught.drops.clear();

    g.w().setBlockWithNotify(0, 63, 0, block::kAir);
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Air));
    CHECK_EQ(int(g.caught.drops.size()), 1);
    // The wire drops the *item*, which is 331 and not block 55.
    CHECK(int(g.caught.drops[0].item) >= 256);
}

TEST(a_sponge_wakes_its_neighbours_when_it_goes_and_absorbs_nothing_ever)
{
    // **a1.1.2's sponge does not remove water.** `ng.e` -- onBlockAdded --
    // walks a 5 x 5 x 5 box comparing each cell's material against water and
    // the body of that comparison is *empty*: the branch target is the next
    // instruction. Absorption is Classic's and then 1.8's; in between, a sponge
    // is a decorative block. The only thing it does is the removal below.
    Ground g;
    g.w().setBlockAndDataWithNotify(0, 65, 0, bid(mcver::Block::Water), 0);
    g.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Sponge), 0);
    // Still water beside a change becomes flowing water -- `hn.a` turns the
    // stationary block back into the moving one -- so what is checked is that
    // the cell still holds *water*, which is the whole claim.
    CHECK(g.id(0, 65, 0) == int(mcver::Block::Water)
          || g.id(0, 65, 0) == int(mcver::Block::FlowingWater));

    // And it still wakes what is around it on the way out, which is what makes
    // the water above it start flowing again.
    g.w().setBlockWithNotify(0, 64, 0, block::kAir);
    CHECK(!g.w().scheduler().empty());
}

// ---------------------------------------------------------------------------
// The falling block
// ---------------------------------------------------------------------------

namespace {

struct FallScene : Ground {
    entity::FallingBlockSystem falling;

    static bool sink(void* ctx, i32 x, int y, i32 z, u16 block)
    {
        auto* self = static_cast<FallScene*>(ctx);
        return self->falling.spawn(self->w(), x, y, z, BlockId(block));
    }

    void watch() { w().setFallingBlockSink(&FallScene::sink, this); }

    void run(int ticks)
    {
        // One 20 Hz step, then the entity pool. The centre is the pile's own
        // column, so the scheduled sand updates are actually reached.
        const tick::TickWorld::Centre centre{0, 0};
        for (int i = 0; i < ticks; ++i) {
            w().tick(&centre, 1, 1);
            falling.tick(w());
        }
    }
};

}  // namespace

TEST(sand_with_no_sink_lands_in_one_tick)
{
    // `BlockSand.fallInstantly`, which is what world generation runs with. The
    // block ends up in the right cell; only the animation is missing.
    //
    // **The fall is scheduled, not immediate**: `dh.a(Lcn;IIII)V` asks to be
    // ticked at its own rate of 3 rather than falling on the spot, which is
    // what gives a collapsing pile its stagger. So a few ticks pass before
    // anything happens either way.
    FallScene g;
    g.w().setBlockWithNotify(0, 70, 0, bid(mcver::Block::Sand));
    CHECK_EQ(g.id(0, 70, 0), int(mcver::Block::Sand));

    g.run(8);
    CHECK_EQ(g.id(0, 70, 0), int(mcver::Block::Air));
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Sand));
    CHECK_EQ(g.falling.count(), 0);
}

TEST(sand_with_a_sink_falls_as_an_entity_and_lands_where_the_instant_path_put_it)
{
    // The same drop, watched. It takes several ticks now, the cell it came from
    // is empty for all of them, and it ends in the same place.
    FallScene g;
    g.watch();
    g.w().setBlockWithNotify(0, 70, 0, bid(mcver::Block::Sand));
    g.run(5);

    CHECK_EQ(g.falling.count(), 1);
    CHECK_EQ(g.id(0, 70, 0), int(mcver::Block::Air));
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Air));

    g.run(120);
    CHECK_EQ(g.falling.count(), 0);
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Sand));
}

TEST(a_falling_block_takes_more_than_one_tick_to_cross_six_blocks)
{
    // The whole point of the entity: it is not a teleport. Six blocks under
    // `motionY -= 0.04` and a 0.98 drag is well over a dozen ticks.
    FallScene g;
    g.watch();
    g.w().setBlockWithNotify(0, 70, 0, bid(mcver::Block::Sand));

    // Wait for the scheduled update to spawn it, then count the ticks it is in
    // the air. The first loop is bounded so a spawn that never happens is a
    // failure rather than a hang.
    int waited = 0;
    while (g.falling.count() == 0 && waited < 20) {
        g.run(1);
        ++waited;
    }
    CHECK_EQ(g.falling.count(), 1);

    int airborne = 0;
    while (g.falling.count() > 0 && airborne < 200) {
        g.run(1);
        ++airborne;
    }
    CHECK(airborne > 10);
    CHECK(airborne < 200);
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Sand));
}

TEST(a_column_of_sand_collapses_from_the_bottom_up)
{
    // Three cells, all of which end up stacked on the floor. The entity clears
    // its own source cell from inside its tick, so the column empties one cell
    // at a time rather than all at once -- but the resting state is the same
    // pile either way.
    FallScene g;
    g.watch();
    for (int y = 70; y <= 72; ++y) {
        g.scene.place(0, y, 0, bid(mcver::Block::Sand), 0);
    }
    g.w().setBlockWithNotify(0, 70, 0, bid(mcver::Block::Sand));
    g.run(400);

    CHECK_EQ(g.falling.count(), 0);
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Sand));
    CHECK_EQ(g.id(0, 65, 0), int(mcver::Block::Sand));
    CHECK_EQ(g.id(0, 66, 0), int(mcver::Block::Sand));
    CHECK_EQ(g.id(0, 70, 0), int(mcver::Block::Air));
}

TEST(more_blocks_fall_at_once_than_the_pool_first_holds)
{
    // No cap: a collapsing field of sand animates every cell, where the fixed
    // pool of sixteen sent the seventeenth down the instant path.
    FallScene g;
    const int many = entity::FallingBlockSystem::kInitialCapacity * 3;
    for (int i = 0; i < many; ++i) {
        CHECK(g.falling.spawn(g.w(), i32(i % 8), 100, i32(i / 8), bid(mcver::Block::Sand)));
    }
    CHECK_EQ(g.falling.count(), many);
    CHECK_EQ(int(g.falling.refused()), 0);
}

TEST(a_full_heap_falls_back_to_the_instant_path_rather_than_losing_the_block)
{
    // Where a refused falling block degrades instead of dropping something:
    // the spawn is refused -- now only because the heap will not hold another
    // -- `fallingTick` takes the other branch, and the block still ends up on
    // the floor.
    FallScene g;
    g.watch();
    for (int i = 0; i < entity::FallingBlockSystem::kInitialCapacity; ++i) {
        CHECK(g.falling.spawn(g.w(), 0, 100, 0, bid(mcver::Block::Sand)));
    }
    CHECK_EQ(g.falling.count(), entity::FallingBlockSystem::kInitialCapacity);

    test::LowHeap low;
    g.w().setBlockWithNotify(3, 70, 3, bid(mcver::Block::Sand));
    g.run(8);
    CHECK_EQ(g.id(3, 70, 3), int(mcver::Block::Air));
    CHECK_EQ(g.id(3, 64, 3), int(mcver::Block::Sand));
    CHECK(g.falling.refused() > 0);
}

// ---------------------------------------------------------------------------
// `onBlockDestroyedByPlayer`, which is not the drop table
// ---------------------------------------------------------------------------

TEST(breaking_a_crop_by_hand_scatters_seeds)
{
    // `hd.b(Lcn;IIII)V` -- three rolls of `nextInt(15) <= metadata`, each one
    // worth a seed. It is the *player's* break that runs this, not the drop
    // table and not the neighbour path, which is why an unripe crop left
    // nothing at all before it existed: `idDropped` answers only at stage 7.
    int withSeeds = 0;
    for (int trial = 0; trial < 40; ++trial) {
        Ground g;
        g.scene.place(0, 63, 0, bid(mcver::Block::Farmland), 0);
        g.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Wheat), 4);
        g.caught.drops.clear();

        CHECK(item::destroyBlock(g.w(), 0, 64, 0));
        const int seeds = g.caught.countOf(u16(mcver::Item::Seeds));
        CHECK(seeds >= 0 && seeds <= 3);
        if (seeds > 0) {
            ++withSeeds;
        }
    }
    // At metadata 4 the roll passes five times in fifteen, so forty breaks
    // leaving *no* seeds at all would mean the rolls are not happening.
    CHECK(withSeeds > 0);
}

TEST(a_riper_crop_broken_by_hand_leaves_more_seeds)
{
    // Each of the three rolls passes (stage + 1) times in fifteen, so stage 7
    // is the most generous but still fails about half its rolls. What is
    // checked is the ceiling of three and that a ripe crop out-seeds a young
    // one over many breaks.
    Ground ripe;
    ripe.scene.place(0, 63, 0, bid(mcver::Block::Farmland), 0);
    ripe.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Wheat), 7);
    ripe.caught.drops.clear();
    CHECK(item::destroyBlock(ripe.w(), 0, 64, 0));
    CHECK(ripe.caught.countOf(u16(mcver::Item::Seeds)) <= 3);

    // Stage 0 passes the roll one time in fifteen. Over enough breaks it has to
    // leave strictly fewer seeds than stage 7 does.
    int young = 0;
    int old = 0;
    for (int trial = 0; trial < 60; ++trial) {
        Ground a;
        a.scene.place(0, 63, 0, bid(mcver::Block::Farmland), 0);
        a.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Wheat), 0);
        a.caught.drops.clear();
        item::destroyBlock(a.w(), 0, 64, 0);
        young += a.caught.countOf(u16(mcver::Item::Seeds));

        Ground b;
        b.scene.place(0, 63, 0, bid(mcver::Block::Farmland), 0);
        b.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Wheat), 7);
        b.caught.drops.clear();
        item::destroyBlock(b.w(), 0, 64, 0);
        old += b.caught.countOf(u16(mcver::Item::Seeds));
    }
    CHECK(old > young);
}

TEST(a_crop_whose_farmland_goes_drops_by_the_table_and_not_by_the_hand)
{
    // The neighbour path is `mq.h`, which is `dropBlockAsItem` alone: wheat at
    // stage 7 and nothing at all below it. **No seeds** -- those are the
    // player's break and only the player's, and conflating the two would make
    // a crop worth farming by mining the dirt under it.
    Ground g;
    g.scene.place(0, 63, 0, bid(mcver::Block::Farmland), 0);
    g.w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Wheat), 7);
    g.caught.drops.clear();

    g.w().setBlockWithNotify(0, 63, 0, block::kAir);
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Air));
    CHECK_EQ(g.caught.countOf(u16(mcver::Item::Seeds)), 0);
    CHECK_EQ(g.caught.countOf(u16(mcver::Item::Wheat)), 1);
}

TEST(the_hook_is_a_draw_from_the_worlds_random_like_every_other_drop)
{
    // The property `dropBlockAsItem` already holds: a world with nowhere to put
    // an item must still take the same path through its random, or a headless
    // tool and a running game would generate different worlds.
    Ground withSink;
    Ground without;
    without.w().setDropSink(nullptr, nullptr);

    for (Ground* g : {&withSink, &without}) {
        g->scene.place(0, 63, 0, bid(mcver::Block::Farmland), 0);
        g->w().setBlockAndDataWithNotify(0, 64, 0, bid(mcver::Block::Wheat), 5);
        item::destroyBlock(g->w(), 0, 64, 0);
    }
    CHECK(withSink.caught.drops.size() > 0);
    CHECK_EQ(int(without.caught.drops.size()), 0);
    CHECK_EQ((long long) withSink.w().random().nextInt(),
             (long long) without.w().random().nextInt());
}
