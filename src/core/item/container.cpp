// `ee.a(III)V`'s slot rules. See container.hpp.

#include "core/item/container.hpp"

#include "core/item/registry.hpp"

#include <utility>

namespace mc::item {

namespace {

int maxStackOf(const ItemStack& stack) { return int(def(ItemId(stack.id)).stack); }

// `dk.a(Lev;)Z` for the three slot classes.
bool accepts(SlotRule rule, int armourType, const ItemStack& stack)
{
    switch (rule) {
    case SlotRule::Any:
        return true;
    case SlotRule::Armour:
        return def(ItemId(stack.id)).armour == armourType && armourType >= 0;
    case SlotRule::TakeOnly:
        return false;
    }
    return false;
}

// `gh.a(II)Lev;` -- decrStackSize, which every inventory here implements the
// same way except the crafting result: a stack no bigger than the request is
// handed over whole and the slot emptied, and a bigger one is split.
ItemStack decrStackSize(ItemStack& slot, int count, SlotRule rule)
{
    if (rule == SlotRule::TakeOnly || slot.count <= count) {
        ItemStack whole = std::move(slot);
        clearStack(slot);
        return whole;
    }
    return splitStack(slot, count);
}

}  // namespace

void clearStack(ItemStack& stack)
{
    stack.id = kEmptyItemId;
    stack.count = 0;
    stack.damage = 0;
    stack.preserved = nbt::PreservedTags{};
}

ItemStack splitStack(ItemStack& from, int count)
{
    ItemStack out;
    out.id = from.id;
    out.damage = from.damage;
    out.count = i8(count);
    from.count = i8(int(from.count) - count);
    return out;
}

SlotClick clickSlot(ItemStack& slot, ItemStack& cursor, int button, SlotRule rule,
                    int armourType, int limit)
{
    SlotClick out;
    if (button != 0 && button != 1) {
        return out;
    }
    const bool haveSlot = !slot.empty();
    const bool haveCursor = !cursor.empty();
    if (!haveSlot && !haveCursor) {
        return out;
    }

    // Pick up.
    if (haveSlot && !haveCursor) {
        const int count = button == 0 ? int(slot.count) : (int(slot.count) + 1) / 2;
        cursor = decrStackSize(slot, count, rule);
        if (slot.count == 0) {
            clearStack(slot);
        }
        out.changed = true;
        out.pickedUp = true;
        return out;
    }

    // Put down.
    if (!haveSlot && haveCursor) {
        if (!accepts(rule, armourType, cursor)) {
            return out;
        }
        int count = button == 0 ? int(cursor.count) : 1;
        if (count > limit) {
            count = limit;
        }
        slot = splitStack(cursor, count);
        if (cursor.count == 0) {
            clearStack(cursor);
        }
        out.changed = true;
        return out;
    }

    // Both full.
    if (accepts(rule, armourType, cursor)) {
        if (slot.id != cursor.id) {
            if (int(cursor.count) <= limit) {
                std::swap(slot, cursor);
                out.changed = true;
            }
            return out;
        }
        int count = button == 0 ? int(cursor.count) : 1;
        if (count > limit - int(slot.count)) {
            count = limit - int(slot.count);
        }
        if (count > maxStackOf(cursor) - int(slot.count)) {
            count = maxStackOf(cursor) - int(slot.count);
        }
        splitStack(cursor, count);
        if (cursor.count == 0) {
            clearStack(cursor);
        }
        slot.count = i8(int(slot.count) + count);
        out.changed = true;
        return out;
    }

    // A slot that will not take the cursor -- a crafting result -- topping the
    // cursor up with a stack of the same item.
    if (slot.id == cursor.id && maxStackOf(cursor) > 1) {
        const int count = slot.count;
        if (count > 0 && count + int(cursor.count) <= maxStackOf(cursor)) {
            cursor.count = i8(int(cursor.count) + count);
            splitStack(slot, count);
            if (slot.count == 0) {
                clearStack(slot);
            }
            out.changed = true;
            out.pickedUp = true;
        }
    }
    return out;
}

bool clickOutside(ItemStack& cursor, int button, ItemStack* thrown)
{
    if (cursor.empty() || thrown == nullptr) {
        return false;
    }
    if (button == 0) {
        *thrown = std::move(cursor);
        clearStack(cursor);
        return true;
    }
    if (button == 1) {
        *thrown = splitStack(cursor, 1);
        if (cursor.count == 0) {
            clearStack(cursor);
        }
        return true;
    }
    return false;
}

}  // namespace mc::item
