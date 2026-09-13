#include "core/item/inventory.hpp"

#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"

#include <utility>

namespace mc::item {

namespace {

// The one empty stack every out-of-range read answers with. Static so `at` can
// return a reference without the caller having to know it is a special case.
const ItemStack kNothing{};

using Layout = mcver::ItemLayout;

}  // namespace

ItemId Inventory::selectedItem() const
{
    if (selected < 0 || selected >= kHotbarSlots) {
        return 0;
    }
    const ItemStack& stack = main[selected];
    return stack.empty() ? 0 : ItemId(stack.id);
}

u16 Inventory::selectedPlacesBlock() const
{
    const ItemId id = selectedItem();
    return id == 0 ? 0 : placesBlock(id);
}

namespace {

// `eu.e()I` -- getInventoryStackLimit. 64 in a1.1.2 and not a per-item number;
// the *item's* own maximum is the other half of the pair and is checked beside
// it.
constexpr int kInventoryStackLimit = 64;

}  // namespace

int Inventory::addStack(ItemId id, int count, i16 damage)
{
    if (id == 0 || count <= 0) {
        return 0;
    }

    // `b(II)I` -- storePartialItemStack, and it tries **one** slot rather than
    // walking the array: the slot already holding this item with room in it,
    // or, failing that, the first empty one. What it cannot fit comes back and
    // the caller below puts the remainder somewhere whole.
    if (damage == 0) {
        int slot = -1;
        for (int i = 0; i < kMainSlots; ++i) {
            const ItemStack& s = main[i];
            if (!s.empty() && s.id == i16(id) && int(s.count) < int(def(id).stack)
                && int(s.count) < kInventoryStackLimit) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            slot = firstEmptySlot();
        }
        if (slot < 0) {
            return count;
        }

        // **Two ceilings, and both are checked.** The item's own maximum --
        // 64 for most things, 1 for a door -- and the inventory's flat 64.
        // The original writes `new ItemStack(id, 0)` into an empty slot first
        // and then adds to it; the count is worked out here before anything is
        // written, which keeps a zero-count stack from ever existing.
        const int held = main[slot].empty() ? 0 : int(main[slot].count);
        int take = count;
        const int itemRoom = int(def(id).stack) - held;
        const int inventoryRoom = kInventoryStackLimit - held;
        take = take > itemRoom ? itemRoom : take;
        take = take > inventoryRoom ? inventoryRoom : take;
        if (take <= 0) {
            return count;
        }
        count -= take;
        set(slot, id, i8(held + take));
        if (count == 0) {
            return 0;
        }
    }

    // `j()` -- getFirstEmptyStack, and the whole remainder goes into it at
    // once. A count past the item's own maximum therefore *can* sit in a slot
    // here, exactly as it can in the original: the ceiling is enforced when
    // merging and not when a stack is dropped in whole.
    const int empty = firstEmptySlot();
    if (empty < 0) {
        return count;
    }
    set(empty, id, i8(count));
    main[empty].damage = damage;
    return 0;
}

int Inventory::firstEmptySlot() const
{
    for (int i = 0; i < kMainSlots; ++i) {
        if (main[i].empty()) {
            return i;
        }
    }
    return -1;
}

ItemId Inventory::dropOne()
{
    if (selected < 0 || selected >= kHotbarSlots) {
        return 0;
    }
    ItemStack& stack = main[selected];
    if (stack.empty()) {
        return 0;
    }

    const ItemId taken = ItemId(stack.id);
    // `if (itemStack.stackSize <= i) { setInventorySlotContents(j, null) }` --
    // the slot is emptied, not left holding a count of zero, and emptying it
    // has to clear the preserved tags with it for the reason `set` gives.
    if (stack.count <= 1) {
        set(selected, 0, 0);
    } else {
        --stack.count;
    }
    return taken;
}

bool Inventory::consumeOne(ItemId id)
{
    // `eu.f(I)I` finds the first main slot whose stack is that item, in index
    // order, and `eu.b(I)Z` takes one off it.
    for (int slot = 0; slot < kMainSlots; ++slot) {
        ItemStack& stack = main[slot];
        if (stack.empty() || stack.id != id) {
            continue;
        }
        if (stack.count <= 1) {
            set(slot, 0, 0);
        } else {
            --stack.count;
        }
        return true;
    }
    return false;
}

void Inventory::cycle(int delta)
{
    // `% kHotbarSlots` after adding the width keeps the intermediate positive
    // for any single-step delta, which is all the two shoulder buttons ever
    // send. A delta larger than nine is not a gesture anything makes.
    int next = (selected + delta) % kHotbarSlots;
    if (next < 0) {
        next += kHotbarSlots;
    }
    selected = next;
}

const ItemStack& Inventory::at(int slot) const
{
    if (Layout::isMain(slot)) {
        return main[slot];
    }
    if (Layout::isArmour(slot)) {
        return armour[Layout::armourIndex(slot)];
    }
    return kNothing;
}

void Inventory::set(int slot, ItemId id, i8 count)
{
    ItemStack* target = nullptr;
    if (Layout::isMain(slot)) {
        target = &main[slot];
    } else if (Layout::isArmour(slot)) {
        target = &armour[Layout::armourIndex(slot)];
    } else {
        return;
    }

    // Id 0 and a count of nothing both mean an empty slot, and emptying has to
    // clear the preserved tags with it -- a slot that keeps another stack's
    // unmodelled NBT would write it back attached to whatever lands there next.
    if (id == 0 || count <= 0) {
        *target = ItemStack{};
        target->slot = i8(slot);
        return;
    }

    target->id = i16(id);
    target->damage = 0;
    target->count = count;
    target->slot = i8(slot);
}

int Inventory::armourSlotFor(ItemId id)
{
    const i8 type = def(id).armour;
    if (type == ItemDef::kNotArmour) {
        return -1;
    }
    // Head-first to feet-first. See ItemDef::armour.
    return Layout::armourSlot(kArmourSlots - 1 - int(type));
}

bool Inventory::accepts(int slot, ItemId id)
{
    if (!Layout::isArmour(slot)) {
        return Layout::isMain(slot);
    }
    // **Both spellings of "nothing".** A caller reading `selectedItem()` gets 0
    // for an empty hand and a caller reading `at(slot).id` gets
    // `kEmptyItemId`, which is -1; an armour slot has to let go of what it is
    // holding whichever one it is asked with.
    return id <= 0 || armourSlotFor(id) == slot;
}

void Inventory::swap(int a, int b)
{
    if (a == b) {
        return;
    }
    const bool aOk = Layout::isMain(a) || Layout::isArmour(a);
    const bool bOk = Layout::isMain(b) || Layout::isArmour(b);
    if (!aOk || !bOk) {
        return;
    }

    // Each end has to be willing to hold what the other is carrying. Only the
    // armour slots ever say no, and only to the wrong piece.
    if (!accepts(a, at(b).id) || !accepts(b, at(a).id)) {
        return;
    }

    ItemStack& first = Layout::isMain(a) ? main[a] : armour[Layout::armourIndex(a)];
    ItemStack& second = Layout::isMain(b) ? main[b] : armour[Layout::armourIndex(b)];

    ItemStack held = first;
    first = second;
    second = held;
    // The slot number travels with the array position, not with the stack: it
    // is where the entry will be written, and swapping the contents must not
    // swap the addresses.
    first.slot = i8(a);
    second.slot = i8(b);
}

namespace {

int stackCeiling(ItemId id)
{
    const int own = int(def(id).stack);
    return own < kInventoryStackLimit ? own : kInventoryStackLimit;
}

// As much of `from` as fits into `slots[first, end)`: onto the matching stacks
// first, then into the empty ones. The slot numbers stay with the array
// positions, as they do in `swap`.
bool mergeInto(ItemStack* slots, int first, int end, ItemStack& from)
{
    if (from.empty()) {
        return false;
    }
    const i8 fromNumber = from.slot;
    const int ceiling = stackCeiling(ItemId(from.id));
    bool moved = false;
    for (int pass = 0; pass < 2 && !from.empty(); ++pass) {
        for (int i = first; i < end && !from.empty(); ++i) {
            ItemStack& slot = slots[i];
            if (&slot == &from) {
                continue;
            }
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
                const i8 number = slot.slot;
                if (int(from.count) <= ceiling) {
                    // Whole, so whatever tags the stack carries go with it.
                    slot = std::move(from);
                    from = ItemStack{};
                } else {
                    slot = ItemStack{};
                    slot.id = from.id;
                    slot.damage = from.damage;
                    slot.count = i8(ceiling);
                    from.count = i8(int(from.count) - ceiling);
                }
                slot.slot = number;
            }
            if (from.count <= 0) {
                from = ItemStack{};
            }
            from.slot = fromNumber;
            moved = true;
        }
    }
    return moved;
}

}  // namespace

bool Inventory::quickMove(int slot)
{
    if (Layout::isMain(slot)) {
        ItemStack& from = main[slot];
        if (from.empty()) {
            return false;
        }
        const int wornSlot = armourSlotFor(ItemId(from.id));
        if (wornSlot >= 0) {
            const int index = Layout::armourIndex(wornSlot);
            if (mergeInto(armour, index, index + 1, from)) {
                return true;
            }
        }
        return slot < kHotbarSlots ? mergeInto(main, kHotbarSlots, kMainSlots, from)
                                   : mergeInto(main, 0, kHotbarSlots, from);
    }
    if (Layout::isArmour(slot)) {
        ItemStack& from = armour[Layout::armourIndex(slot)];
        bool moved = mergeInto(main, kHotbarSlots, kMainSlots, from);
        if (!from.empty()) {
            moved = mergeInto(main, 0, kHotbarSlots, from) || moved;
        }
        return moved;
    }
    return false;
}

bool Inventory::giveStack(ItemId id)
{
    if (id <= 0 || def(id).stack <= 0) {
        return false;
    }
    ItemStack stack;
    stack.id = i16(id);
    stack.count = i8(stackCeiling(id));
    bool moved = mergeInto(main, 0, kHotbarSlots, stack);
    if (!stack.empty()) {
        moved = mergeInto(main, kHotbarSlots, kMainSlots, stack) || moved;
    }
    return moved;
}

void Inventory::fillHandFromPalette(int firstPaletteIndex)
{
    for (int i = 0; i < kHotbarSlots; ++i) {
        // paletteItem answers 0 for an index off the end, and `set` turns that
        // into an empty slot -- so a palette shorter than nine leaves the tail
        // blank instead of repeating itself.
        const ItemId id = paletteItem(firstPaletteIndex + i);
        set(i, id, id == 0 ? 0 : 1);
    }
}

void Inventory::clear()
{
    for (int i = 0; i < kMainSlots; ++i) {
        main[i] = ItemStack{};
        main[i].slot = i8(i);
    }
    for (int i = 0; i < kArmourSlots; ++i) {
        armour[i] = ItemStack{};
        armour[i].slot = i8(Layout::armourSlot(i));
    }
    unmodelled.clear();
    selected = 0;
}

void Inventory::load(const std::vector<ItemStack>& stacks)
{
    clear();
    for (const ItemStack& stack : stacks) {
        const int slot = stack.slot;
        if (Layout::isMain(slot)) {
            main[slot] = stack;
        } else if (Layout::isArmour(slot)) {
            armour[Layout::armourIndex(slot)] = stack;
        } else {
            // Kept rather than dropped. See the header note; the original drops
            // these and a real save in this project's reference world has one.
            unmodelled.push_back(stack);
        }
    }
}

void Inventory::save(std::vector<ItemStack>* stacks) const
{
    stacks->clear();
    // **In slot order, and the empty ones omitted**, which is what
    // `writeToNBT` produces: it walks the array and skips nulls, so the list a
    // real client writes is sparse and ascending. Matching that is what keeps a
    // world this build saved byte-comparable with one the original saved.
    for (int i = 0; i < kMainSlots; ++i) {
        if (!main[i].empty()) {
            stacks->push_back(main[i]);
            stacks->back().slot = i8(i);
        }
    }
    for (int i = 0; i < kArmourSlots; ++i) {
        if (!armour[i].empty()) {
            stacks->push_back(armour[i]);
            stacks->back().slot = i8(Layout::armourSlot(i));
        }
    }
    for (const ItemStack& stack : unmodelled) {
        stacks->push_back(stack);
    }
}

bool Inventory::empty() const
{
    for (int i = 0; i < kMainSlots; ++i) {
        if (!main[i].empty()) {
            return false;
        }
    }
    for (int i = 0; i < kArmourSlots; ++i) {
        if (!armour[i].empty()) {
            return false;
        }
    }
    return true;
}

}  // namespace mc::item
