// Where an open container screen's slots are. See container_layout.hpp.

#include "core/gui/container_layout.hpp"

#include "core/tick/furnace.hpp"
#include "core/world/tile_entity.hpp"

#include <cstdlib>

namespace mc::gui {

namespace {

constexpr int kSlot = 24;
constexpr int kLargeChestSlot = 17;
constexpr int kColumns = 9;

// The panel fills the page band with a pixel of backdrop round it, which is the
// Items page's own frame. **All of it hangs off the page's top**, which the
// caller passes in: a mode with no hotbar hands over a smaller number and every
// slot on every screen moves up with it.
constexpr int kPanelX = 1;
constexpr int kPanelW = kLayoutScreenWidth - 2;

struct Band {
    int panelY;
    int panelH;
    int innerBottom;
    int titleRow;  // the first character row inside the panel, one-based
};

constexpr Band bandFrom(int pageTop)
{
    const int panelY = pageTop;
    const int panelH = kLayoutPageBottom - panelY - 1;
    return Band{panelY, panelH, panelY + panelH - 2, panelY / 8 + 2};
}

SlotRect rectAt(int x, int y, int w, int h)
{
    SlotRect r;
    r.x = i16(x);
    r.y = i16(y);
    r.w = i16(w);
    r.h = i16(h);
    return r;
}

void add(ContainerLayout* out, int x, int y, int size)
{
    if (out->slots < kMaxLayoutSlots) {
        out->rect[out->slots++] = rectAt(x, y, size, size);
    }
}

void title(ContainerLayout* out, const char* text, int row, int column)
{
    if (out->titles < kMaxLayoutTitles) {
        out->title[out->titles] = text;
        out->titleRow[out->titles] = row;
        out->titleColumn[out->titles] = column;
        ++out->titles;
    }
}

// A block of `rows` x 9 slots of `size`, centred across the screen.
void addGrid(ContainerLayout* out, int top, int rows, int size)
{
    const int left = (kLayoutScreenWidth - kColumns * size) / 2;
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            add(out, left + column * size, top + row * size, size);
        }
    }
}

// The same block, but only `window` rows of it are on the screen: the rows
// before `first` and after the window get a **zero-sized rectangle** and keep
// their index. Nothing draws, touches or steps on a slot with no width, so the
// scroll costs no second numbering anywhere else.
void addScrolledGrid(ContainerLayout* out, int top, int rows, int size, int first, int window)
{
    const int left = (kLayoutScreenWidth - kColumns * size) / 2;
    for (int row = 0; row < rows; ++row) {
        const bool shown = row >= first && row < first + window;
        for (int column = 0; column < kColumns; ++column) {
            if (shown) {
                add(out, left + column * size, top + (row - first) * size, size);
            } else if (out->slots < kMaxLayoutSlots) {
                out->rect[out->slots++] = SlotRect{};
            }
        }
    }
}

// The arrows and the track, in the gutter between the grid's right edge and
// the panel's. `top` and `bottom` are the grid's own, so the arrows sit level
// with the first and last row a press moves.
void addScrollBar(ContainerLayout* out, int top, int bottom, int rows, int first, int window)
{
    constexpr int kArrow = 16;
    constexpr int kTrackW = 8;
    const int gridRight = (kLayoutScreenWidth + kColumns * kLargeChestSlot) / 2;
    const int centre = (gridRight + kPanelX + kPanelW) / 2;
    const int x = centre - kArrow / 2;

    out->scrollUp = rectAt(x, top, kArrow, kArrow);
    out->scrollDown = rectAt(x, bottom - kArrow, kArrow, kArrow);

    const int trackTop = top + kArrow + 2;
    const int trackH = (bottom - kArrow - 2) - trackTop;
    if (trackH < 8) {
        return;
    }
    out->scrollTrack = rectAt(centre - kTrackW / 2, trackTop, kTrackW, trackH);

    // The thumb is as much of the track as the window is of the chest, never
    // shorter than it is wide -- a two-pixel thumb says nothing.
    int thumbH = trackH * window / rows;
    if (thumbH < kTrackW) {
        thumbH = kTrackW;
    }
    const int span = rows - window;
    const int thumbY = trackTop + (span <= 0 ? 0 : (trackH - thumbH) * first / span);
    out->scrollThumb = rectAt(centre - kTrackW / 2, thumbY, kTrackW, thumbH);
}

// The backpack against the bottom of the panel, then the hand in the band.
void addPlayer(ContainerLayout* out, const Band& band, int size)
{
    addGrid(out, band.innerBottom - 3 * size, 3, size);
    for (int i = 0; i < item::kHotbarSlots; ++i) {
        const int x = i * kLayoutScreenWidth / item::kHotbarSlots;
        const int next = (i + 1) * kLayoutScreenWidth / item::kHotbarSlots;
        if (out->slots < kMaxLayoutSlots) {
            out->rect[out->slots++] =
                rectAt(x, kLayoutHotbarSlotY, next - x, kLayoutHotbarSlotHeight);
        }
    }
}

}  // namespace

void buildContainerLayout(const item::ContainerSession& session, int pageTop,
                          ContainerLayout* out, int chestFirstRow)
{
    const Band band = bandFrom(pageTop);
    const int titleRow = band.titleRow;
    const int innerBottom = band.innerBottom;

    *out = ContainerLayout{};
    out->panel = rectAt(kPanelX, band.panelY, kPanelW, band.panelH);

    // The top of the space above the backpack, and its middle row of pixels.
    const int top = band.panelY + 8;

    switch (session.kind()) {
    case item::ScreenKind::None:
        return;

    case item::ScreenKind::Inventory: {
        // `lo`: the result, the 2 x 2 grid, then the four armour slots helmet
        // first. The armour is a row here rather than `lo`'s column, which
        // would be four slots tall in a band with room for two.
        const int middle = top + 40;
        add(out, 256, middle - kSlot / 2, kSlot);
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                add(out, 172 + x * kSlot, middle - kSlot + y * kSlot, kSlot);
            }
        }
        for (int i = 0; i < item::kArmourSlots; ++i) {
            add(out, 52 + i * kSlot, middle - kSlot / 2, kSlot);
        }
        out->arrow = rectAt(224, middle - 6, 26, 12);
        title(out, "Armour", titleRow, 52 / 8 + 1);
        title(out, "Crafting", titleRow, 172 / 8 + 1);
        break;
    }

    case item::ScreenKind::Workbench: {
        // `hx`: the result, then the 3 x 3 grid row by row.
        const int middle = top + 40;
        add(out, 232, middle - kSlot / 2, kSlot);
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                add(out, 100 + x * kSlot, middle - 3 * kSlot / 2 + y * kSlot, kSlot);
            }
        }
        out->arrow = rectAt(182, middle - 6, 38, 12);
        title(out, "Crafting", titleRow, 2);
        break;
    }

    case item::ScreenKind::Furnace: {
        // `id`: input over the flame over fuel, the arrow, then the output.
        const int middle = top + 40;
        add(out, 112, middle - 16 - kSlot, kSlot);     // input
        add(out, 112, middle + 16, kSlot);             // fuel
        add(out, 204, middle - kSlot / 2, kSlot);      // output
        out->flame = rectAt(116, middle - 8, 16, 16);
        out->arrow = rectAt(148, middle - 6, 44, 12);
        out->arrowIsProgress = true;
        title(out, "Furnace", titleRow, 2);
        break;
    }

    // **A chest cart's screen is a chest's**, which is not a convenience: the
    // original opens the same `GuiChest` on it, so the slot grid, the rows and
    // the title are the same by construction rather than by resemblance.
    case item::ScreenKind::MinecartChest:
    case item::ScreenKind::Chest: {
        const int rows = session.chestRows();
        out->chestRows = rows;
        out->chestWindowRows = rows;
        if (rows <= 3) {
            addGrid(out, top, rows, kSlot);
            addPlayer(out, band, kSlot);
            return;
        }
        // **No title on a chest.** The other three screens are named because
        // their slots do not say what they are -- a result slot, an armour row,
        // a furnace's two inputs. A grid of chest slots does, and the word was
        // in the way of the only screen that has no room to spare.
        const int backpackTop = innerBottom - 3 * kLargeChestSlot;
        // How many 17-pixel rows there is room for above the backpack. Six, at either page top -- a double chest is exactly the
        // biggest thing that fits, which is why nothing needed to scroll until
        // a chest could be joined to more than one other.
        int window = (backpackTop - 4 - top) / kLargeChestSlot;
        if (window > rows) {
            window = rows;
        }
        out->chestWindowRows = window;

        const int gridTop = backpackTop - 4 - window * kLargeChestSlot;
        if (window >= rows) {
            addGrid(out, gridTop, rows, kLargeChestSlot);
        } else {
            int first = chestFirstRow;
            if (first > rows - window) {
                first = rows - window;
            }
            if (first < 0) {
                first = 0;
            }
            out->chestFirstRow = first;
            addScrolledGrid(out, gridTop, rows, kLargeChestSlot, first, window);
            addScrollBar(out, gridTop, gridTop + window * kLargeChestSlot, rows, first, window);
        }
        addPlayer(out, band, kLargeChestSlot);
        return;
    }
    }
    addPlayer(out, band, kSlot);
}

int containerScrollAt(const ContainerLayout& layout, int x, int y)
{
    // Generously larger than what is drawn, the way the palette's page arrows
    // are: the box is sixteen pixels and a fingertip is not.
    constexpr int kPad = 4;
    const auto inside = [&](const SlotRect& r) {
        return r.w > 0 && x >= r.x - kPad && x < r.x + r.w + kPad && y >= r.y - kPad
               && y < r.y + r.h + kPad;
    };
    if (inside(layout.scrollUp)) {
        return -1;
    }
    if (inside(layout.scrollDown)) {
        return 1;
    }
    return 0;
}

int containerSlotAt(const ContainerLayout& layout, int x, int y)
{
    for (int i = 0; i < layout.slots; ++i) {
        const SlotRect& r = layout.rect[i];
        if (r.w <= 0) {
            continue;  // scrolled out of the window
        }
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            return i;
        }
    }
    return -1;
}

int containerStep(const ContainerLayout& layout, int from, int dx, int dy)
{
    if (from < 0 || from >= layout.slots || (dx == 0 && dy == 0)) {
        return from;
    }
    // Centres doubled, so a 17-pixel slot's middle is still a whole number.
    const SlotRect& here = layout.rect[from];
    const int cx = 2 * here.x + here.w;
    const int cy = 2 * here.y + here.h;

    if (here.w <= 0) {
        return from;
    }

    int best = from;
    int bestScore = 0;
    for (int i = 0; i < layout.slots; ++i) {
        if (i == from) {
            continue;
        }
        const SlotRect& r = layout.rect[i];
        if (r.w <= 0) {
            continue;  // scrolled out of the window
        }
        const int vx = 2 * r.x + r.w - cx;
        const int vy = 2 * r.y + r.h - cy;
        const int along = dx != 0 ? vx * dx : vy * dy;
        if (along <= 0) {
            continue;
        }
        // Straight ahead beats diagonal: sideways distance counts double, so
        // the slot beside this one wins over a nearer one a row off.
        const int across = std::abs(dx != 0 ? vy : vx);
        // And only within about 27 degrees of the press, or left off a grid's
        // first column would find the hotbar band up in the corner.
        if (2 * across > along) {
            continue;
        }
        const int score = along + 2 * across;
        if (best == from || score < bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

}  // namespace mc::gui
