// `ke.b()`, `ke.j()`, `ke.i()` and `ku.a`. See furnace.hpp.

#include "core/tick/furnace.hpp"

#include "core/item/container.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/tile_entity.hpp"

#include "blocks.hpp"   // generated; see tools/configure.py
#include "recipes.hpp"  // generated; see tools/configure.py

#include <utility>

namespace mc::tick {

int smeltingResult(item::ItemId input)
{
    for (int i = 0; i < mcver::kSmeltingRuleCount; ++i) {
        if (mcver::kSmeltingRules[i].input == input) {
            return mcver::kSmeltingRules[i].result;
        }
    }
    return -1;
}

int fuelTicks(item::ItemId item)
{
    if (item <= 0) {
        return 0;
    }
    for (int i = 0; i < mcver::kFuelRuleCount; ++i) {
        if (mcver::kFuelRules[i].item == item) {
            return mcver::kFuelRules[i].ticks;
        }
    }
    return 0;
}

void tileItems(const world::TileEntity& tile, item::ItemStack* out, int slots)
{
    for (int i = 0; i < slots; ++i) {
        item::clearStack(out[i]);
    }
    for (const item::ItemStack& stack : tile.items) {
        if (stack.slot >= 0 && stack.slot < slots && !stack.empty()) {
            out[stack.slot] = stack;
        }
    }
}

void setTileItems(world::TileEntity& tile, const item::ItemStack* in, int slots)
{
    tile.items.clear();
    for (int i = 0; i < slots; ++i) {
        if (in[i].empty()) {
            continue;
        }
        tile.items.push_back(in[i]);
        tile.items.back().slot = i8(i);
    }
}

bool furnaceCanSmelt(const item::ItemStack slots[kFurnaceSlotCount])
{
    const item::ItemStack& input = slots[kFurnaceInputSlot];
    const item::ItemStack& output = slots[kFurnaceOutputSlot];
    if (input.empty()) {
        return false;
    }
    const int result = smeltingResult(item::ItemId(input.id));
    if (result < 0) {
        return false;
    }
    if (output.empty()) {
        return true;
    }
    if (output.id != result) {
        return false;
    }
    // `a[2].a < e() && a[2].a < a[2].c()` then `a[2].a < Item.itemsList[i].b()`.
    if (int(output.count) < item::kInventoryStackLimit
        && int(output.count) < int(item::def(item::ItemId(output.id)).stack)) {
        return true;
    }
    return int(output.count) < int(item::def(item::ItemId(result)).stack);
}

void furnaceSmelt(item::ItemStack slots[kFurnaceSlotCount])
{
    if (!furnaceCanSmelt(slots)) {
        return;
    }
    const int result = smeltingResult(item::ItemId(slots[kFurnaceInputSlot].id));
    item::ItemStack& output = slots[kFurnaceOutputSlot];
    if (output.empty()) {
        output.id = i16(result);
        output.count = 1;
        output.damage = 0;
    } else if (output.id == result) {
        output.count = i8(int(output.count) + 1);
    }
    item::ItemStack& input = slots[kFurnaceInputSlot];
    input.count = i8(int(input.count) - 1);
    if (input.count <= 0) {
        item::clearStack(input);
    }
}

bool furnaceTickAt(TickWorld& world, i32 x, int y, i32 z)
{
    std::vector<world::TileEntity>* list = world.tileEntitiesAt(x, z);
    if (list == nullptr) {
        return false;
    }
    world::TileEntity* tile = world::findTileEntity(*list, x, y, z);
    if (tile == nullptr || tile->kind != world::TileEntityKind::Furnace) {
        return false;
    }

    item::ItemStack slots[kFurnaceSlotCount];
    tileItems(*tile, slots, kFurnaceSlotCount);
    int burn = tile->burnTime;
    int cook = tile->cookTime;
    int current = tile->currentBurnTime;

    const bool wasBurning = burn > 0;
    bool changed = false;
    bool itemsChanged = false;
    if (burn > 0) {
        --burn;
    }
    if (burn == 0 && furnaceCanSmelt(slots)) {
        item::ItemStack& fuel = slots[kFurnaceFuelSlot];
        burn = fuelTicks(fuel.empty() ? item::ItemId(0) : item::ItemId(fuel.id));
        current = burn;
        if (burn > 0) {
            changed = true;
            if (!fuel.empty()) {
                fuel.count = i8(int(fuel.count) - 1);
                if (fuel.count == 0) {
                    item::clearStack(fuel);
                }
                itemsChanged = true;
            }
        }
    }
    if (burn > 0 && furnaceCanSmelt(slots)) {
        ++cook;
        if (cook == kFurnaceCookTicks) {
            cook = 0;
            furnaceSmelt(slots);
            changed = true;
            itemsChanged = true;
        }
    } else {
        cook = 0;
    }

    tile->burnTime = i16(burn);
    tile->cookTime = i16(cook);
    tile->currentBurnTime = i16(current);
    if (itemsChanged) {
        setTileItems(*tile, slots, kFurnaceSlotCount);
    }

    const bool burning = burn > 0;
    if (wasBurning != burning) {
        changed = true;
        // `ku.a(ZLcn;III)V`: the block, then the metadata it had, then the
        // same tile entity -- because the write both re-orients the mouth
        // (`onBlockAdded`) and drops the entry (`onBlockRemoval`).
        world::TileEntity saved = std::move(*tile);
        const u8 metadata = world.dataAt(x, y, z);
        world.setBlockWithNotify(
            x, y, z,
            block::BlockId(burning ? mcver::Block::LitFurnace : mcver::Block::Furnace));
        world.setDataRaw(x, y, z, metadata);
        if (std::vector<world::TileEntity>* again = world.tileEntitiesAt(x, z)) {
            world::TileEntity& put =
                world::putTileEntity(*again, x, y, z, world::TileEntityKind::Furnace);
            put = std::move(saved);
        }
    }
    return changed;
}

}  // namespace mc::tick
