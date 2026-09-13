// The open container screen's model. See container_session.hpp.

#include "core/item/container_session.hpp"

#include "core/block/registry.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"

#include <utility>

namespace mc::item {

namespace {

// The entry at a container block, made if it is missing -- `ga.d`'s lazy heal.
// Null when the block is not that container any more.
world::TileEntity* tileAt(tick::TickWorld& world, i32 x, int y, i32 z,
                          block::TickBehaviour behaviour, world::TileEntityKind kind)
{
    if (block::def(world.blockAt(x, y, z)).tick != behaviour) {
        return nullptr;
    }
    std::vector<world::TileEntity>* list = world.tileEntitiesAt(x, z);
    if (list == nullptr) {
        return nullptr;
    }
    world::TileEntity* tile = world::findTileEntity(*list, x, y, z);
    if (tile == nullptr || tile->kind != kind) {
        tile = &world::putTileEntity(*list, x, y, z, kind);
    }
    return tile;
}

bool sameStack(const ItemStack& a, const ItemStack& b)
{
    if (a.empty() || b.empty()) {
        return a.empty() == b.empty();
    }
    return a.id == b.id && a.count == b.count && a.damage == b.damage;
}

// Copies the tile entity's stacks over `into`, and says whether any differed.
bool pullStacks(const world::TileEntity& tile, ItemStack* into, int slots)
{
    ItemStack fresh[world::kChestSlots];
    tick::tileItems(tile, fresh, slots);
    bool differed = false;
    for (int i = 0; i < slots; ++i) {
        differed = differed || !sameStack(fresh[i], into[i]);
        into[i] = std::move(fresh[i]);
    }
    return differed;
}

}  // namespace

void ContainerSession::openInventory()
{
    kind_ = ScreenKind::Inventory;
    clearStack(cursor_);
    grid_ = CraftingGrid{};
    grid_.width = 2;
}

void ContainerSession::openWorkbench(i32 x, int y, i32 z)
{
    kind_ = ScreenKind::Workbench;
    x_ = x;
    y_ = y;
    z_ = z;
    clearStack(cursor_);
    grid_ = CraftingGrid{};
    grid_.width = 3;
}

bool ContainerSession::openFurnace(tick::TickWorld& world, i32 x, int y, i32 z)
{
    if (tileAt(world, x, y, z, block::TickBehaviour::Furnace, world::TileEntityKind::Furnace)
        == nullptr) {
        return false;
    }
    kind_ = ScreenKind::Furnace;
    x_ = x;
    y_ = y;
    z_ = z;
    clearStack(cursor_);
    return pull(&world);
}

bool ContainerSession::openChest(tick::TickWorld& world, i32 x, int y, i32 z)
{
    if (block::def(world.blockAt(x, y, z)).tick != block::TickBehaviour::Chest) {
        return false;
    }
    kind_ = ScreenKind::Chest;
    x_ = x;
    y_ = y;
    z_ = z;
    chestParts_ = tick::chestInventoryParts(world, x, y, z, parts_);
    clearStack(cursor_);
    return pull(&world);
}

bool ContainerSession::pull(tick::TickWorld* world)
{
    switch (kind_) {
    case ScreenKind::None:
        return false;
    case ScreenKind::Inventory:
        return true;
    case ScreenKind::Workbench:
        return world == nullptr
               || block::def(world->blockAt(x_, y_, z_)).tick == block::TickBehaviour::Workbench;
    case ScreenKind::Furnace: {
        if (world == nullptr) {
            return true;
        }
        world::TileEntity* tile = tileAt(*world, x_, y_, z_, block::TickBehaviour::Furnace,
                                         world::TileEntityKind::Furnace);
        if (tile == nullptr) {
            return false;
        }
        changed_ = pullStacks(*tile, furnace_, tick::kFurnaceSlotCount) || changed_;
        furnaceBurn_ = tile->burnTime;
        furnaceCook_ = tile->cookTime;
        furnaceCurrentBurn_ = tile->currentBurnTime;
        return true;
    }
    case ScreenKind::Chest: {
        if (world == nullptr) {
            return true;
        }
        for (int p = 0; p < chestParts_; ++p) {
            world::TileEntity* tile =
                tileAt(*world, parts_[p].x, parts_[p].y, parts_[p].z,
                       block::TickBehaviour::Chest, world::TileEntityKind::Chest);
            if (tile == nullptr) {
                return false;
            }
            changed_ = pullStacks(*tile, chest_ + p * world::kChestSlots, world::kChestSlots)
                       || changed_;
        }
        return true;
    }
    }
    return false;
}

void ContainerSession::push(tick::TickWorld* world)
{
    if (world == nullptr) {
        return;
    }
    if (kind_ == ScreenKind::Furnace) {
        if (world::TileEntity* tile = tileAt(*world, x_, y_, z_, block::TickBehaviour::Furnace,
                                             world::TileEntityKind::Furnace)) {
            tick::setTileItems(*tile, furnace_, tick::kFurnaceSlotCount);
            world->markTileEntityChanged(x_, z_);
        }
    } else if (kind_ == ScreenKind::Chest) {
        for (int p = 0; p < chestParts_; ++p) {
            if (world::TileEntity* tile =
                    tileAt(*world, parts_[p].x, parts_[p].y, parts_[p].z,
                           block::TickBehaviour::Chest, world::TileEntityKind::Chest)) {
                tick::setTileItems(*tile, chest_ + p * world::kChestSlots, world::kChestSlots);
                world->markTileEntityChanged(parts_[p].x, parts_[p].z);
            }
        }
    }
}

int ContainerSession::containerSlots() const
{
    switch (kind_) {
    case ScreenKind::None:
        return 0;
    case ScreenKind::Inventory:
        return 1 + 4 + kArmourSlots;
    case ScreenKind::Workbench:
        return 1 + 9;
    case ScreenKind::Furnace:
        return tick::kFurnaceSlotCount;
    case ScreenKind::Chest:
        return chestParts_ * world::kChestSlots;
    }
    return 0;
}

ContainerSession::Resolved ContainerSession::resolve(Inventory& inventory, int index)
{
    Resolved out;
    const int own = containerSlots();
    if (index < 0 || index >= own + kScreenPlayerSlots) {
        return out;
    }
    if (index >= own) {
        // Backpack first, then the hand -- every constructor's order.
        const int p = index - own;
        out.playerSlot = p < kBackpackSlots ? kHotbarSlots + p : p - kBackpackSlots;
        out.stack = &inventory.main[out.playerSlot];
        return out;
    }
    switch (kind_) {
    case ScreenKind::Inventory:
    case ScreenKind::Workbench: {
        const int gridCells = grid_.size();
        if (index == 0) {
            out.stack = &grid_.result;
            out.rule = SlotRule::TakeOnly;
            out.result = true;
        } else if (index <= gridCells) {
            out.stack = &grid_.cells[index - 1];
            out.grid = true;
        } else if (kind_ == ScreenKind::Inventory) {
            // `new lj(this, this, inv, inv.c() - 1 - i, 8, 8 + i * 18, i)`: row i
            // is armour type i -- helmet first -- held in `armorInventory[3 - i]`.
            const int i = index - 1 - gridCells;
            out.stack = &inventory.armour[kArmourSlots - 1 - i];
            out.rule = SlotRule::Armour;
            out.armourType = i;
        }
        break;
    }
    case ScreenKind::Furnace:
        out.stack = &furnace_[index];
        out.tileBacked = true;
        break;
    case ScreenKind::Chest:
        out.stack = &chest_[index];
        out.tileBacked = true;
        break;
    case ScreenKind::None:
        break;
    }
    return out;
}

const ItemStack& ContainerSession::slotAt(const Inventory& inventory, int index) const
{
    static const ItemStack kEmpty;
    // `resolve` is the one map from index to stack; a const call through it is
    // safe because nothing it returns is written here.
    Resolved where =
        const_cast<ContainerSession*>(this)->resolve(const_cast<Inventory&>(inventory), index);
    return where.stack != nullptr ? *where.stack : kEmpty;
}

SlotClick ContainerSession::click(tick::TickWorld* world, Inventory& inventory, int index,
                                  int button)
{
    SlotClick out;
    if (!isOpen()) {
        return out;
    }
    if (!pull(world)) {
        return out;
    }
    Resolved where = resolve(inventory, index);
    if (where.stack == nullptr) {
        return out;
    }
    out = clickSlot(*where.stack, cursor_, button, where.rule, where.armourType);
    if (!out.changed) {
        return out;
    }
    if (where.result && out.pickedUp) {
        // `an.a()` -- the result was taken, so the grid pays for it.
        consumeIngredients(grid_);
    } else if (where.grid) {
        // `gu.a(ILev;)V` telling its container.
        refreshResult(grid_);
    }
    if (where.tileBacked) {
        push(world);
    }
    return out;
}

namespace {

// How many crafts one press may take. A result is at least one item and the
// player holds at most 36 stacks of 64, so this is a guard, not a limit anyone
// reaches.
constexpr int kMaxQuickCrafts = 64 * kMainSlots;

int stackCeiling(const ItemStack& stack)
{
    const int own = int(def(ItemId(stack.id)).stack);
    return own < kInventoryStackLimit ? own : kInventoryStackLimit;
}

// Whether a slot of `rule` would ever hold `stack`. The result slot never does.
bool slotTakes(SlotRule rule, int armourType, const ItemStack& stack)
{
    switch (rule) {
    case SlotRule::Any:
        return true;
    case SlotRule::Armour:
        return armourType >= 0 && def(ItemId(stack.id)).armour == armourType;
    case SlotRule::TakeOnly:
        return false;
    }
    return false;
}

}  // namespace

int ContainerSession::roomFor(Inventory& inventory, const ItemStack& stack, int first, int end)
{
    const int ceiling = stackCeiling(stack);
    int room = 0;
    for (int index = first; index < end; ++index) {
        const Resolved where = resolve(inventory, index);
        if (where.stack == nullptr || !slotTakes(where.rule, where.armourType, stack)) {
            continue;
        }
        const ItemStack& slot = *where.stack;
        if (slot.empty()) {
            room += ceiling;
        } else if (slot.id == stack.id && int(slot.count) < ceiling) {
            room += ceiling - int(slot.count);
        }
    }
    return room;
}

bool ContainerSession::mergeInto(Inventory& inventory, ItemStack& from, int first, int end,
                                 bool reverse)
{
    if (from.empty() || first >= end) {
        return false;
    }
    const int ceiling = stackCeiling(from);
    bool moved = false;

    // Onto the stacks already there first, then into the empty slots -- two
    // walks in the same direction, so a hand filled from the right tops up from
    // the right as well.
    for (int pass = 0; pass < 2 && !from.empty(); ++pass) {
        for (int n = 0; n < end - first && !from.empty(); ++n) {
            const int index = reverse ? end - 1 - n : first + n;
            const Resolved where = resolve(inventory, index);
            if (where.stack == nullptr || where.stack == &from
                || !slotTakes(where.rule, where.armourType, from)) {
                continue;
            }
            ItemStack& slot = *where.stack;
            if (pass == 0) {
                if (slot.empty() || slot.id != from.id || int(slot.count) >= ceiling) {
                    continue;
                }
                int take = ceiling - int(slot.count);
                take = take < int(from.count) ? take : int(from.count);
                slot.count = i8(int(slot.count) + take);
                from.count = i8(int(from.count) - take);
            } else {
                if (!slot.empty()) {
                    continue;
                }
                if (int(from.count) <= ceiling) {
                    // Whole, so whatever tags the stack carries go with it.
                    slot = std::move(from);
                    clearStack(from);
                } else {
                    slot = splitStack(from, ceiling);
                }
            }
            if (from.count <= 0) {
                clearStack(from);
            }
            moved = true;
        }
    }
    return moved;
}

SlotClick ContainerSession::quickMove(tick::TickWorld* world, Inventory& inventory, int index)
{
    SlotClick out;
    if (!isOpen() || !pull(world)) {
        return out;
    }
    const Resolved where = resolve(inventory, index);
    if (where.stack == nullptr || where.stack->empty()) {
        return out;
    }

    const int own = containerSlots();
    const int handStart = own + kBackpackSlots;
    const int end = own + kScreenPlayerSlots;
    ItemStack& from = *where.stack;
    bool moved = false;

    if (where.result) {
        // `an.a()` once per craft, and only for a result the player has room
        // for in full.
        for (int n = 0; n < kMaxQuickCrafts && !grid_.result.empty(); ++n) {
            if (roomFor(inventory, grid_.result, own, end) < int(grid_.result.count)) {
                break;
            }
            ItemStack made = grid_.result;
            mergeInto(inventory, made, own, end, true);
            consumeIngredients(grid_);
            moved = true;
        }
    } else if (where.playerSlot < 0) {
        const bool fromTheRight =
            kind_ == ScreenKind::Chest
            || (kind_ == ScreenKind::Furnace && index == tick::kFurnaceOutputSlot);
        moved = mergeInto(inventory, from, own, end, fromTheRight);
    } else {
        const ItemId id = ItemId(from.id);
        if (kind_ == ScreenKind::Chest) {
            moved = mergeInto(inventory, from, 0, own, false);
        } else {
            if (kind_ == ScreenKind::Inventory && def(id).armour >= 0) {
                // `resolve`'s armour rows are helmet first, so the row is the
                // piece's own type.
                const int slot = 1 + grid_.size() + int(def(id).armour);
                moved = mergeInto(inventory, from, slot, slot + 1, false);
            } else if (kind_ == ScreenKind::Furnace) {
                if (tick::smeltingResult(id) >= 0) {
                    moved = mergeInto(inventory, from, tick::kFurnaceInputSlot,
                                      tick::kFurnaceInputSlot + 1, false);
                } else if (tick::fuelTicks(id) > 0) {
                    moved = mergeInto(inventory, from, tick::kFurnaceFuelSlot,
                                      tick::kFurnaceFuelSlot + 1, false);
                }
            }
            if (!moved) {
                moved = where.playerSlot < kHotbarSlots
                            ? mergeInto(inventory, from, own, handStart, false)
                            : mergeInto(inventory, from, handStart, end, false);
            }
        }
    }

    if (!moved) {
        return out;
    }
    out.changed = true;
    if (where.grid) {
        refreshResult(grid_);
    }
    if (kind_ == ScreenKind::Furnace || kind_ == ScreenKind::Chest) {
        push(world);
    }
    return out;
}

bool ContainerSession::throwCursor(int button, ItemStack* thrown)
{
    return clickOutside(cursor_, button, thrown);
}

int ContainerSession::close(ItemStack* out, int max)
{
    int count = 0;
    const auto take = [&](ItemStack& stack) {
        if (!stack.empty() && count < max) {
            out[count++] = std::move(stack);
        }
        clearStack(stack);
    };
    take(cursor_);
    if (kind_ == ScreenKind::Inventory || kind_ == ScreenKind::Workbench) {
        for (int i = 0; i < grid_.size(); ++i) {
            take(grid_.cells[i]);
        }
        clearStack(grid_.result);
    }
    kind_ = ScreenKind::None;
    chestParts_ = 0;
    return count;
}

int ContainerSession::furnaceCookScaled(int scale) const
{
    return furnaceCook_ * scale / tick::kFurnaceCookTicks;
}

int ContainerSession::furnaceBurnScaled(int scale) const
{
    const int current = furnaceCurrentBurn_ == 0 ? tick::kFurnaceCookTicks : furnaceCurrentBurn_;
    return furnaceBurn_ * scale / current;
}

}  // namespace mc::item
