// The open container screen: which stack a click lands on, and what it sets off.

#include "core/item/container_session.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/tick/furnace.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/tile_entity.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

using namespace mc;
using mc::item::ContainerSession;
using mc::item::Inventory;
using mc::item::ItemStack;
using mc::test::SceneWorld;
using mcver::Block;
using mcver::Item;

namespace {

constexpr i32 kX = 8;
constexpr int kY = 64;
constexpr i32 kZ = 8;

// The first backpack slot's index on a screen with `own` slots of its own.
int backpack(const ContainerSession& s, int n) { return s.containerSlots() + n; }

constexpr int kHotbarSlotsForTest = item::kHotbarSlots;

}  // namespace

TEST(sticks_are_crafted_at_the_workbench_from_the_backpack)
{
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Planks), 2);
    ContainerSession s;
    s.openWorkbench(kX, kY, kZ);
    CHECK_EQ(s.containerSlots(), 10);

    // Pick the planks up, and put one in each of two stacked cells.
    s.click(nullptr, inventory, backpack(s, 0), 0);
    CHECK_EQ(int(s.cursor().count), 2);
    s.click(nullptr, inventory, 1 + 1, 1);            // cell (1, 0)
    s.click(nullptr, inventory, 1 + 1 + 3, 1);        // cell (1, 1)
    CHECK(s.cursor().empty());
    CHECK_EQ(int(s.slotAt(inventory, 0).id), int(Item::Stick));

    // Take the result: four sticks on the cursor, the grid spent.
    s.click(nullptr, inventory, 0, 0);
    CHECK_EQ(int(s.cursor().id), int(Item::Stick));
    CHECK_EQ(int(s.cursor().count), 4);
    CHECK(s.slotAt(inventory, 2).empty());
    CHECK(s.slotAt(inventory, 5).empty());
    CHECK(s.slotAt(inventory, 0).empty());
}

TEST(the_inventory_screens_armour_slots_take_only_their_own_piece)
{
    Inventory inventory;
    inventory.set(10, item::ItemId(Item::IronBoots), 1);
    ContainerSession s;
    s.openInventory();
    CHECK_EQ(s.containerSlots(), 9);

    s.click(nullptr, inventory, backpack(s, 1), 0);  // boots onto the cursor
    // Row 0 is the helmet: refused.
    CHECK(!s.click(nullptr, inventory, 5, 0).changed);
    // Row 3 is the boots, held in armour[0].
    CHECK(s.click(nullptr, inventory, 8, 0).changed);
    CHECK_EQ(int(inventory.armour[0].id), int(Item::IronBoots));
}

TEST(a_furnace_click_writes_the_tile_entity_and_a_tick_shows_after_a_pull)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Furnace), 2);
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Cobblestone), 1);
    inventory.set(10, item::ItemId(Item::Coal), 1);

    ContainerSession s;
    CHECK(s.openFurnace(scene.w(), kX, kY, kZ));
    s.click(&scene.w(), inventory, backpack(s, 0), 0);
    s.click(&scene.w(), inventory, tick::kFurnaceInputSlot, 0);
    s.click(&scene.w(), inventory, backpack(s, 1), 0);
    s.click(&scene.w(), inventory, tick::kFurnaceFuelSlot, 0);

    const world::TileEntity* tile =
        world::findTileEntity(*scene.w().tileEntitiesAt(kX, kZ), kX, kY, kZ);
    CHECK(tile != nullptr);
    CHECK_EQ(int(tile->items.size()), 2);

    for (int t = 0; t < tick::kFurnaceCookTicks; ++t) {
        tick::furnaceTickAt(scene.w(), kX, kY, kZ);
    }
    CHECK(s.pull(&scene.w()));
    CHECK_EQ(int(s.slotAt(inventory, tick::kFurnaceOutputSlot).id), int(Block::Stone));
    CHECK(s.furnaceBurnScaled(14) > 0);
}

TEST(a_double_chests_second_half_starts_at_slot_twenty_seven)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    scene.place(kX + 1, kY, kZ, block::BlockId(Block::Chest), 0);
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Dirt), 5);

    ContainerSession s;
    CHECK(s.openChest(scene.w(), kX, kY, kZ));
    CHECK_EQ(s.containerSlots(), 54);
    CHECK_EQ(s.chestRows(), 6);
    s.click(&scene.w(), inventory, backpack(s, 0), 0);
    s.click(&scene.w(), inventory, 27 + 3, 0);

    const world::TileEntity* second =
        world::findTileEntity(*scene.w().tileEntitiesAt(kX + 1, kZ), kX + 1, kY, kZ);
    CHECK(second != nullptr);
    CHECK_EQ(int(second->items.size()), 1);
    CHECK_EQ(int(second->items[0].slot), 3);
    CHECK_EQ(int(second->items[0].count), 5);
}

TEST(a_screen_on_a_block_that_has_gone_reports_it)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    ContainerSession s;
    CHECK(s.openChest(scene.w(), kX, kY, kZ));
    scene.w().setBlockWithNotify(kX, kY, kZ, block::kAir);
    CHECK(!s.pull(&scene.w()));
}

TEST(closing_hands_back_the_cursor_and_the_grid)
{
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Dirt), 3);
    ContainerSession s;
    s.openWorkbench(kX, kY, kZ);
    s.click(nullptr, inventory, backpack(s, 0), 0);
    s.click(nullptr, inventory, 1, 1);  // one dirt into the grid, two on the cursor
    ItemStack dropped[10];
    CHECK_EQ(s.close(dropped, 10), 2);
    CHECK(!s.isOpen());
    CHECK_EQ(int(dropped[0].count) + int(dropped[1].count), 3);
}

// ---------------------------------------------------------------- quickMove
//
// Ours rather than a1.1.2's -- see ContainerSession::quickMove.

TEST(a_quick_move_takes_a_chest_stack_to_the_hands_last_slot_and_back)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Dirt), 5);

    ContainerSession s;
    CHECK(s.openChest(scene.w(), kX, kY, kZ));

    // The backpack's stack goes into the chest's first slot, and the tile
    // entity is written.
    CHECK(s.quickMove(&scene.w(), inventory, backpack(s, 0)).changed);
    CHECK(inventory.at(9).empty());
    CHECK_EQ(int(s.slotAt(inventory, 0).count), 5);
    const world::TileEntity* tile =
        world::findTileEntity(*scene.w().tileEntitiesAt(kX, kZ), kX, kY, kZ);
    CHECK(tile != nullptr);
    CHECK_EQ(int(tile->items.size()), 1);

    // And back out, into the hand from its right-hand end.
    CHECK(s.quickMove(&scene.w(), inventory, 0).changed);
    CHECK(s.slotAt(inventory, 0).empty());
    CHECK_EQ(int(inventory.at(kHotbarSlotsForTest - 1).id), int(Block::Dirt));
    CHECK_EQ(int(inventory.at(kHotbarSlotsForTest - 1).count), 5);
}

TEST(a_quick_move_tops_up_what_is_there_before_it_takes_an_empty_slot)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    Inventory inventory;
    inventory.set(0, item::ItemId(Block::Dirt), 60);
    inventory.set(9, item::ItemId(Block::Dirt), 10);

    ContainerSession s;
    CHECK(s.openChest(scene.w(), kX, kY, kZ));
    CHECK(s.quickMove(&scene.w(), inventory, backpack(s, 0)).changed);
    CHECK(s.quickMove(&scene.w(), inventory, 0).changed);

    CHECK_EQ(int(inventory.at(0).count), 64);
    CHECK_EQ(int(inventory.at(kHotbarSlotsForTest - 1).count), 6);
    CHECK(inventory.at(9).empty());
}

TEST(a_quick_move_crosses_between_backpack_and_hand_and_puts_armour_on)
{
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Cobblestone), 10);

    item::ItemId helmet = 0;
    for (item::ItemId id = 1; id < item::ItemId(mcver::kItemTableSize); ++id) {
        if (item::def(id).known && item::def(id).armour == 0) {
            helmet = id;
            break;
        }
    }
    CHECK(helmet != 0);
    inventory.set(10, helmet, 1);

    ContainerSession s;
    s.openInventory();

    // Backpack to the hand's first empty slot...
    CHECK(s.quickMove(nullptr, inventory, backpack(s, 0)).changed);
    CHECK(inventory.at(9).empty());
    CHECK_EQ(int(inventory.at(0).count), 10);
    // ...and back again, to the backpack's.
    CHECK(s.quickMove(nullptr, inventory, backpack(s, item::kBackpackSlots)).changed);
    CHECK(inventory.at(0).empty());
    CHECK_EQ(int(inventory.at(9).count), 10);

    // A helmet goes on the head rather than across.
    CHECK(s.quickMove(nullptr, inventory, backpack(s, 1)).changed);
    CHECK(inventory.at(10).empty());
    CHECK_EQ(int(inventory.armour[item::kArmourSlots - 1].id), int(helmet));
}

TEST(a_quick_move_on_the_result_crafts_as_many_as_there_are_ingredients_for)
{
    Inventory inventory;
    inventory.set(9, item::ItemId(Block::Planks), 3);
    inventory.set(10, item::ItemId(Block::Planks), 3);
    ContainerSession s;
    s.openWorkbench(kX, kY, kZ);

    s.click(nullptr, inventory, backpack(s, 0), 0);
    s.click(nullptr, inventory, 1 + 1, 0);        // cell (1, 0)
    s.click(nullptr, inventory, backpack(s, 1), 0);
    s.click(nullptr, inventory, 1 + 1 + 3, 0);    // cell (1, 1)
    CHECK_EQ(int(s.slotAt(inventory, 0).id), int(Item::Stick));

    CHECK(s.quickMove(nullptr, inventory, 0).changed);
    CHECK_EQ(int(inventory.at(kHotbarSlotsForTest - 1).id), int(Item::Stick));
    CHECK_EQ(int(inventory.at(kHotbarSlotsForTest - 1).count), 12);
    CHECK(s.slotAt(inventory, 0).empty());
    CHECK(s.slotAt(inventory, 2).empty());
    CHECK(s.slotAt(inventory, 5).empty());
    CHECK(s.cursor().empty());
}

TEST(a_quick_move_into_a_furnace_puts_fuel_on_the_fire_and_ore_in_the_top)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Furnace), 2);
    Inventory inventory;
    inventory.set(9, item::ItemId(Item::Coal), 3);
    inventory.set(10, item::ItemId(Block::Cobblestone), 4);

    ContainerSession s;
    CHECK(s.openFurnace(scene.w(), kX, kY, kZ));
    CHECK(s.quickMove(&scene.w(), inventory, backpack(s, 0)).changed);
    CHECK(s.quickMove(&scene.w(), inventory, backpack(s, 1)).changed);
    CHECK_EQ(int(s.slotAt(inventory, tick::kFurnaceFuelSlot).id), int(Item::Coal));
    CHECK_EQ(int(s.slotAt(inventory, tick::kFurnaceInputSlot).id), int(Block::Cobblestone));
    CHECK_EQ(int(s.slotAt(inventory, tick::kFurnaceInputSlot).count), 4);
}
