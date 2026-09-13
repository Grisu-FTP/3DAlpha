// Which rows of a menu list the bottom-screen previews load next and which
// they give back. See core/preview/preview_window.hpp.

#include "framework.hpp"

#include "core/preview/preview_window.hpp"

using namespace mc;
using namespace mc::preview;

TEST(preview_window_loads_the_cursor_first_then_after_before_it)
{
    int out[8] = {};
    const int n = wantedOrder(5, 20, 2, out, 8);
    CHECK_EQ(n, 5);
    CHECK_EQ(out[0], 5);
    CHECK_EQ(out[1], 6);
    CHECK_EQ(out[2], 4);
    CHECK_EQ(out[3], 7);
    CHECK_EQ(out[4], 3);
}

TEST(preview_window_at_the_ends_of_a_list_only_names_rows_that_exist)
{
    int out[8] = {};
    CHECK_EQ(wantedOrder(0, 20, 2, out, 8), 3);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 1);
    CHECK_EQ(out[2], 2);

    CHECK_EQ(wantedOrder(1, 2, 3, out, 8), 2);
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 0);

    CHECK_EQ(wantedOrder(0, 1, 3, out, 8), 1);
    CHECK_EQ(wantedOrder(0, 0, 3, out, 8), 0);
    // A cursor past the end is clamped rather than trusted.
    CHECK_EQ(wantedOrder(9, 3, 0, out, 8), 1);
    CHECK_EQ(out[0], 2);
}

TEST(preview_window_never_writes_past_the_buffer)
{
    int out[3] = {-7, -7, -7};
    CHECK_EQ(wantedOrder(10, 100, 10, out, 3), 3);
    CHECK_EQ(out[2], 9);
}

TEST(preview_window_gives_back_the_furthest_row_outside_the_window)
{
    const int resident[] = {4, -1, 12, 1, 6};
    CHECK_EQ(farthestOutside(resident, 5, 5, 2), 2);   // row 12, seven away
    const int inside[] = {4, 5, -1, 7};
    CHECK_EQ(farthestOutside(inside, 4, 5, 2), -1);
    CHECK(inWindow(5, 3, 2));
    CHECK(!inWindow(5, 8, 2));
}
