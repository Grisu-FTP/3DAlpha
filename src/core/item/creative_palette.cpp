#include "core/item/creative_palette.hpp"

#include "core/item/registry.hpp"

namespace mc::item {

// The table itself is generated into .rodata beside the item table -- see
// tools/configure.py's write_items -- so there is nothing to build here and
// nothing to initialise at startup. This file is the three questions asked of
// it.

int paletteSize() { return mcver::kPaletteSize; }

ItemId paletteItem(int index)
{
    if (index < 0 || index >= mcver::kPaletteSize) {
        return 0;
    }
    return mcver::kPalette[index];
}

int paletteIndexOf(ItemId id)
{
    // A scan over 66 entries, called when a slot is redrawn and never in the
    // per-frame path. A reverse table would be another 512 bytes of .rodata to
    // save a loop nobody is waiting on.
    for (int i = 0; i < mcver::kPaletteSize; ++i) {
        if (mcver::kPalette[i] == id) {
            return i;
        }
    }
    return -1;
}

}  // namespace mc::item
