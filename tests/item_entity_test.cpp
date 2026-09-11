// The dropped item: the throw, the fall, the wait, and the pickup.
//
// **There is no oracle for any of it**, in the sense the physics suite has one:
// `EntityItem`'s two random draws come off `Math.random()` and a time-seeded
// `Random`, so no sequence is reproducible in the *original* either. What is
// checkable is everything the constants decide -- that it lands, that it stops,
// that it cannot be picked up for two seconds, that it ages out at 6000 ticks,
// and that the inventory arithmetic underneath is
// `addItemStackToInventory`'s.

#include "core/entity/item_entity.hpp"
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
    // after -- so it is the single oldest and nothing has merged.
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
// Merging, and the clock that only runs while somebody is there
// ---------------------------------------------------------------------------
//
// Neither of these is a1.1.2's: this version's `dx.e_()` has no merge step at
// all, and its entities live inside chunks rather than in a flat pool. Both
// are marked as deviations in core/entity/item_entity.hpp; what the tests pin
// is the *later* version's arithmetic, which is what they were transcribed
// from.

TEST(two_stacks_of_the_same_item_lying_together_become_one)
{
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 3, 0));
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 5, 0));
    CHECK_EQ(g.items.count(), 2);

    g.run(1);
    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(g.items[0].count, 8);
}

TEST(the_survivor_of_a_merge_keeps_the_younger_age)
{
    // `entityitem.age = Math.min(entityitem.age, age)`. Without it a fresh
    // stack thrown on to a four-minute-old one inherits four minutes and
    // vanishes under the player's feet.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 0));
    g.run(500);
    CHECK_EQ(g.items.count(), 1);
    CHECK(g.items[0].age >= 500);

    // Land the second one on top of the first, and give the pair a tick on a
    // multiple of 25 to find each other.
    CHECK(g.items.spawn(g.world.w(), g.items[0].x, g.items[0].y, g.items[0].z,
                        stoneItem(), 1, 0));
    for (int i = 0; i < 30 && g.items.count() > 1; ++i) {
        g.items.tick(g.world.w());
    }
    CHECK_EQ(g.items.count(), 1);
    CHECK_EQ(g.items[0].count, 2);
    CHECK(g.items[0].age < 100);
}

TEST(two_stacks_that_would_overflow_a_slot_do_not_merge)
{
    // `if (itemstack1.stackSize + itemstack.stackSize > itemstack1.getMaxStackSize())
    //      return false;` -- a merge never leaves a stack over the ceiling, and
    // never splits the remainder either.
    Ground g;
    const int limit = int(item::def(stoneItem()).stack);
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), limit, 0));
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 0));

    g.run(60);
    CHECK_EQ(g.items.count(), 2);
}

TEST(two_different_items_lying_together_stay_two)
{
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, item::paletteItem(0), 1, 0));
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, item::paletteItem(1), 1, 0));
    CHECK(item::paletteItem(0) != item::paletteItem(1));

    g.run(60);
    CHECK_EQ(g.items.count(), 2);
}

TEST(a_worn_stack_does_not_merge_with_a_fresh_one)
{
    // The damage half of the same rule, which `Inventory::addStack` already
    // applies at the other end of the journey.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 0));
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 7));

    g.run(60);
    CHECK_EQ(g.items.count(), 2);
}

TEST(items_far_enough_apart_do_not_reach_each_other)
{
    // `boundingBox.expand(0.5, 0.0, 0.5)`. Two heaps two blocks apart are two
    // heaps.
    Ground g;
    CHECK(g.items.spawn(g.world.w(), 0.5, 65.0, 0.5, stoneItem(), 1, 0));
    CHECK(g.items.spawn(g.world.w(), 4.5, 65.0, 0.5, stoneItem(), 1, 0));

    g.run(60);
    CHECK_EQ(g.items.count(), 2);
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

TEST(fire_does_not_burn_a_dropped_item)
{
    // **a1.1.2's, and it surprises.** Nothing in the client sets an entity's
    // fire counter except the lava branch above: `og` has no
    // `onEntityCollidedWithBlock` at all and no class outside `kh` writes
    // `kh.aT`. So a stack lying in a flame is untouched. Pinned rather than
    // left as a gap, because "fire should burn items" is what everybody
    // expects and the evidence says otherwise.
    Ground g;
    g.world.place(0, 64, 0, block::BlockId(mcver::Block::Fire), 0);

    CHECK(g.items.spawn(g.world.w(), 0.5, 64.5, 0.5, stoneItem(), 1, 0));
    g.run(100);
    CHECK_EQ(g.items.count(), 1);
}
