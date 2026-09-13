#include "core/gui/settings_list.hpp"

namespace mc::gui {

int listRowOffset(const u8* groups, int index, const ListGeometry& geometry)
{
    int offset = 0;
    for (int i = 1; i <= index; ++i) {
        offset += geometry.pitch;
        if (groups[i] != groups[i - 1]) {
            offset += geometry.groupGap;
        }
    }
    return offset;
}

namespace {

// Whether row `last` fits in the window when row `first` is at its top.
bool fits(const u8* groups, int first, int last, const ListGeometry& geometry)
{
    const int bottom = listRowOffset(groups, last, geometry) -
                       listRowOffset(groups, first, geometry) + geometry.rowHeight;
    return bottom <= geometry.viewHeight;
}

}  // namespace

int listScrollFor(const u8* groups, int count, int cursor, int scroll,
                  const ListGeometry& geometry)
{
    if (count <= 0) {
        return 0;
    }
    if (cursor < 0) {
        cursor = 0;
    }
    if (cursor >= count) {
        cursor = count - 1;
    }
    if (scroll > cursor) {
        scroll = cursor;
    }
    if (scroll < 0) {
        scroll = 0;
    }
    while (scroll < cursor && !fits(groups, scroll, cursor, geometry)) {
        ++scroll;
    }
    while (scroll > 0 && fits(groups, scroll - 1, count - 1, geometry)) {
        --scroll;
    }
    return scroll;
}

int listVisibleCount(const u8* groups, int count, int scroll, const ListGeometry& geometry)
{
    int visible = 0;
    for (int i = scroll; i < count; ++i) {
        if (!fits(groups, scroll, i, geometry)) {
            break;
        }
        ++visible;
    }
    return visible;
}

int listPageCount(int lines, int perPage)
{
    if (perPage < 1 || lines <= 0) {
        return 1;
    }
    return (lines + perPage - 1) / perPage;
}

}  // namespace mc::gui
