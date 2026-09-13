// The generated tool table, through the three methods that read it.
//
// The table itself is the jar's answer and is checked for reproducibility by
// regenerating it; these cases pin how the port reads it -- which blocks need a
// tool, which tier reaches which ore, and the arithmetic `ly.a(Ldm;)F` does
// with the result.

#include "core/block/registry.hpp"
#include "core/item/tool_rules.hpp"
#include "framework.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

#include <cmath>

using namespace mc;
using mcver::Block;
using mcver::Item;

namespace {

block::BlockId b(Block id) { return block::BlockId(id); }
item::ItemId i(Item id) { return item::ItemId(id); }

}  // namespace

TEST(a_bare_hand_harvests_dirt_and_wood_but_not_stone_or_ore)
{
    CHECK(item::canHarvest(0, b(Block::Dirt)));
    CHECK(item::canHarvest(0, b(Block::Log)));
    CHECK(!item::canHarvest(0, b(Block::Stone)));
    CHECK(!item::canHarvest(0, b(Block::Cobblestone)));
    CHECK(!item::canHarvest(0, b(Block::IronOre)));
    CHECK(!item::canHarvest(0, b(Block::SnowBlock)));
    // A sword is no better at it: only the pickaxe and shovel override the test.
    CHECK(!item::canHarvest(i(Item::DiamondSword), b(Block::Stone)));
}

TEST(each_pickaxe_tier_reaches_the_ores_the_jar_gave_it)
{
    CHECK(item::canHarvest(i(Item::WoodenPickaxe), b(Block::Stone)));
    CHECK(item::canHarvest(i(Item::WoodenPickaxe), b(Block::CoalOre)));
    CHECK(!item::canHarvest(i(Item::WoodenPickaxe), b(Block::IronOre)));
    CHECK(item::canHarvest(i(Item::StonePickaxe), b(Block::IronOre)));
    CHECK(!item::canHarvest(i(Item::StonePickaxe), b(Block::GoldOre)));
    CHECK(!item::canHarvest(i(Item::StonePickaxe), b(Block::DiamondOre)));
    CHECK(item::canHarvest(i(Item::IronPickaxe), b(Block::DiamondOre)));
    CHECK(!item::canHarvest(i(Item::IronPickaxe), b(Block::Obsidian)));
    CHECK(item::canHarvest(i(Item::DiamondPickaxe), b(Block::Obsidian)));
    // Gold is wood's tier.
    CHECK(!item::canHarvest(i(Item::GoldenPickaxe), b(Block::IronOre)));
    // The shovel's two.
    CHECK(item::canHarvest(i(Item::WoodenShovel), b(Block::SnowBlock)));
    CHECK(!item::canHarvest(i(Item::WoodenShovel), b(Block::Stone)));
}

TEST(tool_speed_is_the_tier_on_its_own_blocks_and_one_elsewhere)
{
    CHECK_EQ(double(item::strVsBlock(0, b(Block::Stone))), 1.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::WoodenPickaxe), b(Block::Stone))), 2.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::StonePickaxe), b(Block::Stone))), 4.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::IronPickaxe), b(Block::Stone))), 6.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::DiamondPickaxe), b(Block::Stone))), 8.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::GoldenPickaxe), b(Block::Stone))), 2.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::IronPickaxe), b(Block::Dirt))), 1.0);
    CHECK_EQ(double(item::strVsBlock(i(Item::IronAxe), b(Block::Log))), 6.0);
    // A sword is a flat 1.5 against everything.
    CHECK_EQ(double(item::strVsBlock(i(Item::IronSword), b(Block::Dirt))), 1.5);
}

TEST(relative_hardness_is_the_jars_three_lines)
{
    // Harvestable: strength / hardness / 30, all in float.
    CHECK_EQ(double(item::relativeHardness(i(Item::WoodenPickaxe), b(Block::Stone), false, true)),
             double(2.0f / 1.5f / 30.0f));
    // Not harvestable: 1 / hardness / 100, whatever is held.
    CHECK_EQ(double(item::relativeHardness(0, b(Block::Stone), false, true)),
             double(1.0f / 1.5f / 100.0f));
    CHECK_EQ(double(item::relativeHardness(i(Item::DiamondSword), b(Block::Stone), false, true)),
             double(1.0f / 1.5f / 100.0f));
    // Bedrock never moves.
    CHECK_EQ(double(item::relativeHardness(i(Item::DiamondPickaxe), b(Block::Bedrock), false,
                                           true)),
             0.0);
    // A torch goes in one tick.
    CHECK(std::isinf(item::relativeHardness(0, b(Block::Torch), false, true)));
}

TEST(digging_under_water_or_off_the_ground_is_five_times_slower_each)
{
    const float dry = item::relativeHardness(0, b(Block::Dirt), false, true);
    CHECK_EQ(double(item::relativeHardness(0, b(Block::Dirt), true, true)),
             double((1.0f / 5.0f) / 0.5f / 30.0f));
    CHECK_EQ(double(item::relativeHardness(0, b(Block::Dirt), true, false)),
             double((1.0f / 5.0f / 5.0f) / 0.5f / 30.0f));
    CHECK(dry > item::relativeHardness(0, b(Block::Dirt), false, false));
    // The penalty does not reach a block the hand cannot harvest.
    CHECK_EQ(double(item::relativeHardness(0, b(Block::Stone), true, false)),
             double(1.0f / 1.5f / 100.0f));
}

TEST(a_tool_wears_by_one_on_a_block_and_a_sword_by_two)
{
    CHECK_EQ(item::wearOnBreak(i(Item::StonePickaxe)), 1);
    CHECK_EQ(item::wearOnHit(i(Item::StonePickaxe)), 2);
    CHECK_EQ(item::wearOnBreak(i(Item::StoneSword)), 2);
    CHECK_EQ(item::wearOnHit(i(Item::StoneSword)), 1);
    CHECK_EQ(item::wearOnBreak(0), 0);
    CHECK_EQ(item::wearOnBreak(i(Item::Stick)), 0);
}

TEST(food_heals_what_the_jar_constructed_it_with)
{
    CHECK_EQ(item::foodHeals(i(Item::Apple)), 4);
    CHECK_EQ(item::foodHeals(i(Item::Bread)), 5);
    CHECK_EQ(item::foodHeals(i(Item::CookedPorkchop)), 8);
    CHECK_EQ(item::foodHeals(i(Item::GoldenApple)), 42);
    CHECK_EQ(item::foodHeals(i(Item::Stick)), 0);
    CHECK_EQ(item::foodHeals(0), 0);
}
