// Creative's core state: the item palette and the inventory behind it.
//
// **There is no oracle for having a Creative mode and there cannot be one.**
// a1.1.2 has none, so nothing here compares against a real client the way
// player_body_test.cpp and ray_trace_test.cpp do.
//
// What *is* checkable, and is what this file checks, is that the palette is
// **derived**. It used to be "every block in the registry, minus air", computed
// in C++; it is now a generated column measured out of a running jar, and the
// two things worth asserting are that the column says what it should about the
// cases it was introduced for -- a door offers the door *item*, the burning
// furnace is not offered at all -- and that nothing in it fails to place.
//
// The inventory half is ordinary state-machine testing, plus the one property
// that is a promise rather than a behaviour: a slot number this version does
// not model survives a load and a save.

#include "core/block/registry.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "framework.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace mc;
using mc::item::Inventory;
using mc::item::ItemId;
using mc::item::ItemStack;
using mc::item::kBackpackSlots;
using mc::item::kHotbarSlots;
using mc::item::kMainSlots;
using mc::item::paletteIndexOf;
using mc::item::paletteItem;
using mc::item::paletteSize;

namespace {

// The item this version calls `name`, or 0. Looked up rather than written down,
// so a test can say "the door item" without a literal id in it -- which is the
// same rule the engine is held to.
ItemId itemNamed(const char* name, item::IconSheet sheet)
{
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = item::def(ItemId(id));
        if (def.known && def.sheet == sheet && std::strcmp(def.name, name) == 0) {
            return ItemId(id);
        }
    }
    return 0;
}

}  // namespace

TEST(palette_offers_every_item_this_version_defines)
{
    CHECK(paletteSize() > 0);
    for (int i = 0; i < paletteSize(); ++i) {
        const ItemId id = paletteItem(i);
        CHECK(id != 0);
        const item::ItemDef& def = item::def(id);
        CHECK(def.known);
        CHECK(def.palette);
    }

    // **And it really is everything, not most things.** The palette used to
    // require `places != 0`, which offered 63 of the table's 149 rows and left
    // the swords, the ingots, the smelted ores and every piece of armour out of
    // a Creative hand -- which is where they are put into a chest, into a save,
    // or on the ground. The two exclusions that remain are both duplicates:
    // engine-only ids, and an ItemBlock whose block already has a carried form.
    int known = 0;
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        if (item::def(ItemId(id)).known) {
            ++known;
        }
    }
    known += mcver::kItemsOutsideTable;
    CHECK(paletteSize() > known / 2);
    CHECK(paletteSize() < known);

    // A sword places nothing and is offered anyway, which is the change.
    const ItemId sword = itemNamed("iron_sword", item::IconSheet::Items);
    CHECK(sword != 0);
    CHECK_EQ(int(item::def(sword).places), 0);
    CHECK(paletteIndexOf(sword) >= 0);
}

TEST(the_two_music_discs_are_in_the_table_and_in_the_palette)
{
    // **They are 1,910 ids past the end of the item run**, so they are not rows
    // of the contiguous array -- they reach `item::def` through a two-entry
    // side table. Before that they had no icon and no name, and one in a hotbar
    // slot drew as an empty slot.
    //
    // They are obtainable: `dd.b(Lkh;)V` drops one when a **skeleton** kills a
    // creeper, and that is a1.1.2's only source of a record. So a drop that
    // could not be drawn was a real hole and not a curiosity.
    CHECK_EQ(mcver::kItemsOutsideTable, 2);

    for (const ItemId id : {ItemId(2256), ItemId(2257)}) {
        const item::ItemDef& def = item::def(id);
        CHECK(def.known);
        CHECK(def.palette);
        // An icon on the items sheet, which is what draws it: `di.aQ.a(240)`
        // and `di.aR.a(241)` in the class file.
        CHECK(def.sheet == item::IconSheet::Items);
        CHECK(def.icon >= 240);
        // A record does not stack -- `lg`'s constructor sets `maxStackSize` to
        // 1 -- and it places no block.
        CHECK_EQ(int(def.stack), 1);
        CHECK_EQ(int(def.places), 0);
        // Named, rather than `item_2256`. `lg`'s second argument is the record
        // name that `World.playRecord` takes, and `getItemName` is null for
        // both, which is why they used to fall through to the id.
        CHECK(std::string(def.name).rfind("record_", 0) == 0);
        // And in the palette, findable both ways.
        const int at = paletteIndexOf(id);
        CHECK(at >= 0);
        CHECK_EQ(int(paletteItem(at)), int(id));
    }

    // They are the last two, because the palette is in id order.
    CHECK_EQ(int(paletteItem(paletteSize() - 2)), 2256);
    CHECK_EQ(int(paletteItem(paletteSize() - 1)), 2257);

    // An id in neither the array nor the side table is still the unknown item
    // rather than a read past the end.
    CHECK(!item::def(ItemId(9999)).known);
}

TEST(palette_is_in_id_order_and_invertible)
{
    int previous = -1;
    for (int i = 0; i < paletteSize(); ++i) {
        const int id = int(paletteItem(i));
        CHECK(id > previous);
        previous = id;
        CHECK_EQ(paletteIndexOf(ItemId(id)), i);
    }
    CHECK_EQ(paletteIndexOf(0), -1);
}

TEST(palette_answers_nothing_off_the_end)
{
    // The UI's page arithmetic runs off the end of the last page by design -- a
    // page is a fixed grid and the palette does not divide evenly into it -- so
    // this is the ordinary case rather than the error case.
    CHECK_EQ(int(paletteItem(-1)), 0);
    CHECK_EQ(int(paletteItem(paletteSize())), 0);
    CHECK_EQ(int(paletteItem(paletteSize() + 1000)), 0);
}

TEST(palette_offers_the_door_item_and_not_the_door_block)
{
    // **The bug this column was added for.** Block 64's texture is the lower
    // half of a door, because that is what the block is drawn with; the door a
    // player holds is an item on the other sheet. Offering the block form put
    // half a door in the hotbar.
    const ItemId doorItem = itemNamed("wooden_door", item::IconSheet::Items);
    const ItemId doorBlock = itemNamed("wooden_door", item::IconSheet::Terrain);
    CHECK(doorItem != 0);
    CHECK(doorBlock != 0);
    CHECK(paletteIndexOf(doorItem) >= 0);
    CHECK_EQ(paletteIndexOf(doorBlock), -1);
    // ...and it still places the block, which is the half that has to keep
    // working for the swap to be invisible in the world.
    CHECK_EQ(int(item::def(doorItem).places), int(doorBlock));
}

TEST(palette_offers_the_reed_item_and_not_the_reed_block)
{
    const ItemId reedItem = itemNamed("sugar_cane", item::IconSheet::Items);
    const ItemId reedBlock = itemNamed("sugar_cane", item::IconSheet::Terrain);
    CHECK(reedItem != 0);
    CHECK(reedBlock != 0);
    CHECK(paletteIndexOf(reedItem) >= 0);
    CHECK_EQ(paletteIndexOf(reedBlock), -1);
    CHECK_EQ(int(item::def(reedItem).places), int(reedBlock));
}

TEST(palette_hides_the_block_states_a_player_never_holds)
{
    // The four ids the generator excludes by hand, named here by what they are
    // rather than by number. Their resting forms stay offered, which is the
    // half that says this is an exclusion and not a deletion.
    const char* hidden[] = {"flowing_water", "flowing_lava", "fire", "lit_furnace"};
    for (const char* name : hidden) {
        const ItemId id = itemNamed(name, item::IconSheet::Terrain);
        CHECK(id != 0);
        CHECK_EQ(paletteIndexOf(id), -1);
    }
    for (const char* name : {"water", "lava", "furnace"}) {
        const ItemId id = itemNamed(name, item::IconSheet::Terrain);
        CHECK(id != 0);
        CHECK(paletteIndexOf(id) >= 0);
    }
}

TEST(inventory_starts_empty_and_fills_the_hand_from_the_palette)
{
    Inventory inventory;
    inventory.clear();
    CHECK_EQ(inventory.selected, 0);
    CHECK_EQ(int(inventory.selectedItem()), 0);
    CHECK(inventory.empty());

    inventory.fillHandFromPalette();
    CHECK(!inventory.empty());
    for (int i = 0; i < kHotbarSlots; ++i) {
        CHECK_EQ(int(inventory.main[i].id), int(paletteItem(i)));
        CHECK_EQ(int(inventory.main[i].slot), i);
    }
    CHECK_EQ(int(inventory.selectedItem()), int(paletteItem(0)));
    // The backpack is untouched: the opening hand is a hand, not an inventory.
    for (int i = kHotbarSlots; i < kMainSlots; ++i) {
        CHECK(inventory.main[i].empty());
    }
}

TEST(inventory_selection_wraps_both_ways)
{
    Inventory inventory;
    inventory.clear();
    inventory.fillHandFromPalette();

    inventory.cycle(-1);
    CHECK_EQ(inventory.selected, kHotbarSlots - 1);
    CHECK_EQ(int(inventory.selectedItem()), int(paletteItem(kHotbarSlots - 1)));

    inventory.cycle(1);
    CHECK_EQ(inventory.selected, 0);

    for (int i = 0; i < kHotbarSlots; ++i) {
        CHECK_EQ(inventory.selected, i);
        inventory.cycle(1);
    }
    CHECK_EQ(inventory.selected, 0);
}

TEST(inventory_set_replaces_and_zero_clears)
{
    Inventory inventory;
    inventory.clear();
    inventory.fillHandFromPalette();

    const ItemId last = paletteItem(paletteSize() - 1);
    inventory.set(4, last, 12);
    CHECK_EQ(int(inventory.main[4].id), int(last));
    CHECK_EQ(int(inventory.main[4].count), 12);

    inventory.selected = 4;
    CHECK_EQ(int(inventory.selectedItem()), int(last));

    inventory.set(4, 0, 0);
    CHECK(inventory.main[4].empty());
    CHECK_EQ(int(inventory.selectedItem()), 0);

    // Out of range is a no-op, not a write past the array.
    inventory.set(-1, last, 1);
    inventory.set(kMainSlots, last, 1);
    for (int i = 0; i < kHotbarSlots; ++i) {
        CHECK(i == 4 || !inventory.main[i].empty());
    }
}

TEST(dropping_takes_one_off_the_hand_and_empties_the_slot_at_zero)
{
    Inventory inventory;
    inventory.clear();
    const ItemId stone = paletteItem(0);
    inventory.set(3, stone, 3);
    inventory.selected = 3;

    CHECK_EQ(int(inventory.dropOne()), int(stone));
    CHECK_EQ(int(inventory.main[3].count), 2);
    CHECK_EQ(int(inventory.dropOne()), int(stone));
    CHECK_EQ(int(inventory.main[3].count), 1);

    // The last one empties the slot rather than leaving a stack of nothing --
    // `decrStackSize` nulls the entry, and a zero-count stack would be written
    // to the save file as a real item.
    CHECK_EQ(int(inventory.dropOne()), int(stone));
    CHECK(inventory.main[3].empty());
    CHECK_EQ(int(inventory.main[3].id), int(item::kEmptyItemId));

    // An empty hand drops nothing and says so.
    CHECK_EQ(int(inventory.dropOne()), 0);
    CHECK(inventory.main[3].empty());
}

TEST(dropping_out_of_a_selection_that_is_not_a_hand_slot_does_nothing)
{
    Inventory inventory;
    inventory.clear();
    inventory.fillHandFromPalette();
    inventory.selected = kHotbarSlots;  // never written by cycle(), but not a crash either
    CHECK_EQ(int(inventory.dropOne()), 0);
    inventory.selected = -1;
    CHECK_EQ(int(inventory.dropOne()), 0);
}

TEST(inventory_holds_the_block_the_held_item_places)
{
    Inventory inventory;
    inventory.clear();
    const ItemId doorItem = itemNamed("wooden_door", item::IconSheet::Items);
    inventory.set(0, doorItem, 1);
    inventory.selected = 0;
    // The one question the placement path asks, and the reason it does not ask
    // for a block id: the door in the hand is not the door in the world.
    CHECK_EQ(int(inventory.selectedPlacesBlock()),
             int(itemNamed("wooden_door", item::IconSheet::Terrain)));
}

TEST(inventory_swap_moves_stacks_and_keeps_slot_numbers)
{
    Inventory inventory;
    inventory.clear();
    const ItemId stone = paletteItem(0);
    inventory.set(0, stone, 7);

    // Into an empty backpack cell: a move.
    inventory.swap(0, kHotbarSlots);
    CHECK(inventory.main[0].empty());
    CHECK_EQ(int(inventory.main[kHotbarSlots].id), int(stone));
    CHECK_EQ(int(inventory.main[kHotbarSlots].count), 7);
    // The slot number is the array position, not something that travels with
    // the stack -- swapping them would write both entries to the wrong place.
    CHECK_EQ(int(inventory.main[kHotbarSlots].slot), kHotbarSlots);
    CHECK_EQ(int(inventory.main[0].slot), 0);

    // Onto a full cell: an exchange.
    const ItemId dirt = paletteItem(2);
    inventory.set(0, dirt, 3);
    inventory.swap(0, kHotbarSlots);
    CHECK_EQ(int(inventory.main[0].id), int(stone));
    CHECK_EQ(int(inventory.main[kHotbarSlots].id), int(dirt));
    CHECK_EQ(int(inventory.main[0].slot), 0);
    CHECK_EQ(int(inventory.main[kHotbarSlots].slot), kHotbarSlots);
}

TEST(an_armour_slot_takes_only_the_piece_that_belongs_in_it)
{
    // `SlotArmor.isItemValid`, which this build had no equivalent of at all --
    // so every one of the four took every item and a helmet could be worn on
    // the feet. The ids come out of the generated table by their armour column
    // rather than being written down here: what is asserted is the *rule*, and
    // a version whose armour lives at other ids still exercises it.
    using mc::item::ItemDef;
    ItemId helmet = 0;
    ItemId boots = 0;
    ItemId notArmour = 0;
    for (int id = 1; id < mcver::kItemTableSize && (helmet == 0 || boots == 0
                                                    || notArmour == 0); ++id) {
        const ItemDef& d = item::def(ItemId(id));
        if (!d.known) {
            continue;
        }
        if (d.armour == 0 && helmet == 0) {
            helmet = ItemId(id);
        } else if (d.armour == 3 && boots == 0) {
            boots = ItemId(id);
        } else if (d.armour == ItemDef::kNotArmour && notArmour == 0) {
            notArmour = ItemId(id);
        }
    }
    CHECK(helmet != 0);
    CHECK(boots != 0);
    CHECK(notArmour != 0);

    // armorType counts from the head, armorInventory from the feet, so the
    // helmet lands in the *last* of the four.
    const int helmetSlot = Inventory::armourSlotFor(helmet);
    const int bootsSlot = Inventory::armourSlotFor(boots);
    CHECK_EQ(helmetSlot, mc::item::kArmourBase + mc::item::kArmourSlots - 1);
    CHECK_EQ(bootsSlot, mc::item::kArmourBase);
    CHECK_EQ(Inventory::armourSlotFor(notArmour), -1);

    CHECK(Inventory::accepts(helmetSlot, helmet));
    CHECK(!Inventory::accepts(helmetSlot, boots));
    CHECK(!Inventory::accepts(bootsSlot, helmet));
    CHECK(!Inventory::accepts(helmetSlot, notArmour));
    // Emptying an armour slot is the same swap run backwards, so nothing may
    // refuse an empty stack.
    CHECK(Inventory::accepts(helmetSlot, 0));
    // The thirty-six take anything at all.
    CHECK(Inventory::accepts(0, helmet));
    CHECK(Inventory::accepts(kHotbarSlots, notArmour));
}

TEST(a_refused_armour_swap_changes_nothing_at_either_end)
{
    // A swap is one move: a slot that refuses must leave the *other* slot alone
    // too, or the stack is destroyed rather than rejected.
    using mc::item::ItemDef;
    ItemId helmet = 0;
    for (int id = 1; id < mcver::kItemTableSize && helmet == 0; ++id) {
        if (item::def(ItemId(id)).known && item::def(ItemId(id)).armour == 0) {
            helmet = ItemId(id);
        }
    }
    CHECK(helmet != 0);

    Inventory inventory;
    inventory.clear();
    inventory.set(0, helmet, 1);

    const int boots = mc::item::kArmourBase;   // the feet
    inventory.swap(0, boots);
    CHECK_EQ(int(inventory.at(0).id), int(helmet));
    CHECK(inventory.at(boots).empty());

    // And the slot it does belong in still takes it.
    const int head = mc::item::kArmourBase + mc::item::kArmourSlots - 1;
    inventory.swap(0, head);
    CHECK(inventory.at(0).empty());
    CHECK_EQ(int(inventory.at(head).id), int(helmet));
    CHECK_EQ(int(inventory.at(head).slot), head);

    // And it comes back off again.
    inventory.swap(head, 0);
    CHECK_EQ(int(inventory.at(0).id), int(helmet));
    CHECK(inventory.at(head).empty());
}

TEST(inventory_round_trips_through_the_save_list)
{
    Inventory inventory;
    inventory.clear();
    inventory.fillHandFromPalette();
    inventory.set(kHotbarSlots + 5, paletteItem(3), 42);

    std::vector<ItemStack> stacks;
    inventory.save(&stacks);
    // Sparse and ascending, which is what InventoryPlayer.writeToNBT produces.
    CHECK_EQ(int(stacks.size()), kHotbarSlots + 1);
    for (usize i = 1; i < stacks.size(); ++i) {
        CHECK(stacks[i - 1].slot < stacks[i].slot);
    }

    Inventory reloaded;
    reloaded.load(stacks);
    for (int i = 0; i < kMainSlots; ++i) {
        CHECK_EQ(int(reloaded.main[i].id), int(inventory.main[i].id));
        CHECK_EQ(int(reloaded.main[i].count), int(inventory.main[i].count));
    }
}

TEST(inventory_keeps_a_slot_number_this_version_does_not_model)
{
    // **The promise, and it is not hypothetical.** The reference a1.1.2 world
    // in this project's own docs has a stack at slot 81, which is neither a
    // main slot nor an armour one. `InventoryPlayer.readFromNBT` drops it; this
    // does not, because dropping it is a load/save cycle destroying data.
    ItemStack stranger;
    stranger.id = 263;
    stranger.count = 2;
    stranger.slot = 81;

    std::vector<ItemStack> stacks;
    stacks.push_back(stranger);

    Inventory inventory;
    inventory.load(stacks);
    CHECK_EQ(int(inventory.unmodelled.size()), 1);
    CHECK(inventory.empty());

    std::vector<ItemStack> written;
    inventory.save(&written);
    CHECK_EQ(int(written.size()), 1);
    CHECK_EQ(int(written[0].slot), 81);
    CHECK_EQ(int(written[0].id), 263);
    CHECK_EQ(int(written[0].count), 2);
}

TEST(inventory_load_forgets_the_last_world)
{
    Inventory inventory;
    inventory.clear();
    inventory.fillHandFromPalette();

    ItemStack stranger;
    stranger.id = 263;
    stranger.count = 1;
    stranger.slot = 81;
    inventory.load(std::vector<ItemStack>{stranger});

    // Every modelled slot is empty again, and only the stranger came across.
    CHECK(inventory.empty());
    CHECK_EQ(int(inventory.unmodelled.size()), 1);
}

TEST(a_creative_quick_move_crosses_between_hand_and_backpack_topping_up_first)
{
    Inventory inventory;
    inventory.clear();
    const ItemId stone = paletteItem(0);
    const int ceiling = int(item::def(stone).stack);
    CHECK(ceiling > 10);

    // The hand's stack tops up the backpack's before it takes an empty cell.
    inventory.set(kHotbarSlots + 4, stone, i8(ceiling - 3));
    inventory.set(2, stone, 10);
    CHECK(inventory.quickMove(2));
    CHECK_EQ(int(inventory.main[kHotbarSlots + 4].count), ceiling);
    CHECK_EQ(int(inventory.main[kHotbarSlots].id), int(stone));
    CHECK_EQ(int(inventory.main[kHotbarSlots].count), 7);
    CHECK_EQ(int(inventory.main[kHotbarSlots].slot), kHotbarSlots);
    CHECK(inventory.main[2].empty());
    CHECK_EQ(int(inventory.main[2].slot), 2);

    // And back: the backpack goes to the hand's first empty slot.
    CHECK(inventory.quickMove(kHotbarSlots));
    CHECK_EQ(int(inventory.main[0].id), int(stone));
    CHECK_EQ(int(inventory.main[0].count), 7);
    CHECK(inventory.main[kHotbarSlots].empty());

    // A full hand leaves the stack where it was.
    const ItemId dirt = paletteItem(2);
    for (int i = 0; i < kHotbarSlots; ++i) {
        inventory.set(i, dirt, 1);
    }
    inventory.set(kHotbarSlots + 5, stone, 4);
    CHECK(!inventory.quickMove(kHotbarSlots + 5));
    CHECK_EQ(int(inventory.main[kHotbarSlots + 5].count), 4);
    CHECK(!inventory.quickMove(kHotbarSlots + 6));
}

TEST(a_creative_quick_move_puts_armour_on_and_takes_it_off)
{
    using mc::item::ItemDef;
    ItemId boots = 0;
    for (int id = 1; id < mcver::kItemTableSize && boots == 0; ++id) {
        const ItemDef& d = item::def(ItemId(id));
        if (d.known && d.armour == 3) {
            boots = ItemId(id);
        }
    }
    CHECK(boots != 0);
    const int bootsSlot = Inventory::armourSlotFor(boots);

    Inventory inventory;
    inventory.clear();
    inventory.set(kHotbarSlots + 3, boots, 1);
    CHECK(inventory.quickMove(kHotbarSlots + 3));
    CHECK_EQ(int(inventory.at(bootsSlot).id), int(boots));
    CHECK_EQ(int(inventory.at(bootsSlot).slot), bootsSlot);
    CHECK(inventory.main[kHotbarSlots + 3].empty());

    // A second pair has nowhere to be worn and crosses to the hand instead.
    inventory.set(kHotbarSlots + 3, boots, 1);
    CHECK(inventory.quickMove(kHotbarSlots + 3));
    CHECK_EQ(int(inventory.main[0].id), int(boots));

    // Off again: the backpack before the hand.
    CHECK(inventory.quickMove(bootsSlot));
    CHECK(inventory.at(bootsSlot).empty());
    CHECK_EQ(int(inventory.main[kHotbarSlots].id), int(boots));
}

TEST(a_palette_quick_move_gives_a_full_stack_to_the_hand_then_the_backpack)
{
    Inventory inventory;
    inventory.clear();
    const ItemId stone = paletteItem(0);
    const int ceiling = int(item::def(stone).stack);
    const int full = ceiling < 64 ? ceiling : 64;

    inventory.set(4, stone, i8(full - 1));
    CHECK(inventory.giveStack(stone));
    CHECK_EQ(int(inventory.main[4].count), full);
    CHECK_EQ(int(inventory.main[0].id), int(stone));
    CHECK_EQ(int(inventory.main[0].count), full - 1);

    // A full hand spills into the backpack; a full inventory takes nothing.
    const ItemId dirt = paletteItem(2);
    for (int i = 0; i < kHotbarSlots; ++i) {
        inventory.set(i, dirt, i8(full));
    }
    CHECK(inventory.giveStack(stone));
    CHECK_EQ(int(inventory.main[kHotbarSlots].count), full);
    for (int i = 0; i < kMainSlots; ++i) {
        inventory.set(i, dirt, i8(full));
    }
    CHECK(!inventory.giveStack(stone));
    CHECK(!inventory.giveStack(0));
}
