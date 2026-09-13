// The grouped, scrolling list the menu's Options and World Settings screens
// lay their rows out with. See core/gui/settings_list.hpp.

#include "framework.hpp"

#include "core/gui/settings_list.hpp"

using namespace mc;
using namespace mc::gui;

namespace {

ListGeometry geometry(int viewHeight)
{
    ListGeometry g;
    g.rowHeight = 20;
    g.pitch = 25;
    g.groupGap = 10;
    g.viewHeight = viewHeight;
    return g;
}

}  // namespace

TEST(settings_rows_in_one_group_are_one_pitch_apart_and_a_new_group_adds_the_gap)
{
    const u8 groups[] = {0, 0, 1, 1, 2};
    const ListGeometry g = geometry(1000);
    CHECK_EQ(listRowOffset(groups, 0, g), 0);
    CHECK_EQ(listRowOffset(groups, 1, g), 25);
    CHECK_EQ(listRowOffset(groups, 2, g), 60);
    CHECK_EQ(listRowOffset(groups, 3, g), 85);
    CHECK_EQ(listRowOffset(groups, 4, g), 120);
}

TEST(a_settings_list_that_fits_never_scrolls)
{
    const u8 groups[] = {0, 0, 1, 1, 2};
    const ListGeometry g = geometry(140);  // last row ends at 140 exactly
    for (int cursor = 0; cursor < 5; ++cursor) {
        CHECK_EQ(listScrollFor(groups, 5, cursor, 0, g), 0);
    }
    CHECK_EQ(listVisibleCount(groups, 5, 0, g), 5);
}

TEST(moving_down_past_the_window_scrolls_by_the_least_that_shows_the_cursor)
{
    const u8 groups[] = {0, 0, 0, 1, 1, 1, 2, 3};
    const ListGeometry g = geometry(100);  // rows 0..3 fit: 0, 25, 50, 85 + 20 > 100
    CHECK_EQ(listVisibleCount(groups, 8, 0, g), 3);
    CHECK_EQ(listScrollFor(groups, 8, 2, 0, g), 0);
    // Row 3 is 85..105 from row 0; from row 1 it is 60..80.
    CHECK_EQ(listScrollFor(groups, 8, 3, 0, g), 1);
    // Moving back up inside the window leaves it alone.
    CHECK_EQ(listScrollFor(groups, 8, 2, 1, g), 1);
    // Moving above it follows the cursor.
    CHECK_EQ(listScrollFor(groups, 8, 0, 1, g), 0);
}

TEST(wrapping_from_the_top_to_the_last_row_shows_the_end_of_the_settings_list)
{
    const u8 groups[] = {0, 0, 0, 1, 1, 1, 2, 3};
    const ListGeometry g = geometry(100);
    const int scroll = listScrollFor(groups, 8, 7, 0, g);
    CHECK(listRowOffset(groups, 7, g) - listRowOffset(groups, scroll, g) + g.rowHeight <= 100);
    CHECK_EQ(scroll + listVisibleCount(groups, 8, scroll, g), 8);
    // And back to the top again.
    CHECK_EQ(listScrollFor(groups, 8, 0, scroll, g), 0);
}

TEST(a_settings_list_that_got_shorter_scrolls_back_to_fill_the_window)
{
    // In game World Settings has four rows where the home screen has seven.
    const u8 groups[] = {0, 1, 1, 3};
    const ListGeometry g = geometry(1000);
    CHECK_EQ(listScrollFor(groups, 4, 3, 3, g), 0);
}

TEST(a_settings_tooltip_is_never_zero_pages)
{
    CHECK_EQ(listPageCount(0, 16), 1);
    CHECK_EQ(listPageCount(16, 16), 1);
    CHECK_EQ(listPageCount(17, 16), 2);
    CHECK_EQ(listPageCount(5, 0), 1);
}
