// The dropped item: the throw, the fall, the wait, and the pickup.
//
// **There is no oracle for any of it**, in the sense the physics suite has one:
// `EntityItem`'s two random draws come off `Math.random()` and a time-seeded
// `Random`, so no sequence is reproducible in the *original* either. What is
// checkable is everything the constants decide -- that it lands, that it stops,
// that it cannot be picked up for two seconds, that it ages out at 6000 ticks,
// and that the inventory arithmetic underneath is
// `addItemStackToInventory`'s.

#include "core/entity/explosion.hpp"
#include "core/entity/fire_entry.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/mob.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "low_heap.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::entity::ItemEntitySystem;
using mc::item::Inventory;
using mc::item::ItemId;
using mc::test::SceneWorld;

namespace {

// A floor of stone at y = 63, so the top of it is y = 64.
struct Ground {
    SceneWorld world{0, 0};
    ItemEntitySystem items{1234};

    Ground()
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 8; ++z) {
                world.place(x, 63, z, block::BlockId(mcver::Block::Stone), 0);
            }
        }
    }

    void run(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            items.tick(world.w());
        }
    }
};

ItemId stoneItem() { return item::paletteItem(0); }

}  // namespace

TEST(a_dropped_item_falls_to_the_floor_and_stops_there)
{
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 70.0, 0.5, stoneItem(), 1, 0));
    CHECK_EQ(g.items.count(), 1);

    g.run(200);
    CHECK_EQ(g.items.count(), 1);

    // The box is a quarter of a block and its bottom rests on y = 64, so the
    // centre sits an eighth above the floor.
    const entity::ItemEntity& e = g.items[0];
    CHECK(e.onGround);
    CHECK_EQ(e.box.minY, 64.0);
    CHECK(e.y > 64.0);
    CHECK(e.y < 64.2);

    // And it has come to rest rather than still creeping.
    const double restX = e.x;
    const double restZ = e.z;
    g.run(40);
    CHECK(g.items[0].x - restX < 1e-6 && restX - g.items[0].x < 1e-6);
    CHECK(g.items[0].z - restZ < 1e-6 && restZ - g.items[0].z < 1e-6);
}

TEST(an_item_does_not_fall_through_the_world_it_lands_on)
{
    // The failure `Particle` documents at length -- a box rebuilt from a
    // rounded centre sits a fraction of an ulp below the block it landed on and
    // the next sweep declines to stop it. An item is a box for the same reason
    // and this is the test that would catch it going the other way.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 90.0, 0.5, stoneItem(), 1, 0));
    g.run(2000);
    CHECK_EQ(g.items.count(), 1);
    CHECK(g.items[0].y > 63.0);
}

TEST(an_item_ages_out_after_five_minutes)
{
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 66.0, 0.5, stoneItem(), 1, 0));
    g.run(entity::kItemMaxAge - 1);
    CHECK_EQ(g.items.count(), 1);
    g.run(1);
    CHECK_EQ(g.items.count(), 0);
}

TEST(a_players_own_drop_cannot_be_walked_back_into_for_two_seconds)
{
    Ground g;
    CHECK(g.items.dropFromPlayer(g.world.w(), 0.5, 66.0, 0.5, 0.0f, 0.0f, stoneItem(), 1, 0));
    CHECK_EQ(g.items[0].pickupDelay, entity::kItemDropPickupDelay);

    Inventory inventory;
    inventory.clear();

    // A box that swallows the whole area, so only the delay can be refusing.
    const AABB everywhere{-8.0, 60.0, -8.0, 8.0, 80.0, 8.0};
    for (int i = 0; i < entity::kItemDropPickupDelay; ++i) {
        CHECK_EQ(g.items.collect(everywhere, inventory), 0);
        g.items.tick(g.world.w());
    }
    CHECK_EQ(g.items.collect(everywhere, inventory), 1);
    CHECK_EQ(g.items.count(), 0);
    CHECK_EQ(int(inventory.main[0].id), int(stoneItem()));
    CHECK_EQ(int(inventory.main[0].count), 1);
}

TEST(a_thrown_item_leaves_the_hand_below_the_eye_and_goes_where_it_is_looking)
{
    // `posY - 0.3 + getEyeHeight()`, and getEyeHeight is 0.12 on the player.
    Ground g;
    CHECK(g.items.dropFromPlayer(g.world.w(), 0.5, 70.0, 0.5, 0.0f, 0.0f, stoneItem(), 1, 0));
    CHECK(g.items[0].y < 70.0);
    CHECK(g.items[0].y > 69.5);

    // Yaw 0 is +z in the original's frame, and the throw is 0.3 along it -- far
    // more than the 0.02 scatter, so the sign is not in doubt.
    CHECK(g.items[0].motionZ > 0.2);

    // And the other way round at 180 degrees.
    Ground back;
    CHECK(back.items.dropFromPlayer(back.world.w(), 0.5, 70.0, 0.5, 180.0f, 0.0f,
                                    stoneItem(), 1, 0));
    CHECK(back.items[0].motionZ < -0.2);
}

TEST(there_is_no_dropped_item_limit)
{
    // a1.1.2 has none, and a Creative player mining fast used to fill the
    // fixed sixty-four and start evicting drops still in the air.
    Ground g;
    const int many = ItemEntitySystem::kInitialCapacity * 3;
    for (int i = 0; i < many; ++i) {
        CHECK(g.items.spawn(g.world.w(), 0.5 + double(i % 8), 66.0, 0.5 + double(i / 8),
                            stoneItem(), 1, 0));
    }
    CHECK_EQ(g.items.count(), many);
    CHECK_EQ(int(g.items.evicted()), 0);
    CHECK_EQ(int(g.items.refused()), 0);
}

TEST(a_drop_the_heap_will_not_hold_replaces_the_oldest_item)
{
    // A broken block's, cart's or painting's drop has nowhere else to be, so
    // when the heap says stop the pool makes room rather than refusing it -- a
    // refusal is a drop that silently never appears.
    Ground g;
    // One item left to age on its own, then the rest spawned fresh with no tick
    // after -- so it is the single oldest.
    CHECK(g.items.spawn(g.world.w(), 0.5, 66.0, 0.5, stoneItem(), 1, 0));
    for (int t = 0; t < 100; ++t) {
        g.items.tick(g.world.w());
    }
    CHECK_EQ(g.items[0].age, 100);
    while (g.items.count() < ItemEntitySystem::kInitialCapacity) {
        CHECK(g.items.spawn(g.world.w(), 4.5, 66.0, 4.5, stoneItem(), 1, 0));
    }

    test::LowHeap low;
    const ItemId dirt = ItemId(mcver::Block::Dirt);
    CHECK(g.items.spawn(g.world.w(), 0.5, 66.0, 0.5, dirt, 1, 0));
    CHECK_EQ(g.items.count(), ItemEntitySystem::kInitialCapacity);
    CHECK_EQ(int(g.items.evicted()), 1);
    CHECK_EQ(int(g.items.refused()), 0);
    CHECK_EQ(int(g.items[0].item), int(dirt));
    CHECK_EQ(g.items[0].age, 0);
}

TEST(a_throw_the_heap_will_not_hold_is_refused_and_stays_in_the_hand)
{
    // The one spawn that refuses rather than evicts: the thrower still holds
    // the item, so refusing loses nothing, where evicting would lose something
    // on the ground.
    Ground g;
    for (int i = 0; i < ItemEntitySystem::kInitialCapacity; ++i) {
        CHECK(g.items.spawn(g.world.w(), 0.5, 66.0, 0.5, stoneItem(), 1, 0));
    }
    test::LowHeap low;
    CHECK(!g.items.dropFromPlayer(g.world.w(), 0.5, 70.0, 0.5, 0.0f, 0.0f, stoneItem(), 1, 0));
    CHECK_EQ(g.items.count(), ItemEntitySystem::kInitialCapacity);
    CHECK_EQ(int(g.items.refused()), 1);
    CHECK_EQ(int(g.items.evicted()), 0);
}

TEST(an_item_that_does_not_fit_is_left_on_the_ground_holding_the_remainder)
{
    // `addItemStackToInventory` answers true only for a stack that went in
    // whole, and the entity survives on a false -- which is what stops a full
    // inventory eating what it could not carry.
    Ground g;
    const ItemId stone = stoneItem();
    CHECK(g.items.spawn(g.world.w(), 0.5, 66.0, 0.5, stone, 40, 0));
    // A plain spawn waits five ticks, not forty; tick them out rather than
    // reaching into the entity.
    g.run(entity::kItemPickupDelay);

    Inventory inventory;
    inventory.clear();
    // Every slot full of something else, bar one holding 60 stone: 4 of the 40
    // fit and 36 have nowhere to go.
    const ItemId other = item::paletteItem(1);
    for (int i = 0; i < item::kMainSlots; ++i) {
        inventory.set(i, other, i8(item::def(other).stack));
    }
    inventory.set(0, stone, 60);

    const AABB everywhere{-8.0, 60.0, -8.0, 8.0, 80.0, 8.0};
    CHECK_EQ(g.items.collect(everywhere, inventory), 0);
    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(g.items[0].count, 36);
    CHECK_EQ(int(inventory.main[0].count), 64);
}

TEST(adding_a_stack_fills_a_part_used_slot_before_an_empty_one)
{
    Inventory inventory;
    inventory.clear();
    const ItemId stone = stoneItem();
    inventory.set(4, stone, 10);

    CHECK_EQ(inventory.addStack(stone, 5), 0);
    CHECK_EQ(int(inventory.main[4].count), 15);
    // Nothing landed anywhere else.
    for (int i = 0; i < item::kMainSlots; ++i) {
        CHECK(i == 4 || inventory.main[i].empty());
    }
}

TEST(adding_more_than_a_slot_holds_spills_into_the_first_empty_one)
{
    Inventory inventory;
    inventory.clear();
    const ItemId stone = stoneItem();
    inventory.set(3, stone, 60);

    // Four top the slot up and the remaining ten go somewhere whole --
    // `storePartialItemStack` tries one slot and `addItemStackToInventory`
    // places what is left in the first gap.
    CHECK_EQ(inventory.addStack(stone, 14), 0);
    CHECK_EQ(int(inventory.main[3].count), 64);
    CHECK_EQ(int(inventory.main[0].id), int(stone));
    CHECK_EQ(int(inventory.main[0].count), 10);
}

TEST(adding_to_a_full_inventory_gives_the_whole_stack_back)
{
    Inventory inventory;
    inventory.clear();
    const ItemId other = item::paletteItem(1);
    for (int i = 0; i < item::kMainSlots; ++i) {
        inventory.set(i, other, i8(item::def(other).stack));
    }
    const ItemId stone = stoneItem();
    CHECK_EQ(inventory.addStack(stone, 7), 7);
}

// ---------------------------------------------------------------------------
// Two heaps stay two heaps, and the clock that only runs while somebody is
// there
// ---------------------------------------------------------------------------
//
// **Nothing merges on the ground**, which is a1.1.2's own behaviour: `dx.e_()`
// has no such step and no version does until `EntityItem.combineItems` in
// 1.3.1. These tests pin that, because the merge was once here and was taken
// out again. The residency rule below is the deviation, and it is marked as
// one in core/entity/item_entity.hpp.

TEST(two_stacks_of_the_same_item_lying_together_stay_two)
{
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 3, 0));
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 5, 0));
    CHECK_EQ(g.items.count(), 2);

    // Long enough to pass every multiple of 25 the merge scan used to wake on.
    g.run(120);
    CHECK_EQ(g.items.count(), 2);
    CHECK_EQ(g.items[0].count, 3);
    CHECK_EQ(g.items[1].count, 5);
}

TEST(a_heap_of_like_stacks_stays_a_heap)
{
    // The pile a broken chest or a Creative handful leaves: eight stacks of one
    // item in one block, and eight entities five minutes later. This is the
    // thing the merge was there to shrink, so it is the thing that pins its
    // absence.
    Ground g;
    for (int i = 0; i < 8; ++i) {
        CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 0));
    }
    g.run(120);
    CHECK_EQ(g.items.count(), 8);
    for (int i = 0; i < g.items.count(); ++i) {
        CHECK_EQ(g.items[i].count, 1);
    }
}

TEST(an_old_stack_does_not_have_its_clock_reset_by_a_fresh_one)
{
    // The merge kept the younger of the two ages, so a fresh stack thrown on to
    // an old one used to hold the pair open. Without it each stack despawns on
    // its own clock, which is what the original does.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 0));
    g.run(500);
    CHECK_EQ(g.items.count(), 1);
    const int old = g.items[0].age;
    CHECK(old >= 500);

    CHECK(g.items.spawn(g.world.w(), g.items[0].x, g.items[0].y, g.items[0].z,
                        stoneItem(), 1, 0));
    g.run(30);
    CHECK_EQ(g.items.count(), 2);
    CHECK(g.items[0].age >= old + 30);
    CHECK(g.items[1].age <= 30);
}

TEST(an_item_outside_a_loaded_column_does_not_age_out)
{
    // The five-minute clock only advances on a tick the item actually takes,
    // and it takes none while its column is gone -- so walking away from a
    // drop and coming back finds it there. SceneWorld holds a 7x7 patch of
    // columns around chunk 0, so chunk 40 is a column that does not exist.
    Ground g;
    const double far = 40.0 * 16.0 + 0.5;
    CHECK(g.items.spawn(g.world.w(), far, 65.0, far, stoneItem(), 1, 0));
    CHECK(!g.world.w().chunkResident(40, 40));

    g.run(entity::kItemMaxAge * 2);

    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(g.items[0].age, 0);
    // It did not fall, either: nothing about the entity ran.
    CHECK_EQ(g.items[0].y, 65.0);
}

// ---------------------------------------------------------------------------
// Lava
// ---------------------------------------------------------------------------

TEST(lava_destroys_a_dropped_item_on_the_tick_it_touches_it)
{
    // `kh.y()` -- Entity.onEntityUpdate -- runs before anything else in
    // `dx.e_()`, and its lava branch is `attackEntityFrom(null, 10)` against a
    // health of 5. So one tick in lava is the end of the stack, and the hop and
    // the fizz below it are what a *falling* item does on the tick it arrives,
    // not a way of surviving.
    Ground g;
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -2; z <= 2; ++z) {
            g.world.place(x, 64, z, block::BlockId(mcver::Block::Lava), 0);
        }
    }

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    CHECK_EQ(g.items.count(), 1);
    g.run(1);
    CHECK_EQ(g.items.count(), 0);
}

TEST(an_item_beside_lava_rather_than_in_it_survives)
{
    // The probe is the box shrunk by 0.4 top and bottom, not expanded, so an
    // item lying on the floor next to a lava cell is not in it. Without this
    // the test above would pass for the wrong reason.
    Ground g;
    g.world.place(2, 64, 0, block::BlockId(mcver::Block::Lava), 0);

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    g.run(40);
    CHECK_EQ(g.items.count(), 1);
}

TEST(fire_burns_up_a_dropped_item)
{
    // **`moveEntity`'s tail is what lights it**, and this test used to say the
    // opposite. The old reading was that nothing in a1.1.2 can set an entity's
    // fire counter except the lava branch, because `og` has no
    // `onEntityCollidedWithBlock` and no class outside `kh` writes `kh.aT`.
    // Both are true; the conclusion was wrong. `kh.c(DDD)V` writes its own
    // counter four times from `isBoundingBoxBurning`, and `dealFireDamage(1)`
    // goes with it -- one point a tick against five health and no
    // invulnerability window, so a stack lying in a flame is gone in five
    // ticks. See core/entity/fire_entry.hpp.
    Ground g;
    g.world.place(0, 64, 0, block::BlockId(mcver::Block::Fire), 0);

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    g.run(4);
    CHECK_EQ(g.items.count(), 1);   // four points gone, one left
    g.run(1);
    CHECK_EQ(g.items.count(), 0);
}

TEST(an_item_beside_a_fire_rather_than_in_it_survives)
{
    // `isBoundingBoxBurning` is generous -- it runs to `floor(max + 1)` on each
    // axis, so it reaches a cell the box only touches the plane of -- but a
    // stack two cells away is still two cells away.
    Ground g;
    g.world.place(2, 64, 0, block::BlockId(mcver::Block::Fire), 0);

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    g.run(40);
    CHECK_EQ(g.items.count(), 1);
    CHECK(g.items[0].health == entity::kItemHealth);
    CHECK(g.items[0].fire <= 0);
}

TEST(a_stack_carried_out_of_a_fire_still_burns_to_nothing)
{
    // The counter outlives the flame: `fire = 300` on the tick it catches, and
    // `kh.y()` spends one of the stack's five points every twentieth tick of
    // it. So a stack that is lit and then moved out of the fire has about a
    // hundred ticks left, not five minutes.
    // **One tick in the open first**, which parks the counter at its fuse of
    // -1: a counter sitting at the constructor's zero goes to one rather than
    // catching, because `fire++` runs before `if (fire == 0)`. See
    // tests/fire_entry_test.cpp.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    g.run(1);
    CHECK_EQ(g.items.count(), 1);

    g.world.place(0, 64, 0, block::BlockId(mcver::Block::Fire), 0);
    g.run(1);
    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(int(g.items[0].fire), entity::kCaughtFireTicks);

    // Put the fire out and move the stack clear of where it was.
    g.world.place(0, 64, 0, block::kAir, 0);
    g.run(200);
    CHECK_EQ(g.items.count(), 0);
}

// ---------------------------------------------------------------------------
// Cactus
// ---------------------------------------------------------------------------
//
// A dropped stack wanders: `dx`'s constructor gives it +-0.1 of horizontal
// motion and the air drag is 0.98, so one that falls a long way can glide a
// block or more. These lay a patch of cactus rather than a single one wherever
// the answer must not depend on where it drifts to.

namespace {

// Cactus over the stone, from -3 to 3 on both axes.
void layCactus(Ground& g)
{
    for (i32 x = -3; x <= 3; ++x) {
        for (i32 z = -3; z <= 3; ++z) {
            g.world.place(x, 64, z, block::BlockId(mcver::Block::Cactus), 0);
        }
    }
}

}  // namespace

TEST(a_cactus_destroys_a_stack_that_lands_on_it)
{
    // `hy.b(Lcn;IIILkh;)V` is `attackEntityFrom(null, 1)` and nothing else,
    // called from `moveEntity`'s tail once per cell the box overlaps. A stack
    // has five health and no invulnerability window, so a cactus destroys what
    // lands on it in a quarter of a second -- where before this it lay there
    // for the full five minutes and the despawn clock did the work.
    Ground g;
    layCactus(g);

    CHECK(g.items.spawn(g.world.w(), 0.5, 68.0, 0.5, stoneItem(), 1, 0));
    CHECK_EQ(g.items.count(), 1);

    g.run(60);
    CHECK_EQ(g.items.count(), 0);
}

TEST(a_cactus_costs_a_stack_one_point_a_tick)
{
    // Spawned in the cell rather than dropped into it, so what is counted is
    // the damage and not the fall: five hits, one a tick, and no window
    // between them.
    Ground g;
    layCactus(g);

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    for (int tick = 1; tick <= 4; ++tick) {
        g.run(1);
        CHECK_EQ(g.items.count(), 1);
        CHECK_EQ(int(g.items[0].health), entity::kItemHealth - tick);
    }
    g.run(1);
    CHECK_EQ(g.items.count(), 0);
}

TEST(a_stack_resting_on_top_of_a_cactus_is_inside_its_cell)
{
    // The detail that makes the whole thing work. A cactus collides at
    // 0.9375 of its cell, so a stack settles with its box bottom there -- and
    // the loop in `moveEntity`'s tail floors the box's bounds with none of the
    // thousandth-of-a-block inset later versions add, so that floor is the
    // cactus's own y. An implementation that inset the bounds would leave a
    // stack sitting on the spikes for ever.
    Ground g;
    layCactus(g);

    CHECK(g.items.spawn(g.world.w(), 0.5, 65.4, 0.5, stoneItem(), 1, 0));
    // Long enough to land and settle, short enough that it is the landing that
    // killed it and not five minutes of ageing.
    g.run(40);
    CHECK_EQ(g.items.count(), 0);
}

TEST(a_stack_two_cells_from_a_cactus_is_not_touching_it)
{
    // The hits are the cells the box overlaps and nothing wider, so a stack a
    // wall away from a cactus is untouched however long it lies there.
    //
    // **The wall has to be a real block**, and that is not padding for the
    // test: a cactus's collision box is inset a sixteenth, so something pushed
    // up against its side is standing *in its cell* while still outside the
    // box it collides with -- which is exactly why walking into a cactus hurts
    // in the original. A stack that drifts against one really is destroyed by
    // it, and only a cell that is not the cactus's keeps it off.
    Ground g;
    for (int y = 64; y <= 65; ++y) {
        g.world.place(1, y, 0, block::BlockId(mcver::Block::Stone), 0);
        g.world.place(-1, y, 0, block::BlockId(mcver::Block::Stone), 0);
        g.world.place(0, y, 1, block::BlockId(mcver::Block::Stone), 0);
        g.world.place(0, y, -1, block::BlockId(mcver::Block::Stone), 0);
    }
    g.world.place(2, 64, 0, block::BlockId(mcver::Block::Cactus), 0);

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    g.run(100);
    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(int(g.items[0].health), entity::kItemHealth);
}

TEST(a_blast_destroys_the_stacks_beside_it_and_throws_the_ones_further_out)
{
    // `je` calls `attackEntityFrom` on every entity in its box, and `dx`'s is
    // five points of health with no invulnerability. Beside a creeper the
    // damage is 25; at 5.5 blocks out of its 6.0 reach it is 3, which a stack
    // survives -- and is pushed away from the centre either way.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    CHECK(g.items.spawn(g.world.w(), 6.0, 64.5, 0.5, stoneItem(), 1, 0));
    CHECK(g.items.spawn(g.world.w(), 20.5, 64.5, 0.5, stoneItem(), 1, 0));
    CHECK_EQ(g.items.count(), 3);
    const double farMotion = g.items[1].motionX;

    entity::Explosion blast(0.5, 64.5, 0.5, entity::kCreeperBlast);
    blast.cast(g.world.w());
    entity::MobSurroundings around;
    around.items = &g.items;
    entity::applyBlast(g.world.w(), blast, nullptr, &around);
    blast.destroy(g.world.w());

    CHECK_EQ(g.items.count(), 2);
    int thrown = 0;
    int untouched = 0;
    for (int i = 0; i < g.items.count(); ++i) {
        if (g.items[i].x > 10.0) {
            CHECK_EQ(int(g.items[i].health), int(entity::kItemHealth));
            ++untouched;
        } else {
            CHECK(g.items[i].health > 0 && g.items[i].health < entity::kItemHealth);
            CHECK(g.items[i].motionX > farMotion);
            ++thrown;
        }
    }
    CHECK_EQ(thrown, 1);
    CHECK_EQ(untouched, 1);
}

TEST(handing_thrown_items_to_a_server_leaves_the_servers_own_alone)
{
    // In a session the pool holds two kinds of stack at once: the ones this
    // console threw, which are the server's to make, and the ones the server
    // has already made and stamped with an id of its own. A client hands over
    // the first kind and must not touch the second -- handing back what the
    // server just sent is how one dropped stack became an endless supply of
    // them. See `NetPlay::forwardDrops`.
    Ground g;
    CHECK(g.items.dropFromPlayer(g.world.w(), 0.5, 66.0, 0.5, 0.0f, 0.0f, stoneItem(), 1, 0));
    CHECK(g.items.spawnFromServer(g.world.w(), 4242, 3.5, 66.0, 3.5, stoneItem(), 2, 0, 0.0,
                                  0.0, 0.0)
          != nullptr);
    CHECK(g.items.dropFromPlayer(g.world.w(), 6.5, 66.0, 6.5, 0.0f, 0.0f, stoneItem(), 1, 0));
    CHECK_EQ(g.items.count(), 3);

    CHECK_EQ(g.items.removeUnowned(), 2);
    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(int(g.items[0].entityId), 4242);

    // And again on a pool that has nothing of its own left to give.
    CHECK_EQ(g.items.removeUnowned(), 0);
    CHECK_EQ(g.items.count(), 1);
}

TEST(a_guests_throw_is_out_of_their_reach_by_the_time_it_can_be_taken)
{
    // "Dropping an item as a guest picks it straight back up." The host spawns
    // a guest's throw from the packet the guest sent, and the a1.1.2 server
    // (`id.a(Lk;)V`) gives it ten ticks; the constructor's five left it inside
    // the thrower's reach on the tick it became collectable. A guest standing
    // at (0.5, 64, 0.5), eye 65.62, looking 30 degrees down along +z: the
    // throw is 0.3 along the look plus 0.1 up.
    const AABB reach{0.2 - 1.0, 64.0, 0.2 - 1.0, 0.8 + 1.0, 65.8, 0.8 + 1.0};
    // 1 when the stack is in reach on the tick it may first be taken, 0 when
    // it is not, -1 when it was never spawned.
    const auto collectableInReach = [&](int delay) {
        Ground g;
        if (!g.items.spawnMoving(g.world.w(), 0.5, 65.62 - 0.3 + 0.12, 0.5, stoneItem(), 1, 0,
                                 0.0, -0.05, 0.2598, delay)) {
            return -1;
        }
        while (g.items[0].pickupDelay > 0) {
            g.items.tick(g.world.w());
        }
        return g.items[0].box.intersects(reach) ? 1 : 0;
    };
    CHECK_EQ(collectableInReach(entity::kThrownByClientPickupDelay), 0);
    // The old delay, kept as the proof of what the report was.
    CHECK_EQ(collectableInReach(entity::kItemPickupDelay), 1);
}
