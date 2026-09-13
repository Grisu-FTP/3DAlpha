#pragma once

// **What a held item does to a block and to a body**, which is the whole of
// how a1.1.2 decides how long a break takes, whether it drops anything, how a
// tool wears, and what eating something is worth.
//
// Every number comes from data/<version>/harvest.json, which
// `tools/genref.java --harvest` asked of a running jar: `di.a(Lev;Lly;)F`
// (getStrVsBlock), `di.a(Lly;)Z` (canHarvestBlock), what `hitEntity` and
// `hitBlock` add to a fresh stack's damage, `mr.aY` and `oj.a`. The formulas
// the numbers go into are three short methods and are transcribed below.
//
// `held` is an item id with **0 for an empty hand**, which is what
// `Inventory::selectedItem` answers -- and what `InventoryPlayer` does with a
// null slot, which is to skip the item's opinion and use the defaults.
//
// Named for the rules rather than `harvest.hpp`: a quoted include of that name
// from this directory would find this file before the generated table.

#include "core/block/block_def.hpp"
#include "core/item/item_def.hpp"
#include "core/util/types.hpp"

namespace mc::item {

// `dm.b(Lly;)Z` through `eu.b(Lly;)Z` -- canHarvestBlock. True outright for
// a block whose material is not one of the four the jar gates; otherwise only
// a held item whose own `canHarvestBlock` accepts the block -- the pickaxe
// and the shovel, by tier.
bool canHarvest(ItemId held, block::BlockId block);

// `eu.a(Lly;)F` -- how fast the hand digs, before the player's penalties. 1
// for an empty hand and for everything that is not a tool or a sword.
float strVsBlock(ItemId held, block::BlockId block);

// `dm.a(Lly;)F` -- `strVsBlock`, divided by five with the eye under water and
// by five again off the ground. Both penalties are the jar's; neither is
// removed by anything in this version.
float playerStrVsBlock(ItemId held, block::BlockId block, bool eyeInWater, bool onGround);

// `ly.a(Ldm;)F` -- getPlayerRelativeBlockHardness, the amount one tick of
// holding the button adds to a break:
//
// ```
// if (hardness < 0) return 0;                     // bedrock
// if (!player.canHarvestBlock(this)) return 1 / hardness / 100;
// return player.getCurrentPlayerStrVsBlock(this) / hardness / 30;
// ```
//
// A hardness of zero is infinity, which is what makes a torch go in one click.
float relativeHardness(ItemId held, block::BlockId block, bool eyeInWater, bool onGround);

// What `hitBlock` and `hitEntity` add to the held stack's damage: 1 and 2 for
// a tool, 2 and 1 for a sword, 0 for everything else.
int wearOnBreak(ItemId held);
int wearOnHit(ItemId held);

// `oj.a` -- the health an ItemFood gives back, or 0 for an item that is not
// food.
int foodHeals(ItemId held);

// **What an item's own right-click costs its stack in Survival**, once the item
// -- not the block it was aimed at -- took the click. Read off the item table's
// columns rather than listed, and checked against every class in the jar that
// writes `stackSize` or calls `damageItem` from a use method:
//
//   * `spendOnUse` is 1 for anything that puts a block down (`av`, `ec` the
//     door, `gf` sugar cane, `jn` seeds, `ef` redstone, `md` the sign) and for
//     the three that put an entity down (`od` the painting, `me` the boat,
//     `jo` the minecart). Each decrements on its success path and nowhere else.
//   * `wearOnUse` is 1 for the two that wear instead: `fu` the hoe and `nx`
//     flint and steel, which `damageItem(1)` on success.
//
// Food, the bow's arrow, the saddle and the bucket are not here: food spends on
// every right-click, the bow spends a *different* item, and the saddle and the
// bucket are items that turn into something else.
int spendOnUse(ItemId held);
int wearOnUse(ItemId held);

}  // namespace mc::item
