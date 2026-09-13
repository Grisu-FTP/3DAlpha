#pragma once

// **The furnace** -- TileEntityFurnace (`ke`), the one tile entity in the
// column's own list that ticks, and BlockFurnace's lit/unlit swap (`ku.a`).
//
// `ke.b()` a tick, in its order:
//
//   * a burning furnace spends a tick of its fire;
//   * a cold one with something it can smelt takes its fuel -- the burn time
//     and `currentItemBurnTime` both become the fuel's value, and one item of
//     fuel is used up if there was any;
//   * while it burns and can smelt, the cook clock runs, and at 200 the input
//     becomes one of its result; otherwise the clock is reset to zero;
//   * and if it was burning and is not, or the other way round, the block is
//     swapped between the furnace and the lit furnace **keeping its metadata
//     and its tile entity** -- `setBlockWithNotify` would re-orient the mouth
//     and drop the entry, so both are put back, which is `ku.a`'s own order.
//
// The smelting results and burn times are the jar's, asked of `ke.d(I)I` and
// `ke.a(Lev;)I` for every id by `tools/genref.java --recipes`. Two consequences
// fall out of them and are not bugs: **a lava bucket burns as a whole item and
// leaves no bucket**, because the fuel slot is decremented like any other; and
// **the output slot is an ordinary slot** on the screen, so a player can put
// anything into it and a furnace whose output holds the wrong item stops.

#include "core/item/item_def.hpp"
#include "core/item/item_stack.hpp"
#include "core/util/types.hpp"

namespace mc::world {
struct TileEntity;
}

namespace mc::tick {

class TickWorld;

inline constexpr int kFurnaceInputSlot = 0;
inline constexpr int kFurnaceFuelSlot = 1;
inline constexpr int kFurnaceOutputSlot = 2;
inline constexpr int kFurnaceSlotCount = 3;

// `if (d == 200)`.
inline constexpr int kFurnaceCookTicks = 200;

// `ke.d(I)I` -- what an input smelts into, or -1.
int smeltingResult(item::ItemId input);

// `ke.a(Lev;)I` -- how many ticks an item burns for, or 0.
int fuelTicks(item::ItemId item);

// The sparse `Items` list as a dense array and back. Slots past `slots` are
// ignored on the way in and never written on the way out.
void tileItems(const world::TileEntity& tile, item::ItemStack* out, int slots);
void setTileItems(world::TileEntity& tile, const item::ItemStack* in, int slots);

// `ke.j()` and `ke.i()` over the three slots.
bool furnaceCanSmelt(const item::ItemStack slots[kFurnaceSlotCount]);
void furnaceSmelt(item::ItemStack slots[kFurnaceSlotCount]);

// `ke.b()` for the furnace at a position, found by that position in the
// column's list. Returns true when `onInventoryChanged` would have run -- fuel
// taken, an item smelted, the fire lit or out -- which is when the column wants
// saving.
bool furnaceTickAt(TickWorld& world, i32 x, int y, i32 z);

}  // namespace mc::tick
