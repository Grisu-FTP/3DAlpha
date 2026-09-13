// One click on a container slot, against `ee.a(III)V`'s branches.
//
// Every expected stack here is the method's arithmetic worked by hand; the
// cases are named for the rule each pins, so a regression says which one.

#include "core/item/container.hpp"
#include "core/item/item_stack.hpp"
#include "core/item/registry.hpp"
#include "framework.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

using namespace mc;
using mc::item::ItemStack;
using mc::item::SlotRule;
using mcver::Block;
using mcver::Item;

namespace {

ItemStack stack(i16 id, int count, i16 damage = 0)
{
    ItemStack s;
    s.id = id;
    s.count = i8(count);
    s.damage = damage;
    return s;
}

i16 id(Item item) { return i16(item); }
i16 id(Block block) { return i16(block); }

}  // namespace

TEST(an_empty_cursor_picks_up_all_on_the_left_and_half_on_the_right)
{
    ItemStack slot = stack(id(Block::Cobblestone), 5);
    ItemStack cursor;
    CHECK(item::clickSlot(slot, cursor, 1, SlotRule::Any).pickedUp);
    CHECK_EQ(int(cursor.count), 3);  // (5 + 1) / 2
    CHECK_EQ(int(slot.count), 2);

    ItemStack cursor2;
    item::clickSlot(slot, cursor2, 0, SlotRule::Any);
    CHECK_EQ(int(cursor2.count), 2);
    CHECK(slot.empty());
}

TEST(a_result_slot_gives_up_the_whole_stack_even_to_a_right_click)
{
    ItemStack result = stack(id(Block::Torch), 4);
    ItemStack cursor;
    const item::SlotClick click = item::clickSlot(result, cursor, 1, SlotRule::TakeOnly);
    CHECK(click.pickedUp);
    CHECK_EQ(int(cursor.count), 4);
    CHECK(result.empty());
}

TEST(a_full_cursor_puts_down_all_on_the_left_and_one_on_the_right)
{
    ItemStack slot;
    ItemStack cursor = stack(id(Block::Dirt), 10);
    item::clickSlot(slot, cursor, 1, SlotRule::Any);
    CHECK_EQ(int(slot.count), 1);
    CHECK_EQ(int(cursor.count), 9);

    ItemStack other;
    item::clickSlot(other, cursor, 0, SlotRule::Any);
    CHECK_EQ(int(other.count), 9);
    CHECK(cursor.empty());
}

TEST(nothing_goes_into_a_result_slot_and_armour_only_where_it_belongs)
{
    ItemStack result;
    ItemStack cursor = stack(id(Block::Dirt), 3);
    CHECK(!item::clickSlot(result, cursor, 0, SlotRule::TakeOnly).changed);
    CHECK(result.empty());
    CHECK_EQ(int(cursor.count), 3);

    const int helmetType = item::def(item::ItemId(Item::IronHelmet)).armour;
    const int bootsType = item::def(item::ItemId(Item::IronBoots)).armour;
    ItemStack head;
    ItemStack boots = stack(id(Item::IronBoots), 1);
    CHECK(!item::clickSlot(head, boots, 0, SlotRule::Armour, helmetType).changed);
    ItemStack feet;
    CHECK(item::clickSlot(feet, boots, 0, SlotRule::Armour, bootsType).changed);
    CHECK_EQ(int(feet.id), int(id(Item::IronBoots)));
}

TEST(a_different_item_on_the_cursor_swaps)
{
    ItemStack slot = stack(id(Block::Dirt), 7);
    ItemStack cursor = stack(id(Block::Stone), 2);
    item::clickSlot(slot, cursor, 0, SlotRule::Any);
    CHECK_EQ(int(slot.id), int(id(Block::Stone)));
    CHECK_EQ(int(slot.count), 2);
    CHECK_EQ(int(cursor.id), int(id(Block::Dirt)));
    CHECK_EQ(int(cursor.count), 7);
}

TEST(the_same_item_merges_up_to_its_own_stack_size)
{
    ItemStack slot = stack(id(Item::Stick), 60);
    ItemStack cursor = stack(id(Item::Stick), 10);
    item::clickSlot(slot, cursor, 0, SlotRule::Any);
    CHECK_EQ(int(slot.count), 64);
    CHECK_EQ(int(cursor.count), 6);

    // Right: one at a time.
    ItemStack small = stack(id(Item::Stick), 3);
    item::clickSlot(small, cursor, 1, SlotRule::Any);
    CHECK_EQ(int(small.count), 4);
    CHECK_EQ(int(cursor.count), 5);

    // A sword stacks to one: two swords do not merge.
    ItemStack sword = stack(id(Item::IronSword), 1);
    ItemStack otherSword = stack(id(Item::IronSword), 1);
    item::clickSlot(sword, otherSword, 0, SlotRule::Any);
    CHECK_EQ(int(sword.count), 1);
    CHECK_EQ(int(otherSword.count), 1);
}

TEST(a_result_tops_up_a_cursor_of_the_same_item_when_it_all_fits)
{
    ItemStack result = stack(id(Block::Torch), 4);
    ItemStack cursor = stack(id(Block::Torch), 4);
    const item::SlotClick click = item::clickSlot(result, cursor, 0, SlotRule::TakeOnly);
    CHECK(click.pickedUp);
    CHECK_EQ(int(cursor.count), 8);
    CHECK(result.empty());

    ItemStack more = stack(id(Block::Torch), 4);
    ItemStack full = stack(id(Block::Torch), 62);
    CHECK(!item::clickSlot(more, full, 0, SlotRule::TakeOnly).changed);
    CHECK_EQ(int(more.count), 4);
}

TEST(a_click_outside_throws_the_cursor_all_or_one)
{
    ItemStack cursor = stack(id(Block::Dirt), 5);
    ItemStack thrown;
    CHECK(item::clickOutside(cursor, 1, &thrown));
    CHECK_EQ(int(thrown.count), 1);
    CHECK_EQ(int(cursor.count), 4);
    CHECK(item::clickOutside(cursor, 0, &thrown));
    CHECK_EQ(int(thrown.count), 4);
    CHECK(cursor.empty());
    CHECK(!item::clickOutside(cursor, 0, &thrown));
}
