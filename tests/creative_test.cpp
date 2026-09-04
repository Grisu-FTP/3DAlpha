// Creative's two pieces of core state: the block palette and the hotbar.
//
// **There is no oracle here and there cannot be one.** a1.1.2 has no Creative
// mode, so nothing in this file compares against a real client the way
// player_body_test.cpp and ray_trace_test.cpp do. What it checks instead is
// that the palette really is *derived* -- that it is the registry's own set of
// known blocks and not a list somebody typed -- and that the hotbar's index
// arithmetic survives the wrap-around it is going to be driven with.

#include "core/block/registry.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/hotbar.hpp"
#include "framework.hpp"

using namespace mc;
using mc::item::Hotbar;
using mc::item::kHotbarSlots;
using mc::item::paletteBlock;
using mc::item::paletteIndexOf;
using mc::item::paletteSize;

TEST(palette_is_every_known_block_but_air)
{
    // Counted off the registry rather than written down: the point of the
    // palette is that adding a block to blocks.json adds it here.
    int known = 0;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (id != int(block::kAir) && block::def(block::BlockId(id)).known) {
            ++known;
        }
    }
    CHECK_EQ(paletteSize(), known);
    CHECK(paletteSize() > 0);
}

TEST(palette_is_in_id_order_and_holds_no_air)
{
    int previous = -1;
    for (int i = 0; i < paletteSize(); ++i) {
        const block::BlockId id = paletteBlock(i);
        CHECK(id != block::kAir);
        // Known, so a palette cell can never draw the unknown-block placeholder.
        CHECK(block::def(id).known);
        CHECK(int(id) > previous);
        previous = int(id);
    }
}

TEST(palette_index_round_trips)
{
    for (int i = 0; i < paletteSize(); ++i) {
        CHECK_EQ(paletteIndexOf(paletteBlock(i)), i);
    }
    // Air is not in it, and neither is an id this version does not define.
    CHECK_EQ(paletteIndexOf(block::kAir), -1);
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (!block::def(block::BlockId(id)).known) {
            CHECK_EQ(paletteIndexOf(block::BlockId(id)), -1);
        }
    }
}

TEST(palette_answers_air_off_the_end)
{
    // The UI's page arithmetic runs off the end of the last page by design --
    // a page is a fixed grid and the palette does not divide evenly into it --
    // so this is the ordinary case rather than the error case.
    CHECK_EQ(paletteBlock(-1), block::kAir);
    CHECK_EQ(paletteBlock(paletteSize()), block::kAir);
    CHECK_EQ(paletteBlock(paletteSize() + 1000), block::kAir);
}

TEST(hotbar_starts_empty_and_fills_from_the_palette)
{
    Hotbar bar;
    CHECK_EQ(bar.selected, 0);
    CHECK_EQ(bar.selectedBlock(), block::kAir);

    bar.fillFromPalette();
    for (int i = 0; i < kHotbarSlots; ++i) {
        CHECK_EQ(block::BlockId(bar.slots[i].id), paletteBlock(i));
        CHECK_EQ(int(bar.slots[i].count), 1);
        CHECK_EQ(int(bar.slots[i].slot), i);
    }
    CHECK_EQ(bar.selectedBlock(), paletteBlock(0));
}

TEST(hotbar_selection_wraps_both_ways)
{
    Hotbar bar;
    bar.fillFromPalette();

    bar.cycle(-1);
    CHECK_EQ(bar.selected, kHotbarSlots - 1);
    CHECK_EQ(bar.selectedBlock(), paletteBlock(kHotbarSlots - 1));

    bar.cycle(1);
    CHECK_EQ(bar.selected, 0);

    for (int i = 0; i < kHotbarSlots; ++i) {
        CHECK_EQ(bar.selected, i);
        bar.cycle(1);
    }
    CHECK_EQ(bar.selected, 0);
}

TEST(hotbar_set_replaces_and_air_clears)
{
    Hotbar bar;
    bar.fillFromPalette();

    const block::BlockId last = paletteBlock(paletteSize() - 1);
    bar.set(4, last);
    CHECK_EQ(block::BlockId(bar.slots[4].id), last);

    bar.selected = 4;
    CHECK_EQ(bar.selectedBlock(), last);

    // Air empties the slot rather than storing air in it, so "pick the empty
    // cell" is how a slot is cleared.
    bar.set(4, block::kAir);
    CHECK(bar.slots[4].empty());
    CHECK_EQ(bar.selectedBlock(), block::kAir);

    // Out of range is a no-op, not a write past the array.
    bar.set(-1, last);
    bar.set(kHotbarSlots, last);
    for (int i = 0; i < kHotbarSlots; ++i) {
        CHECK(i == 4 || !bar.slots[i].empty());
    }
}

TEST(hotbar_fill_past_the_palette_leaves_the_tail_empty)
{
    Hotbar bar;
    bar.fillFromPalette(paletteSize() - 3);
    CHECK(!bar.slots[0].empty());
    CHECK(!bar.slots[1].empty());
    CHECK(!bar.slots[2].empty());
    for (int i = 3; i < kHotbarSlots; ++i) {
        CHECK(bar.slots[i].empty());
    }
}
