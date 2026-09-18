#pragma once

// **The container screen that is open** -- the model behind a1.1.2's four
// `ee` screens, without the drawing: `lo` the player's inventory with its 2 x 2
// grid, `hx` the workbench, `id` the furnace and `ea` a chest.
//
// Each screen is a list of slots in the constructor's own order, and a click
// names a slot by its place in that list:
//
//   | screen    | slots, in order                                              |
//   | Inventory | result, grid (4), armour (4, helmet first), backpack, hand   |
//   | Workbench | result, grid (9), backpack, hand                             |
//   | Furnace   | input, fuel, output, backpack, hand                          |
//   | Chest     | the chest (27 a part), backpack, hand                        |
//
// "Backpack" is the player's slots 9..35 and "hand" 0..8, which is how all four
// constructors add them. The rules for a click are core/item/container.hpp's;
// this decides which stack a click lands on and what else it sets off -- a grid
// recomputing its result, a result spending its ingredients, a furnace or a
// chest writing its tile entity back.
//
// **The tile entities are copied, not borrowed.** A furnace's and a chest's
// contents live in their column's list, which a block write can reorder under a
// held pointer -- the furnace's own lit/unlit swap does. So the session pulls a
// dense copy before every click and before drawing, and pushes it back after
// any click that changed it; nothing outlives a call. A missing entry is made
// on the spot, which is `ga.d(III)Lic;`'s own lazy heal for a container block
// placed since the last save.
//
// **Closing drops what is not the world's**: the stack on the cursor
// (`ar.a(Ldm;)V`) and whatever is in a crafting grid (`et`'s and `n`'s own
// override). A chest's and a furnace's contents stay in them.

#include "core/entity/minecart.hpp"
#include "core/item/container.hpp"
#include "core/item/crafting.hpp"
#include "core/item/inventory.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/furnace.hpp"
#include "core/util/types.hpp"
#include "core/world/tile_entity.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::item {

// **MinecartChest is a chest whose 27 slots are an entity's, not a cell's.**
// `oc` implements `IInventory` itself and `player.displayGUIChest(minecart)`
// opens `GuiChest` on the cart, so the screen, the click rules and the layout
// are a chest's exactly -- only where the stacks live differs. It is a kind of
// its own rather than a flag on `Chest` because every `pull` and `push` has to
// ask a different question, and a bool inside those would be read four times.
enum class ScreenKind : u8 { None, Inventory, Workbench, Furnace, Chest, MinecartChest };

inline constexpr int kMaxChestScreenSlots = tick::kMaxChestParts * world::kChestSlots;

// The player's 36, after whatever the screen's own slots are.
inline constexpr int kScreenPlayerSlots = kMainSlots;

class ContainerSession {
public:
    ScreenKind kind() const { return kind_; }
    bool isOpen() const { return kind_ != ScreenKind::None; }

    // `lo`. No world: the grid is the player's own.
    void openInventory();
    // `hx`, at the workbench's position.
    void openWorkbench(i32 x, int y, i32 z);
    // `id` and `ea`. False, and nothing opened, when the block is not the
    // container it should be.
    bool openFurnace(tick::TickWorld& world, i32 x, int y, i32 z);
    bool openChest(tick::TickWorld& world, i32 x, int y, i32 z);

    // **`player.displayGUIChest(minecart)`** -- a chest cart's own 27 slots,
    // addressed by the cart's stable id rather than by a cell. False, and
    // nothing opened, when that id is not a live chest cart.
    //
    // The system is borrowed and must outlive the screen, which on the console
    // it does: both belong to the world loop. `pull` re-reads through it and
    // closes the screen when the cart has gone -- run over, broken, or simply
    // out of a resident chunk -- which is the same answer `pull` gives a chest
    // whose block was blown up under it.
    bool openMinecartChest(entity::MinecartSystem& carts, u32 cartId);

    // Refreshes the copies from the tile entities -- or, for a chest cart, from
    // the cart. False when a block the screen is on, or the cart it is on, has
    // gone, which is the caller's cue to close.
    //
    // **That is a deviation, and a safe one.** a1.1.2's containers have no
    // `canInteractWith` at all -- `ar` has no such method -- so a chest blown
    // up under an open screen stays open on a tile entity the world has already
    // spilled, and every stack taken out of it after that is a copy. Closing
    // is the only answer that duplicates nothing.
    bool pull(tick::TickWorld* world);

    // True once after a pull found a stack different from the last one, which
    // is a furnace finishing an item: the caller's cue to redraw the slots.
    bool takeChanged()
    {
        const bool changed = changed_;
        changed_ = false;
        return changed;
    }

    // How many of the slots are the screen's own, and how many in all.
    int containerSlots() const;
    int slotCount() const { return containerSlots() + kScreenPlayerSlots; }

    // The stack a slot holds, for drawing.
    const ItemStack& slotAt(const Inventory& inventory, int index) const;

    // One click of `button` on slot `index`. `world` may be null for a screen
    // with no tile entity.
    SlotClick click(tick::TickWorld* world, Inventory& inventory, int index, int button);

    // A click outside the screen: see `item::clickOutside`.
    bool throwCursor(int button, ItemStack* thrown);

    // **A shift-click**, which is ours: a1.1.2's `GuiContainer.mouseClicked`
    // reads no modifier at all, and the move arrived in a later Beta. On a
    // console with no mouse the alternative is a pick-up and a put-down per
    // stack, and emptying a chest that way is most of an evening.
    //
    // Where the stack goes, which is the later game's routing:
    //
    //   * **The screen's own slots go to the player** -- the chest's, the
    //     furnace's, a crafting cell and the armour. A chest and a furnace
    //     output fill the hand from its last slot backwards, the other way
    //     round from a pick-up.
    //   * **A crafting result is crafted as many times as fits**, each whole
    //     result going in only if all of it has room, so nothing is left on the
    //     grid half-taken.
    //   * **The player's slots go into the chest**; into the furnace's input if
    //     they smelt and its fuel if they burn; onto the matching armour slot
    //     on the inventory screen. Anything with nowhere of that kind to go
    //     crosses between the backpack and the hand.
    //
    // A stack is topped up onto what is already there before an empty slot is
    // taken, under both the item's own stack size and the inventory's 64.
    // Nothing moves on to the cursor, so the cursor does not matter.
    SlotClick quickMove(tick::TickWorld* world, Inventory& inventory, int index);

    // Closes the screen and writes into `out` what goes on the ground. Returns
    // how many stacks that is -- at most ten, the cursor and a 3 x 3 grid.
    int close(ItemStack* out, int max);

    const ItemStack& cursor() const { return cursor_; }
    int chestRows() const { return chestParts_ * 3; }

    // `ke.a(I)I` and `ke.b(I)I` -- the arrow and the flame, scaled to `scale`
    // pixels, off the last pull.
    int furnaceCookScaled(int scale) const;
    int furnaceBurnScaled(int scale) const;

    // Where the screen's block is, for the caller's reach and residency checks.
    i32 x() const { return x_; }
    int y() const { return y_; }
    i32 z() const { return z_; }

    // The chest cart this screen is on, or 0. A caller that checks reach
    // against a block has to ask this first: a cart has no cell to check.
    u32 minecartChest() const { return cart_; }

private:
    // Where a slot index lands. Exactly one of `stack` or `playerSlot` is set.
    struct Resolved {
        ItemStack* stack = nullptr;
        int playerSlot = -1;
        SlotRule rule = SlotRule::Any;
        int armourType = -1;
        bool grid = false;
        bool result = false;
        bool tileBacked = false;
    };
    Resolved resolve(Inventory& inventory, int index);
    void push(tick::TickWorld* world);

    // `quickMove`'s two halves over screen slots `[first, end)`: how much of
    // `stack` would find room, and moving as much of `from` as does.
    int roomFor(Inventory& inventory, const ItemStack& stack, int first, int end);
    bool mergeInto(Inventory& inventory, ItemStack& from, int first, int end, bool reverse);

    ScreenKind kind_ = ScreenKind::None;
    bool changed_ = false;
    i32 x_ = 0;
    int y_ = 0;
    i32 z_ = 0;

    ItemStack cursor_;
    CraftingGrid grid_;
    ItemStack furnace_[tick::kFurnaceSlotCount];
    int furnaceBurn_ = 0;
    int furnaceCook_ = 0;
    int furnaceCurrentBurn_ = 0;

    tick::ChestPart parts_[tick::kMaxChestParts];
    int chestParts_ = 0;
    ItemStack chest_[kMaxChestScreenSlots];

    // The chest cart, when that is what is open. Borrowed; see
    // `openMinecartChest`.
    entity::MinecartSystem* carts_ = nullptr;
    u32 cart_ = 0;
};

}  // namespace mc::item
