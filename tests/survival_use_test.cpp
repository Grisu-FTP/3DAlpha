// What a Survival right-click costs the stack, and the two pieces of core the
// console build leans on to charge it: which of the item and the block took the
// click, and the bow's search for an arrow.
//
// The costs are read off the item table's columns, and those columns were
// checked against every class in the jar that writes `stackSize` or calls
// `damageItem` from a use method -- see core/item/tool_rules.hpp. These cases
// pin the reading, one item per class.

#include "core/entity/ray_trace.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/item/tool_rules.hpp"
#include "core/item/use.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

using namespace mc;
using mc::entity::RayHit;
using mc::test::SceneWorld;
using mcver::Block;
using mcver::Item;

namespace {

item::ItemId i(Item id) { return item::ItemId(id); }

const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

RayHit topOf(i32 x, int y, i32 z)
{
    RayHit hit;
    hit.hit = true;
    hit.x = x;
    hit.y = y;
    hit.z = z;
    hit.face = mesh::kFacePosY;
    return hit;
}

}  // namespace

TEST(placing_anything_that_puts_a_block_down_spends_one)
{
    CHECK_EQ(item::spendOnUse(item::ItemId(Block::Stone)), 1);  // av
    CHECK_EQ(item::spendOnUse(i(Item::WoodenDoor)), 1);          // ec
    CHECK_EQ(item::spendOnUse(i(Item::Seeds)), 1);               // jn
    CHECK_EQ(item::spendOnUse(i(Item::SugarCane)), 1);           // gf
    CHECK_EQ(item::spendOnUse(i(Item::Redstone)), 1);            // ef
    CHECK_EQ(item::spendOnUse(i(Item::Sign)), 1);                // md
}

TEST(hanging_launching_or_railing_an_entity_spends_one)
{
    CHECK_EQ(item::spendOnUse(i(Item::Painting)), 1);  // od
    CHECK_EQ(item::spendOnUse(i(Item::Boat)), 1);      // me
    CHECK_EQ(item::spendOnUse(i(Item::Minecart)), 1);  // jo
}

// **A music disc places nothing and spends anyway**, which is why it is a row
// and not a column: `lg.a` ends with `itemstack.stackSize--` on its success
// path exactly as the nine above do, and neither `places` nor `spawns` can say
// so. Without it a disc put into a jukebox stayed in the hand and taking it
// back out was a second one -- reported from play.
TEST(a_music_disc_spends_one_when_it_goes_into_a_jukebox)
{
    CHECK_EQ(item::spendOnUse(item::ItemId(mcver::Item::Record13)), 1);   // lg
    CHECK_EQ(item::spendOnUse(item::ItemId(mcver::Item::RecordCat)), 1);
    CHECK_EQ(item::wearOnUse(item::ItemId(mcver::Item::Record13)), 0);
}

TEST(a_hoe_and_flint_and_steel_wear_instead_of_spending)
{
    CHECK_EQ(item::spendOnUse(i(Item::WoodenHoe)), 0);
    CHECK_EQ(item::wearOnUse(i(Item::WoodenHoe)), 1);
    CHECK_EQ(item::spendOnUse(i(Item::FlintAndSteel)), 0);
    CHECK_EQ(item::wearOnUse(i(Item::FlintAndSteel)), 1);
}

TEST(a_sword_a_bow_a_bucket_and_an_empty_hand_cost_nothing_to_use)
{
    for (item::ItemId held : {i(Item::IronSword), i(Item::Bow), i(Item::Bucket),
                              i(Item::WaterBucket), item::ItemId(0)}) {
        CHECK_EQ(item::spendOnUse(held), 0);
        CHECK_EQ(item::wearOnUse(held), 0);
    }
}

TEST(the_bow_takes_its_arrow_from_the_first_slot_that_has_one)
{
    item::Inventory inventory;
    inventory.set(0, i(Item::Bow), 1);
    inventory.set(20, i(Item::Arrow), 2);
    inventory.set(30, i(Item::Arrow), 5);
    CHECK(inventory.consumeOne(i(Item::Arrow)));
    CHECK_EQ(int(inventory.at(20).count), 1);
    CHECK_EQ(int(inventory.at(30).count), 5);
    CHECK(inventory.consumeOne(i(Item::Arrow)));
    CHECK(inventory.at(20).empty());
    CHECK(inventory.consumeOne(i(Item::Arrow)));
    CHECK_EQ(int(inventory.at(30).count), 4);
}

TEST(a_quiver_that_is_empty_refuses_the_shot)
{
    item::Inventory inventory;
    inventory.set(0, i(Item::Bow), 1);
    CHECK(!inventory.consumeOne(i(Item::Arrow)));
    CHECK_EQ(int(inventory.at(0).count), 1);
}

TEST(a_right_click_says_whether_the_item_or_the_block_took_it)
{
    SceneWorld scene(0, 0);
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -2; z <= 2; ++z) {
            scene.place(x, 63, z, block::BlockId(Block::Stone), 0);
        }
    }

    // Stone put down on stone: the item took it, and Survival pays for it.
    bool itemTook = false;
    CHECK(item::rightClick(scene.w(), item::ItemId(Block::Stone), topOf(0, 63, 0), kNoPlayer,
                           0.0f, {}, &itemTook));
    CHECK(itemTook);
    CHECK_EQ(int(scene.w().blockAt(0, 64, 0)), int(Block::Stone));

    // A lever flicked with stone in hand: the block took it, and nothing is
    // spent -- the stone never left the hand.
    scene.place(1, 64, 1, block::BlockId(Block::Lever), 5);
    itemTook = true;
    CHECK(item::rightClick(scene.w(), item::ItemId(Block::Stone), topOf(1, 64, 1), kNoPlayer,
                           0.0f, {}, &itemTook));
    CHECK(!itemTook);
    CHECK_EQ(int(scene.w().blockAt(1, 65, 1)), 0);

    // Nothing to place and nothing to activate: not taken at all.
    itemTook = true;
    CHECK(!item::rightClick(scene.w(), i(Item::IronSword), topOf(-1, 63, -1), kNoPlayer, 0.0f,
                            {}, &itemTook));
    CHECK(!itemTook);
}
