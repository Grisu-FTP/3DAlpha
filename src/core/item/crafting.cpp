// Shaped recipe matching over the generated list. See crafting.hpp.

#include "core/item/crafting.hpp"

#include "core/item/container.hpp"

#include "recipes.hpp"  // generated; see tools/configure.py

namespace mc::item {

namespace {

// `bv.a([IIIZ)Z` -- the recipe placed at (offsetX, offsetY), mirrored or not,
// against all nine cells.
bool matchesAt(const mcver::CraftingRecipe& recipe, const i16 ids[kCraftingCells], int offsetX,
               int offsetY, bool mirrored)
{
    for (int x = 0; x < 3; ++x) {
        for (int y = 0; y < 3; ++y) {
            const int rx = x - offsetX;
            const int ry = y - offsetY;
            int expected = -1;
            if (rx >= 0 && ry >= 0 && rx < recipe.width && ry < recipe.height) {
                expected = mirrored ? recipe.cells[recipe.width - rx - 1 + ry * recipe.width]
                                    : recipe.cells[rx + ry * recipe.width];
            }
            if (ids[x + y * 3] != expected) {
                return false;
            }
        }
    }
    return true;
}

// `bv.a([I)Z`.
bool matches(const mcver::CraftingRecipe& recipe, const i16 ids[kCraftingCells])
{
    for (int offsetX = 0; offsetX <= 3 - recipe.width; ++offsetX) {
        for (int offsetY = 0; offsetY <= 3 - recipe.height; ++offsetY) {
            if (matchesAt(recipe, ids, offsetX, offsetY, true)
                || matchesAt(recipe, ids, offsetX, offsetY, false)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

ItemStack matchRecipe(const i16 ids[kCraftingCells])
{
    for (int i = 0; i < mcver::kCraftingRecipeCount; ++i) {
        const mcver::CraftingRecipe& recipe = mcver::kCraftingRecipes[i];
        if (recipe.width == 0 || !matches(recipe, ids)) {
            continue;
        }
        ItemStack out;
        out.id = recipe.result;
        out.count = i8(recipe.count);
        out.damage = 0;
        return out;
    }
    return ItemStack{};
}

void refreshResult(CraftingGrid& grid)
{
    i16 ids[kCraftingCells];
    for (int x = 0; x < 3; ++x) {
        for (int y = 0; y < 3; ++y) {
            i16 id = -1;
            if (x < grid.width && y < grid.width) {
                const ItemStack& cell = grid.cells[x + y * grid.width];
                if (!cell.empty()) {
                    id = cell.id;
                }
            }
            ids[x + y * 3] = id;
        }
    }
    grid.result = matchRecipe(ids);
}

void consumeIngredients(CraftingGrid& grid)
{
    for (int i = 0; i < grid.size(); ++i) {
        ItemStack& cell = grid.cells[i];
        if (cell.empty()) {
            continue;
        }
        // `gu.a(II)Lev;` with a count of one.
        if (cell.count <= 1) {
            clearStack(cell);
        } else {
            cell.count = i8(int(cell.count) - 1);
        }
    }
    refreshResult(grid);
}

}  // namespace mc::item
