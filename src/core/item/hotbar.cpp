#include "core/item/hotbar.hpp"

#include "core/item/creative_palette.hpp"

namespace mc::item {

block::BlockId Hotbar::selectedBlock() const
{
    if (selected < 0 || selected >= kHotbarSlots) {
        return block::kAir;
    }
    const ItemStack& stack = slots[selected];
    return stack.empty() ? block::kAir : block::BlockId(stack.id);
}

void Hotbar::cycle(int delta)
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

void Hotbar::set(int slot, block::BlockId id)
{
    if (slot < 0 || slot >= kHotbarSlots) {
        return;
    }
    if (id == block::kAir) {
        slots[slot] = ItemStack{};
        slots[slot].slot = i8(slot);
        return;
    }
    slots[slot].id = i16(id);
    slots[slot].damage = 0;
    slots[slot].count = 1;
    slots[slot].slot = i8(slot);
}

void Hotbar::fillFromPalette(int firstPaletteIndex)
{
    for (int i = 0; i < kHotbarSlots; ++i) {
        // paletteBlock answers air for an index off the end, and `set` turns
        // that into an empty slot -- so a palette shorter than nine leaves the
        // tail blank instead of repeating itself.
        set(i, paletteBlock(firstPaletteIndex + i));
    }
}

}  // namespace mc::item
