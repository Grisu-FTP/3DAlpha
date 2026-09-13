// Crafting against CraftingManager's own list and `bv`'s matching.
//
// The recipe list is the jar's (data/<version>/recipes.json); these cases pin
// how it is matched -- offsets, mirroring, the 2 x 2 inside the 3 x 3, the
// blanks that must stay blank -- and what taking a result spends.

#include "core/item/crafting.hpp"
#include "core/item/item_stack.hpp"
#include "framework.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

using namespace mc;
using mc::item::CraftingGrid;
using mc::item::ItemStack;
using mcver::Block;
using mcver::Item;

namespace {

void put(CraftingGrid& grid, int x, int y, i16 id, int count = 1)
{
    ItemStack& cell = grid.cells[x + y * grid.width];
    cell.id = id;
    cell.count = i8(count);
    cell.damage = 0;
}

const i16 kPlanks = i16(Block::Planks);
const i16 kLog = i16(Block::Log);
const i16 kStick = i16(Item::Stick);

}  // namespace

TEST(a_log_anywhere_in_the_inventory_grid_makes_four_planks)
{
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            CraftingGrid grid;
            grid.width = 2;
            put(grid, x, y, kLog);
            item::refreshResult(grid);
            CHECK_EQ(int(grid.result.id), int(kPlanks));
            CHECK_EQ(int(grid.result.count), 4);
        }
    }
}

TEST(two_planks_stacked_make_sticks_in_either_column)
{
    CraftingGrid grid;
    grid.width = 2;
    put(grid, 1, 0, kPlanks);
    put(grid, 1, 1, kPlanks);
    item::refreshResult(grid);
    CHECK_EQ(int(grid.result.id), int(kStick));
    CHECK_EQ(int(grid.result.count), 4);
}

TEST(a_pickaxe_needs_the_workbench)
{
    CraftingGrid bench;
    bench.width = 3;
    put(bench, 0, 0, kPlanks);
    put(bench, 1, 0, kPlanks);
    put(bench, 2, 0, kPlanks);
    put(bench, 1, 1, kStick);
    put(bench, 1, 2, kStick);
    item::refreshResult(bench);
    CHECK_EQ(int(bench.result.id), int(Item::WoodenPickaxe));
}

TEST(an_axe_is_accepted_facing_either_way)
{
    // Straight: planks planks / planks stick / _ stick.
    CraftingGrid straight;
    put(straight, 0, 0, kPlanks);
    put(straight, 1, 0, kPlanks);
    put(straight, 0, 1, kPlanks);
    put(straight, 1, 1, kStick);
    put(straight, 1, 2, kStick);
    item::refreshResult(straight);
    CHECK_EQ(int(straight.result.id), int(Item::WoodenAxe));

    // Mirrored: planks planks / stick planks / stick _.
    CraftingGrid mirrored;
    put(mirrored, 0, 0, kPlanks);
    put(mirrored, 1, 0, kPlanks);
    put(mirrored, 0, 1, kStick);
    put(mirrored, 1, 1, kPlanks);
    put(mirrored, 0, 2, kStick);
    item::refreshResult(mirrored);
    CHECK_EQ(int(mirrored.result.id), int(Item::WoodenAxe));
}

TEST(a_recipe_matches_at_any_offset_that_fits)
{
    CraftingGrid grid;
    put(grid, 2, 1, kPlanks);
    put(grid, 2, 2, kPlanks);
    item::refreshResult(grid);
    CHECK_EQ(int(grid.result.id), int(kStick));
}

TEST(a_stray_item_outside_the_shape_spoils_the_match)
{
    CraftingGrid grid;
    put(grid, 0, 0, kPlanks);
    put(grid, 0, 1, kPlanks);
    put(grid, 2, 2, i16(Block::Dirt));
    item::refreshResult(grid);
    CHECK(grid.result.empty());
}

TEST(taking_the_result_spends_one_of_every_ingredient)
{
    CraftingGrid grid;
    grid.width = 2;
    put(grid, 0, 0, kLog, 2);
    item::refreshResult(grid);
    CHECK_EQ(int(grid.result.id), int(kPlanks));
    item::consumeIngredients(grid);
    CHECK_EQ(int(grid.cells[0].count), 1);
    CHECK_EQ(int(grid.result.id), int(kPlanks));
    item::consumeIngredients(grid);
    CHECK(grid.cells[0].empty());
    CHECK(grid.result.empty());
}
