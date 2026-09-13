// Where an open container screen's slots are on the bottom screen.

#include "core/gui/container_layout.hpp"
#include "core/item/container_session.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

using namespace mc;
using mc::gui::ContainerLayout;
using mc::gui::SlotRect;
using mc::item::ContainerSession;

namespace {

bool overlaps(const SlotRect& a, const SlotRect& b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Every slot is on the screen, none overlaps another, and each page slot is
// inside the panel -- for whichever screen is laid out.
void checkSane(const ContainerSession& session, const ContainerLayout& layout)
{
    CHECK_EQ(layout.slots, session.slotCount());
    const int handFirst = session.slotCount() - item::kHotbarSlots;
    for (int i = 0; i < layout.slots; ++i) {
        const SlotRect& r = layout.rect[i];
        if (r.w <= 0) {
            // A chest row scrolled out of the window, and the only thing
            // allowed to have no rectangle -- see the scroll test below.
            CHECK(i < session.containerSlots());
            continue;
        }
        CHECK(r.x >= 0 && r.x + r.w <= gui::kLayoutScreenWidth);
        if (i < handFirst) {
            CHECK(r.x >= layout.panel.x && r.x + r.w <= layout.panel.x + layout.panel.w);
            CHECK(r.y >= layout.panel.y && r.y + r.h <= layout.panel.y + layout.panel.h);
        } else {
            CHECK(r.y + r.h <= gui::kLayoutPageTop);
        }
        for (int j = i + 1; j < layout.slots; ++j) {
            if (layout.rect[j].w <= 0) {
                continue;
            }
            CHECK(!overlaps(r, layout.rect[j]));
        }
        // The slot's own middle hits the slot.
        CHECK_EQ(gui::containerSlotAt(layout, r.x + r.w / 2, r.y + r.h / 2), i);
    }
    if (layout.arrow.w > 0) {
        for (int i = 0; i < layout.slots; ++i) {
            CHECK(!overlaps(layout.arrow, layout.rect[i]));
        }
    }
}

}  // namespace

TEST(every_container_screen_lays_out_inside_the_page_band)
{
    ContainerSession inventory;
    inventory.openInventory();
    ContainerLayout layout;
    gui::buildContainerLayout(inventory, gui::kLayoutPageTop, &layout);
    checkSane(inventory, layout);

    ContainerSession workbench;
    workbench.openWorkbench(0, 64, 0);
    gui::buildContainerLayout(workbench, gui::kLayoutPageTop, &layout);
    checkSane(workbench, layout);
}

TEST(a_furnace_and_both_chests_lay_out_inside_the_page_band)
{
    constexpr i32 kX = 8;
    constexpr int kY = 64;
    constexpr i32 kZ = 8;
    ContainerLayout layout;

    test::SceneWorld furnaceScene(0, 0);
    furnaceScene.place(kX, kY, kZ, block::BlockId(mcver::Block::Furnace), 2);
    ContainerSession furnace;
    CHECK(furnace.openFurnace(furnaceScene.w(), kX, kY, kZ));
    gui::buildContainerLayout(furnace, gui::kLayoutPageTop, &layout);
    checkSane(furnace, layout);
    CHECK(layout.flame.w > 0 && layout.arrowIsProgress);

    test::SceneWorld chestScene(0, 0);
    chestScene.place(kX, kY, kZ, block::BlockId(mcver::Block::Chest), 0);
    ContainerSession single;
    CHECK(single.openChest(chestScene.w(), kX, kY, kZ));
    gui::buildContainerLayout(single, gui::kLayoutPageTop, &layout);
    checkSane(single, layout);

    chestScene.place(kX, kY, kZ + 1, block::BlockId(mcver::Block::Chest), 0);
    ContainerSession large;
    CHECK(large.openChest(chestScene.w(), kX, kY, kZ));
    CHECK_EQ(large.chestRows(), 6);
    gui::buildContainerLayout(large, gui::kLayoutPageTop, &layout);
    checkSane(large, layout);
}

// Three chests in a row, opened from the middle one -- which is nine rows of
// slots, and only reachable in a1.1.2 by placing the third into water. See
// tests/chest_test.cpp.
namespace {

void openTriple(test::SceneWorld& scene, ContainerSession* out)
{
    constexpr i32 kX = 8;
    constexpr int kY = 64;
    constexpr i32 kZ = 8;
    for (int d = -1; d <= 1; ++d) {
        scene.place(kX, kY, kZ + d, block::BlockId(mcver::Block::Chest), 0);
    }
    CHECK(out->openChest(scene.w(), kX, kY, kZ));
    CHECK_EQ(out->chestRows(), 9);
}

}  // namespace

TEST(a_chest_taller_than_the_band_scrolls_under_a_window)
{
    test::SceneWorld scene(0, 0);
    ContainerSession triple;
    openTriple(scene, &triple);

    ContainerLayout layout;
    gui::buildContainerLayout(triple, gui::kLayoutPageTop, &layout, 0);
    checkSane(triple, layout);
    CHECK_EQ(layout.chestRows, 9);
    CHECK_EQ(layout.chestWindowRows, 6);
    CHECK_EQ(layout.chestFirstRow, 0);

    // The window's own six rows are on the screen and the three below it are
    // not, and every index is where it always was.
    for (int i = 0; i < 6 * 9; ++i) {
        CHECK(layout.rect[i].w > 0);
    }
    for (int i = 6 * 9; i < 9 * 9; ++i) {
        CHECK_EQ(int(layout.rect[i].w), 0);
    }

    // A touch cannot land on a slot that is not drawn, and a d-pad step cannot
    // walk on to one either.
    for (int i = 0; i < 6 * 9; ++i) {
        for (int dy = -1; dy <= 1; ++dy) {
            const int next = gui::containerStep(layout, i, 0, dy);
            CHECK(next < 6 * 9 || next >= triple.containerSlots());
        }
    }

    // Scrolled to the bottom, the first three rows are the hidden ones.
    const int windowTop = layout.rect[0].y;
    gui::buildContainerLayout(triple, gui::kLayoutPageTop, &layout, 3);
    checkSane(triple, layout);
    CHECK_EQ(layout.chestFirstRow, 3);
    for (int i = 0; i < 3 * 9; ++i) {
        CHECK_EQ(int(layout.rect[i].w), 0);
    }
    for (int i = 3 * 9; i < 9 * 9; ++i) {
        CHECK(layout.rect[i].w > 0);
    }
    // The window does not move: row 3 is drawn where row 0 was.
    CHECK_EQ(int(layout.rect[3 * 9].y), windowTop);

    // Whatever it is handed is clamped, so the caller can hold a stale number.
    gui::buildContainerLayout(triple, gui::kLayoutPageTop, &layout, 99);
    CHECK_EQ(layout.chestFirstRow, 3);
    gui::buildContainerLayout(triple, gui::kLayoutPageTop, &layout, -5);
    CHECK_EQ(layout.chestFirstRow, 0);
}

TEST(the_scroll_bar_is_beside_the_grid_and_only_on_a_chest_that_scrolls)
{
    test::SceneWorld scene(0, 0);
    ContainerSession triple;
    openTriple(scene, &triple);

    ContainerLayout layout;
    gui::buildContainerLayout(triple, gui::kLayoutPageTop, &layout, 1);
    CHECK(layout.scrollUp.w > 0 && layout.scrollDown.w > 0);
    CHECK(layout.scrollTrack.w > 0 && layout.scrollThumb.w > 0);
    // In the gutter: clear of every slot, and inside the panel.
    for (const SlotRect* r : {&layout.scrollUp, &layout.scrollDown, &layout.scrollTrack}) {
        CHECK(r->x >= layout.panel.x && r->x + r->w <= layout.panel.x + layout.panel.w);
        CHECK(r->y >= layout.panel.y && r->y + r->h <= layout.panel.y + layout.panel.h);
        for (int i = 0; i < layout.slots; ++i) {
            CHECK(!overlaps(*r, layout.rect[i]));
        }
    }
    // The thumb is inside its track, and it moves down as the window does.
    CHECK(layout.scrollThumb.y >= layout.scrollTrack.y);
    CHECK(layout.scrollThumb.y + layout.scrollThumb.h
          <= layout.scrollTrack.y + layout.scrollTrack.h);
    const int middleThumb = layout.scrollThumb.y;
    gui::buildContainerLayout(triple, gui::kLayoutPageTop, &layout, 0);
    CHECK(layout.scrollThumb.y < middleThumb);

    // Touching an arrow is -1 up and 1 down, and nothing else on the screen is
    // either.
    CHECK_EQ(gui::containerScrollAt(layout, layout.scrollUp.x + 8, layout.scrollUp.y + 8), -1);
    CHECK_EQ(gui::containerScrollAt(layout, layout.scrollDown.x + 8, layout.scrollDown.y + 8), 1);
    CHECK_EQ(gui::containerScrollAt(layout, layout.rect[0].x, layout.rect[0].y), 0);

    // **A double chest does not scroll**, because it fits: six rows is exactly
    // what the band holds, which is why nothing needed this until a chest could
    // be joined to more than one other.
    test::SceneWorld pairScene(0, 0);
    pairScene.place(8, 64, 8, block::BlockId(mcver::Block::Chest), 0);
    pairScene.place(8, 64, 9, block::BlockId(mcver::Block::Chest), 0);
    ContainerSession large;
    CHECK(large.openChest(pairScene.w(), 8, 64, 8));
    gui::buildContainerLayout(large, gui::kLayoutPageTop, &layout, 0);
    CHECK_EQ(layout.chestRows, 6);
    CHECK_EQ(layout.chestWindowRows, 6);
    CHECK_EQ(int(layout.scrollUp.w), 0);
    CHECK_EQ(gui::containerScrollAt(layout, 160, 100), 0);
    for (int i = 0; i < layout.slots; ++i) {
        CHECK(layout.rect[i].w > 0);
    }
}

TEST(the_hand_is_the_hotbar_band)
{
    ContainerSession workbench;
    workbench.openWorkbench(0, 64, 0);
    ContainerLayout layout;
    gui::buildContainerLayout(workbench, gui::kLayoutPageTop, &layout);
    const int first = workbench.slotCount() - item::kHotbarSlots;
    // A touch on the band's first cell is hand slot 0, and on its last is 8.
    CHECK_EQ(gui::containerSlotAt(layout, 1, 10), first);
    CHECK_EQ(gui::containerSlotAt(layout, 319, 10), first + 8);
    // Between the band and the page is nothing.
    CHECK_EQ(gui::containerSlotAt(layout, 160, 40), -1);
}

TEST(the_dpad_walks_the_workbench_grid_by_position)
{
    ContainerSession workbench;
    workbench.openWorkbench(0, 64, 0);
    ContainerLayout layout;
    gui::buildContainerLayout(workbench, gui::kLayoutPageTop, &layout);
    // Cell (0,0) is slot 1; right is (1,0), down is (0,1).
    CHECK_EQ(gui::containerStep(layout, 1, 1, 0), 2);
    CHECK_EQ(gui::containerStep(layout, 1, 0, 1), 4);
    // Right off the grid's middle row is the result.
    CHECK_EQ(gui::containerStep(layout, 6, 1, 0), 0);
    // Left off the first column goes nowhere.
    CHECK_EQ(gui::containerStep(layout, 1, -1, 0), 1);
    // Down off the grid's bottom row lands in the backpack's top row.
    const int below = gui::containerStep(layout, 8, 0, 1);
    CHECK(below >= workbench.containerSlots()
          && below < workbench.containerSlots() + item::kHotbarSlots);
    // Up off the grid's top row is the hand, in the band above.
    CHECK(gui::containerStep(layout, 2, 0, -1) >= workbench.slotCount() - item::kHotbarSlots);
}
