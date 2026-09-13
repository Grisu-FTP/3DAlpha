#pragma once

// **Where the rows of a settings screen go**: a list of buttons in groups, a
// few pixels of extra space between one group and the next, and a window over
// it that scrolls to keep the cursor in view.
//
// The menu's Options and World Settings screens both used to be a fixed column
// of rows pitched to fill 240 pixels exactly, which is why Sound had to be a
// sub-screen and why a seventh row on either meant re-deriving every number.
// This is the arithmetic that replaced them, kept in core because none of it
// needs a screen and all of it is the kind of off-by-one a test catches.
//
// Rows are identified by index and grouped by a byte per row: two neighbours
// with the same byte sit one `pitch` apart, and two with different bytes sit
// `pitch + groupGap` apart. The window scrolls by whole rows, so a row is
// either drawn or it is not -- citro2d does not clip, and half a button hanging
// under the title would be drawn over it.

#include "core/util/types.hpp"

namespace mc::gui {

struct ListGeometry {
    int rowHeight = 22;  // one button, outline not included
    int pitch = 27;      // top of one row to the top of the next in a group
    int groupGap = 8;    // added to the pitch where the group changes
    int viewHeight = 180;
};

// The distance from the top of row 0 to the top of row `index`, gaps included.
int listRowOffset(const u8* groups, int index, const ListGeometry& geometry);

// The first row to draw so that `cursor` is wholly inside the window, moving
// `scroll` as little as possible -- and back up, when the list has room below
// its last row, so a window is never left half empty by a list that shrank.
int listScrollFor(const u8* groups, int count, int cursor, int scroll,
                  const ListGeometry& geometry);

// How many rows, starting at `scroll`, are wholly inside the window.
int listVisibleCount(const u8* groups, int count, int scroll, const ListGeometry& geometry);

// Pages of `perPage` lines it takes to show `lines` of them, for the tooltip
// under the list. Never zero: an empty tooltip is still one page.
int listPageCount(int lines, int perPage);

}  // namespace mc::gui
