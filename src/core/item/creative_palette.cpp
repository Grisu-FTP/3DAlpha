#include "core/item/creative_palette.hpp"

#include "core/block/registry.hpp"

namespace mc::item {

namespace {

// **Built at compile time, out of the generated table.** The alternative was a
// lazily-filled static, and this build has `-fno-threadsafe-statics`: a guard
// variable that two threads can race is not something to introduce for a table
// the compiler is perfectly able to fold. 512 bytes of .rodata and no
// initialiser runs at startup.
struct Palette {
    block::BlockId ids[mcver::kBlockTableSize];
    int count;
};

constexpr Palette buildPalette()
{
    Palette palette{};
    palette.count = 0;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        // Air is not a thing to hold, and `known` is false for every id this
        // version leaves empty -- sixteen of them in a1.1.2, ids 21..34 and 36.
        if (id == int(block::kAir) || !mcver::kBlocks[id].known) {
            continue;
        }
        palette.ids[palette.count++] = block::BlockId(id);
    }
    return palette;
}

constexpr Palette kPalette = buildPalette();

// The registry defines 71 ids including air, so the palette is 70. Asserted
// rather than commented, so a change to blocks.json that quietly drops a block
// fails the build of the file that depends on it.
static_assert(kPalette.count == mcver::kBlockCount - 1,
              "the palette is every known block but air");

}  // namespace

int paletteSize() { return kPalette.count; }

block::BlockId paletteBlock(int index)
{
    if (index < 0 || index >= kPalette.count) {
        return block::kAir;
    }
    return kPalette.ids[index];
}

int paletteIndexOf(block::BlockId id)
{
    // A scan over 70 entries, called when a slot is redrawn and never in the
    // per-frame path. A reverse table would be 512 more bytes of .rodata to
    // save a loop nobody is waiting on.
    for (int i = 0; i < kPalette.count; ++i) {
        if (kPalette.ids[i] == id) {
            return i;
        }
    }
    return -1;
}

}  // namespace mc::item
