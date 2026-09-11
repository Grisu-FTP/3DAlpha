#include "platform/ctr/hud.hpp"

#include "core/block/registry.hpp"
#include "core/gui/item_icon.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"
#include "core/texture/atlas_image.hpp"

#include <3ds.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mc::ctr::hud {

namespace {

gui::Pixel px(u32 rgb)
{
    return gui::rgb565(rgb);
}

// The colour half of `text`. Two sequences rather than one, because a single
// SGR carrying both would be six parameters deep and there is nothing to be
// won by making it harder to read.
int writeColours(char* out, usize size, u32 fg, u32 bg)
{
    return std::snprintf(out, size, "\x1b[38;2;%lu;%lu;%lum\x1b[48;2;%lu;%lu;%lum",
                         (unsigned long)((fg >> 16) & 0xFF), (unsigned long)((fg >> 8) & 0xFF),
                         (unsigned long)(fg & 0xFF), (unsigned long)((bg >> 16) & 0xFF),
                         (unsigned long)((bg >> 8) & 0xFF), (unsigned long)(bg & 0xFF));
}

// The seven letters, five by seven, one byte a row with 0x10 as the leftmost
// pixel. Indexed by `Letter`, so the order here is the order there.
constexpr int kGlyphWidth = kLetterWidth;
constexpr int kGlyphHeight = kLetterHeight;
const u8 kGlyphs[7][kGlyphHeight] = {
    {0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11},  // N
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11},  // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},  // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
};

void drawGlyph(const gui::Surface& surface, int x, int y, const u8* rows, gui::Pixel colour)
{
    for (int row = 0; row < kGlyphHeight; ++row) {
        for (int col = 0; col < kGlyphWidth; ++col) {
            if ((rows[row] & (0x10 >> col)) != 0) {
                gui::Pixel* out = surface.at(x + col, y + row);
                if (out != nullptr) {
                    *out = colour;
                }
            }
        }
    }
}

// The eight compass points, as the glyphs that spell them. Index 0 is north and
// they run clockwise, which is the order a bearing counts in.
const u8* const kPointGlyphs[8][2] = {
    {kGlyphs[0], nullptr},      {kGlyphs[0], kGlyphs[1]}, {kGlyphs[1], nullptr},
    {kGlyphs[2], kGlyphs[1]},   {kGlyphs[2], nullptr},    {kGlyphs[2], kGlyphs[3]},
    {kGlyphs[3], nullptr},      {kGlyphs[0], kGlyphs[3]},
};

// The look pad and the compass ribbon inside it, shared by the two calls that
// draw them.
constexpr int kPadX = 8;
// **The page's top, not the body's**, and it stops eight pixels short of the
// hotbar. It used to run 32..232; the hotbar took the bottom band and the focus
// banner the top one, and a drag area that ran into either would be a drag
// starting on a control.
constexpr int kPadY = kPageTop;
constexpr int kPadW = kScreenWidth - kPadX * 2;
constexpr int kPadH = kTabTop - kPadY - 8;
static_assert(kPadY + kPadH <= kTabTop, "the look pad must clear the tab strip");
static_assert(kPadY >= kPageTop, "the look pad must clear the banner band");
constexpr int kRibbonX = kPadX + 8;
constexpr int kRibbonY = kPadY + 8;
constexpr int kRibbonW = kPadW - 16;
constexpr int kRibbonH = 32;
constexpr u32 kRibbonFace = 0x0C0C0C;
// 1.5 pixels a degree puts a little over 90 degrees either side of the centre
// on a 288-pixel ribbon, which is wide enough that the point behind you is off
// it and the ones beside you are not.
constexpr float kPixelsPerDegree = 1.5f;

// Where a tab starts and ends, in character columns, so a label can be centred
// on the cell grid rather than between two of them.
void tabSpan(const TabStrip& tabs, int index, int* left, int* width)
{
    *left = index * kScreenWidth / tabs.count;
    *width = (index + 1) * kScreenWidth / tabs.count - *left;
}

}  // namespace

bool bottomSurface(gui::Surface* out)
{
    u16 framebufferWidth = 0;
    u16 framebufferHeight = 0;
    u8* raw = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &framebufferWidth, &framebufferHeight);

    // Checked rather than assumed: libctru reports the framebuffer in the
    // orientation it is stored in, 240 down a column and 320 columns across.
    if (raw == nullptr || framebufferWidth != kScreenHeight || framebufferHeight != kScreenWidth) {
        return false;
    }

    out->pixels = reinterpret_cast<gui::Pixel*>(raw) + (kScreenHeight - 1);
    out->strideX = kScreenHeight;
    out->strideY = -1;
    out->width = kScreenWidth;
    out->height = kScreenHeight;
    return true;
}

void text(int row, int column, int columns, u32 fg, u32 bg, const char* fmt, ...)
{
    if (row < 1 || row > kRows || column < 1 || columns <= 0) {
        return;
    }
    if (column + columns - 1 > kColumns) {
        columns = kColumns - column + 1;
        if (columns <= 0) {
            return;
        }
    }

    char body[96];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    char colours[64];
    writeColours(colours, sizeof(colours), fg, bg);

    // `%-*.*s` is both halves of the contract: clipped to the columns claimed,
    // and padded out to them so a shorter string overwrites what was there.
    // **Plain text only** -- an escape sequence in `body` would be counted as
    // the bytes it costs rather than the nothing it draws, and there is no
    // reason for one, because the colours are the caller's to pass.
    std::printf("\x1b[%d;%dH%s%-*.*s\x1b[0m", row, column, colours, columns, columns, body);
}

void textCentred(int row, int left, int width, u32 fg, u32 bg, const char* string)
{
    const int length = int(std::strlen(string));
    // The nearest cell to the middle. A glyph cannot start half way through a
    // character cell, so the text is centred to within four pixels and that is
    // the whole of it.
    int column = (left + (width - length * kCell) / 2 + kCell / 2) / kCell;
    if (column < 0) {
        column = 0;
    }
    text(row, column + 1, length, fg, bg, "%s", string);
}

void drawLetter(const gui::Surface& surface, int x, int y, Letter letter, u32 colour)
{
    drawGlyph(surface, x, y, kGlyphs[int(letter)], px(colour));
}

void panel(const gui::Surface& surface, int x, int y, int w, int h)
{
    gui::bevelBox(surface, x, y, w, h, px(kPanelFace), px(kPanelLight), px(kPanelDark), true);
}

void slot(const gui::Surface& surface, int x, int y, int w, int h)
{
    gui::bevelBox(surface, x, y, w, h, px(kSlotFace), px(kPanelLight), px(kSlotDark), false);
}

void readout(const gui::Surface& surface, int x, int y, int w, int h)
{
    // The light half of the bevel is grey rather than white here: a full white
    // edge against a near-black face is a hard line that reads as a mistake,
    // and the point of the bevel is depth rather than contrast.
    gui::bevelBox(surface, x, y, w, h, px(kReadoutFace), px(kSlotFace), px(0x000000), false);
}

void drawBackdrop(const gui::Surface& surface, const gui::Pixel* tile)
{
    if (tile != nullptr) {
        gui::tilePattern(surface, 0, 0, kScreenWidth, kScreenHeight, tile, kTileEdge);
        return;
    }
    gui::fillRect(surface, 0, 0, kScreenWidth, kScreenHeight, px(kBackdrop));
}

void drawTabs(const gui::Surface& surface, const TabStrip& tabs)
{
    if (tabs.count <= 0) {
        return;
    }

    for (int i = 0; i < tabs.count; ++i) {
        int left = 0;
        int width = 0;
        tabSpan(tabs, i, &left, &width);
        const bool chosen = i == tabs.selected;

        // **The chosen tab is the tall one.** It stands the full height of the
        // strip while the others are inset four pixels from the top, which is
        // how a tab has said "this is the page you are on" since long before
        // this console -- and it costs no colour, so it still reads on a screen
        // the player has turned the brightness down on.
        // **The chosen tab is the tall one, and it now grows upwards**, which
        // is the whole of what moving the strip to the bottom of the screen
        // costs: an idle tab is inset four pixels from the *page* side, so the
        // chosen one still reaches towards the page it opens.
        if (chosen) {
            gui::bevelBox(surface, left, kTabTop, width, kTabHeight, px(kPanelFace),
                          px(kPanelLight), px(kPanelDark), true);
        } else {
            gui::bevelBox(surface, left, kTabTop + 4, width, kTabHeight - 4, px(kTabIdleFace),
                          px(kSlotFace), px(kSlotDark), true);
        }

        // **The middle of the three rows, not the first.** `kTabTop / kCell` is
        // the row the strip starts on counting from zero, and `text` counts
        // from one -- so `+ 1` names the strip's *top* row and put every label
        // hard against the upper edge of its button. The band is 216..240,
        // which is rows 28, 29 and 30 one-based; 29 is the one whose glyphs
        // (224..231) share their centre with the button's (216..239).
        textCentred(kTabTop / kCell + 2, left, width, chosen ? kPanelText : kTabIdleText,
                    chosen ? kPanelFace : kTabIdleFace, tabs.labels[i]);
    }
}

int tabAt(const TabStrip& tabs, int touchX, int touchY)
{
    if (tabs.count <= 0 || touchY < kTabTop || touchY >= kScreenHeight) {
        return -1;
    }
    for (int i = 0; i < tabs.count; ++i) {
        int left = 0;
        int width = 0;
        tabSpan(tabs, i, &left, &width);
        if (touchX >= left && touchX < left + width) {
            return i;
        }
    }
    return -1;
}

void drawLookPage(const gui::Surface& surface)
{
    readout(surface, kPadX, kPadY, kPadW, kPadH);
    gui::bevelBox(surface, kRibbonX, kRibbonY, kRibbonW, kRibbonH, px(kRibbonFace), px(0x555555),
                  px(0x000000), false);

    // Where the ribbon is read: a notch under it, pointing up at the mark the
    // player is facing. Under it rather than on it, so it never sits on top of
    // a tick and leaves you guessing which one it means -- and outside the
    // rectangle the ribbon clears, because the centre is the one place on it
    // that never moves.
    for (int i = 0; i < 5; ++i) {
        gui::hLine(surface, kRibbonX + kRibbonW / 2 - i, kRibbonY + kRibbonH + i, i * 2 + 1,
                   px(0xFFD24A));
    }

    // **The drag area, marked at its corners.** A blank rectangle says nothing
    // about whether it is for touching; four brackets say it is, and they cost
    // no row of text to say it with.
    const int dragTop = kRibbonY + kRibbonH + 8;
    const int dragLeft = kPadX + 8;
    const int dragRight = kPadX + kPadW - 9;
    const int dragBottom = kPadY + kPadH - 9;
    const gui::Pixel bracket = px(0x3E3E3E);
    constexpr int kArm = 12;
    for (int corner = 0; corner < 4; ++corner) {
        const int x = (corner & 1) != 0 ? dragRight : dragLeft;
        const int y = (corner & 2) != 0 ? dragBottom : dragTop;
        const int stepX = (corner & 1) != 0 ? -1 : 1;
        const int stepY = (corner & 2) != 0 ? -1 : 1;
        gui::fillRect(surface, stepX > 0 ? x : x - kArm + 1, y, kArm, 1, bracket);
        gui::fillRect(surface, x, stepY > 0 ? y : y - kArm + 1, 1, kArm, bracket);
    }

    // A crosshair in the middle of it, because a crosshair is the one mark that
    // says "this is the view" without a label.
    const int crossX = kScreenWidth / 2;
    const int crossY = (dragTop + dragBottom) / 2;
    const gui::Pixel crossColour = px(0x6E6E6E);
    gui::hLine(surface, crossX - 12, crossY, 25, crossColour);
    gui::vLine(surface, crossX, crossY - 12, 25, crossColour);
    gui::fillRect(surface, crossX - 1, crossY - 1, 3, 3, px(0xE0E0E0));
}

void drawCompassRibbon(const gui::Surface& surface, float yawDegrees)
{
    // Cleared to the ribbon's own face rather than redrawn as a box, so the
    // bevel around it survives a turn.
    gui::fillRect(surface, kRibbonX + 1, kRibbonY + 1, kRibbonW - 2, kRibbonH - 2,
                  px(kRibbonFace));

    // **The bearing, not the yaw.** Minecraft's yaw is zero looking south and
    // increases towards west; a compass counts clockwise from north. The two
    // differ by half a turn, which is why this is an offset rather than a
    // table.
    float bearing = std::fmod(yawDegrees + 180.0f, 360.0f);
    if (bearing < 0.0f) {
        bearing += 360.0f;
    }

    const int centreX = kRibbonX + kRibbonW / 2;
    const gui::Pixel tickColour = px(0x7A7A7A);
    const gui::Pixel pointColour = px(0xFFFFFF);

    // Every fifteen degrees, placed by the pixel rather than by the cell, which
    // is the whole reason the four letters below are drawn here instead of
    // printed: a character cell cannot slide.
    for (int degrees = 0; degrees < 360; degrees += 15) {
        float offset = float(degrees) - bearing;
        offset = std::fmod(offset + 540.0f, 360.0f) - 180.0f;
        const int x = centreX + int(std::lround(offset * kPixelsPerDegree));
        if (x < kRibbonX + 2 || x >= kRibbonX + kRibbonW - 2) {
            continue;
        }
        const bool cardinal = (degrees % 45) == 0;
        gui::vLine(surface, x, kRibbonY + kRibbonH - (cardinal ? 12 : 7), cardinal ? 10 : 5,
                   cardinal ? pointColour : tickColour);
        if (!cardinal) {
            continue;
        }
        const u8* const* glyphs = kPointGlyphs[degrees / 45];
        const int letters = glyphs[1] != nullptr ? 2 : 1;
        const int width = letters * kGlyphWidth + (letters - 1);
        int letterX = x - width / 2;
        for (int i = 0; i < letters; ++i) {
            drawGlyph(surface, letterX, kRibbonY + 6, glyphs[i], pointColour);
            letterX += kGlyphWidth + 1;
        }
    }

}


// ---------------------------------------------------------------------------
// Block icons, the hotbar, and the Creative palette.
// ---------------------------------------------------------------------------

namespace {

// The palette page's furniture, laid out from the same 24-pixel slot the hotbar
// used to be. Nine by five is 45 cells, so a1.1.2's 147 offered items are four
// pages -- and a version with more of them gets more pages without a constant
// here changing.
//
// **It is centred on the screen rather than on the hotbar**, which it used to
// take its columns from. The hotbar's columns are 35 and 36 pixels wide now and
// the palette's are 24, so the two grids no longer line up whatever is done --
// and a nine-wide grid that is *nearly* under another nine-wide grid reads
// worse than one that is plainly its own thing.
constexpr int kPalGridWidth = kPaletteColumns * kSlotPixels;
constexpr int kPalPanelX = (kScreenWidth - kPalGridWidth) / 2 - 8;
constexpr int kPalPanelW = kPalGridWidth + 16;
constexpr int kPalPanelY = kPageTop;
constexpr int kPalPanelH = kTabTop - kPageTop - 8;
constexpr int kPalGridX = kPalPanelX + 8;
constexpr int kPalGridY = kPalPanelY + 24;          // under the title row
constexpr int kPalTitleRow = kPalPanelY / kCell + 2;
// The first whole character row under the grid. `text` rows are 1-based, so
// the +1 is what puts it below the last slot instead of across it.
constexpr int kPalCaptionRow = (kPalGridY + kPaletteRows * kSlotPixels) / kCell + 1;

// The two page arrows.
//
// **The glyph is one character cell inside a sixteen-pixel box, not two.** The
// arrow is console text, and `text` paints its own background across every
// column it claims -- so a two-column field on a sixteen-pixel slot filled the
// slot edge to edge and painted over both halves of the bevel, which is the
// arrow "flowing out left and right". One column is eight pixels, so the fill
// stops four pixels short on each side and the bevel survives.
//
// A character cell cannot start half way through the 8-pixel grid, so the
// *box* is placed around the cell rather than the other way round: the arrow's
// left edge is four pixels left of a cell boundary, which is what the two
// static_asserts below pin.
constexpr int kArrowW = 16;
constexpr int kArrowH = 16;
constexpr int kArrowY = kPalPanelY + 4;
constexpr int kArrowInset = (kArrowW - kCell) / 2;
constexpr int kArrowLeftX = kPalPanelX + 8;
constexpr int kArrowRightX = kPalPanelX + kPalPanelW - 8 - kArrowW;

// The console column the glyph goes in, 1-based as `text` counts them.
constexpr int arrowColumn(int x) { return (x + kArrowInset) / kCell + 1; }

static_assert((kArrowLeftX + kArrowInset) % kCell == 0,
              "the left arrow's glyph cell must land on the character grid");
static_assert((kArrowRightX + kArrowInset) % kCell == 0,
              "the right arrow's glyph cell must land on the character grid");
static_assert(kArrowLeftX >= kPalPanelX + 4 && kArrowRightX + kArrowW <= kPalPanelX + kPalPanelW - 4,
              "both arrows must stay inside the panel");
static_assert(kArrowY + kArrowH <= kPalGridY, "the arrows must clear the palette grid");

static_assert(kPalPanelY >= kPageTop, "the palette must clear the banner band");
static_assert(kPalPanelY + kPalPanelH <= kTabTop, "the palette must clear the tab strip");
static_assert(kPalGridY + kPaletteRows * kSlotPixels <= kPalPanelY + kPalPanelH,
              "the palette grid must fit its panel");
static_assert(kPalCaptionRow * kCell <= kPalPanelY + kPalPanelH,
              "the caption must fit inside the panel");

// Highlights. **Neither is a1.1.2's**, because a1.1.2 has neither: its hotbar
// selection is a sprite off `gui.png` and it has no cursor at all, there being
// a mouse. White for the hand and amber for the cursor, which are the two
// colours already used on this screen for "this is yours" and "this is where
// you are" -- the map's marker is the same amber.
constexpr u32 kSelectedEdge = 0xFFFFFF;
constexpr u32 kCursorEdge = 0xFFD24A;

// A two-pixel frame just outside a slot, which is where a1.1.2 puts its own
// selection sprite: over the gap between slots rather than over the item, so
// nothing it marks is harder to see for being marked.
void slotEdge(const gui::Surface& surface, int x, int y, int w, int h, u32 colour)
{
    const gui::Pixel pixel = px(colour);
    gui::frameRect(surface, x, y, w, h, pixel);
    gui::frameRect(surface, x + 1, y + 1, w - 2, h - 2, pixel);
}

// The digits, three by five, one byte a row with 0x04 as the leftmost pixel.
// Small enough that two of them fit in the corner of a 24-pixel slot without
// touching the icon, which is the whole requirement.
constexpr int kDigitWidth = 3;
constexpr int kDigitHeight = 5;
constexpr u8 kDigits[10][kDigitHeight] = {
    {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7}, {0x7, 0x1, 0x7, 0x4, 0x7},
    {0x7, 0x1, 0x7, 0x1, 0x7}, {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1}, {0x7, 0x5, 0x7, 0x5, 0x7},
    {0x7, 0x5, 0x7, 0x1, 0x7},
};

// A stack count in the bottom-right of a slot, with a one-pixel shadow under
// it. **Only above one**, which is what the original does: `RenderItem` draws
// the number when `stackSize > 1`, and a slot showing "1" on every item is
// noise on a screen with 45 of them.
void drawCount(const gui::Surface& surface, int x, int y, int w, int h, int count)
{
    if (count <= 1) {
        return;
    }
    const int digits = count >= 100 ? 3 : (count >= 10 ? 2 : 1);
    int left = x + w - 3 - digits * (kDigitWidth + 1);
    const int top = y + h - 3 - kDigitHeight;

    int divisor = 1;
    for (int i = 1; i < digits; ++i) {
        divisor *= 10;
    }
    for (int i = 0; i < digits; ++i, divisor /= 10) {
        const u8* rows = kDigits[(count / divisor) % 10];
        for (int row = 0; row < kDigitHeight; ++row) {
            for (int col = 0; col < kDigitWidth; ++col) {
                if ((rows[row] & (0x4 >> col)) == 0) {
                    continue;
                }
                // Shadow first, then the glyph over it, so the number stays
                // readable on a light tile as well as a dark one.
                gui::Pixel* shadow = surface.at(left + col + 1, top + row + 1);
                if (shadow != nullptr) {
                    *shadow = px(0x101010);
                }
                gui::Pixel* out = surface.at(left + col, top + row);
                if (out != nullptr) {
                    *out = px(0xFFFFFF);
                }
            }
        }
        left += kDigitWidth + 1;
    }
}

// One cell of a grid: the slot, the item's icon in the middle of it, its count
// if it has more than one, and whichever of the two edges apply.
void drawCell(const gui::Surface& surface, int x, int y, int w, int h,
              const gui::IconSheets& sheets, item::ItemId id, int count, bool selected,
              bool cursor)
{
    slot(surface, x, y, w, h);
    if (id != 0) {
        // The icon is centred rather than stretched: a cell that is 35 wide and
        // 28 tall is not square, and an icon drawn to fill it would be too.
        const int size = kIconPixels < w - 2 ? kIconPixels : w - 2;
        const int fit = size < h - 2 ? size : h - 2;
        gui::drawItemIcon(surface, x + (w - fit) / 2, y + (h - fit) / 2, fit, sheets, id);
        drawCount(surface, x, y, w, h, count);
    }
    // The cursor is drawn last so it wins where both apply -- which is the
    // common case, since picking a block puts the cursor and the hand on it.
    if (selected) {
        slotEdge(surface, x, y, w, h, kSelectedEdge);
    }
    if (cursor) {
        slotEdge(surface, x, y, w, h, kCursorEdge);
    }
}

// The square case, which is every grid but the hotbar.
void drawCell(const gui::Surface& surface, int x, int y, int size,
              const gui::IconSheets& sheets, item::ItemId id, int count, bool selected,
              bool cursor)
{
    drawCell(surface, x, y, size, size, sheets, id, count, selected, cursor);
}

bool insideBox(int px_, int py_, int x, int y, int w, int h)
{
    return px_ >= x && px_ < x + w && py_ >= y && py_ < y + h;
}

}  // namespace

namespace {

// **The Items page fills the page band**, which is 320 x 168 between the focus
// banner and the tab strip. Nine columns of backpack at 32 pixels is 288, an
// armour column of the same beside it is 320, and 320 is the screen -- so the
// two grids are laid out from the panel's inside edges rather than centred, and
// the panel is the page.
//
// **At file scope so the drawing and the hit test share them**, which they
// previously did not: `itemsCellAt` used to be able to disagree with
// `drawItemsPage` about where the grid was, and a hit test that is a few pixels
// out is the kind of bug that is felt as "the touchscreen is bad".
constexpr int kItemsPanelX = 2;
constexpr int kItemsPanelW = kScreenWidth - 4;              // 316
constexpr int kItemsPanelY = kPageTop + 2;                  // 50
constexpr int kItemsPanelH = kTabTop - kItemsPanelY - 2;    // 164

// The title row, then the two grids under it.
constexpr int kItemsTitleRow = kItemsPanelY / kCell + 1;
constexpr int kItemsGridTop = kItemsPanelY + 22;

// The armour column is on the left, where a paper doll would be, and the
// backpack fills what is left. Four armour slots stand taller than three rows
// of backpack, so the panel's height is the armour's.
constexpr int kItemsArmourX = kItemsPanelX + 6;                      // 8
constexpr int kItemsArmourY = kItemsGridTop;
constexpr int kItemsGridWidth = kItemsColumns * kItemsSlotPixels;    // 270
constexpr int kItemsGridX = kScreenWidth - 4 - kItemsGridWidth;      // 46
constexpr int kItemsGridY = kItemsGridTop;
constexpr int kItemsGridHeight = kItemsRows * kItemsSlotPixels;      // 90

static_assert(kItemsPanelY + kItemsPanelH <= kTabTop,
              "the inventory must clear the tab strip");
static_assert(kItemsPanelY >= kPageTop, "the inventory must clear the banner band");
static_assert(kItemsArmourX + kItemsSlotPixels <= kItemsGridX,
              "the armour column must not run into the backpack grid");
static_assert(kItemsGridX + kItemsGridWidth <= kItemsPanelX + kItemsPanelW,
              "the backpack grid must fit the panel");
static_assert(kItemsArmourY + item::kArmourSlots * kItemsSlotPixels
                  <= kItemsPanelY + kItemsPanelH,
              "the armour column must fit the panel");
static_assert(kItemsColumns * kItemsRows == item::kBackpackSlots,
              "the grid must hold every slot that is not the hand");

// The four armour slots, top to bottom. a1.1.2 numbers them **feet first** --
// `armorInventory[0]` is the boots and `[3]` the helmet -- so drawing them in
// index order would stand the player on their head. This is the one place that
// order is inverted, and it is inverted here rather than in `Inventory` because
// the save file's order is the save file's.
int armourIndexForRow(int row) { return item::kArmourSlots - 1 - row; }

}  // namespace

int itemsSlotForCell(int cell)
{
    if (cell < 0 || cell >= kItemsCells) {
        return -1;
    }
    if (cell < item::kBackpackSlots) {
        return item::kHotbarSlots + cell;
    }
    return item::kArmourBase + armourIndexForRow(cell - item::kBackpackSlots);
}

void drawItemsPage(const gui::Surface& surface, const item::Inventory& inventory,
                   const gui::IconSheets& sheets, int cursor, int held)
{
    panel(surface, kItemsPanelX, kItemsPanelY, kItemsPanelW, kItemsPanelH);
    textCentred(kItemsTitleRow, kItemsPanelX, kItemsPanelW, kPanelText, kPanelFace,
                "Inventory");

    // The backpack: slots 9..35, three rows of nine.
    for (int row = 0; row < kItemsRows; ++row) {
        for (int column = 0; column < kItemsColumns; ++column) {
            const int cell = row * kItemsColumns + column;
            const int slotNumber = itemsSlotForCell(cell);
            const item::ItemStack& stack = inventory.at(slotNumber);
            // A stack that has been picked up is drawn as an empty cell with
            // its outline still on it: it is following the cursor, so leaving
            // its icon behind would show it in two places at once.
            const bool lifted = slotNumber == held;
            drawCell(surface, kItemsGridX + column * kItemsSlotPixels,
                     kItemsGridY + row * kItemsSlotPixels, kItemsSlotPixels, sheets,
                     lifted || stack.empty() ? item::ItemId(0) : item::ItemId(stack.id),
                     lifted ? 0 : int(stack.count), false, cell == cursor);
        }
    }

    // **The armour, which had nowhere to be drawn until now.** `Inventory` has
    // held these four since the item table landed and they round-trip through
    // `level.dat`; what was missing was somewhere to put them. Nothing in this
    // build wears armour yet -- there is no damage to reduce -- so a helmet in
    // slot 103 is carried and saved and does nothing, which is honest and is
    // still better than a helmet that cannot be put anywhere at all.
    for (int row = 0; row < item::kArmourSlots; ++row) {
        const int cell = item::kBackpackSlots + row;
        const int slotNumber = itemsSlotForCell(cell);
        const item::ItemStack& stack = inventory.at(slotNumber);
        const bool lifted = slotNumber == held;
        drawCell(surface, kItemsArmourX, kItemsArmourY + row * kItemsSlotPixels,
                 kItemsSlotPixels, sheets,
                 lifted || stack.empty() ? item::ItemId(0) : item::ItemId(stack.id),
                 lifted ? 0 : int(stack.count), false, cell == cursor);
    }

    // Inside the panel rather than under it, dimmer than the title, because it
    // is an explanation rather than a heading -- and because a row of text
    // outside the panel would have to guess the backdrop's colour, which is the
    // pack's dirt and not a constant.
    textCentred((kItemsGridY + kItemsGridHeight) / kCell + 2, kItemsGridX, kItemsGridWidth,
                kPanelDark, kPanelFace, held >= 0 ? "A to put it down" : "A picks a stack up");
}

int itemsCellAt(int touchX, int touchY)
{
    if (insideBox(touchX, touchY, kItemsGridX, kItemsGridY, kItemsGridWidth,
                  kItemsGridHeight)) {
        return ((touchY - kItemsGridY) / kItemsSlotPixels) * kItemsColumns
               + (touchX - kItemsGridX) / kItemsSlotPixels;
    }
    if (insideBox(touchX, touchY, kItemsArmourX, kItemsArmourY, kItemsSlotPixels,
                  item::kArmourSlots * kItemsSlotPixels)) {
        return item::kBackpackSlots + (touchY - kItemsArmourY) / kItemsSlotPixels;
    }
    return -1;
}

void drawHotbar(const gui::Surface& surface, const item::Inventory& inventory,
                const gui::IconSheets& sheets, int cursor, int held)
{
    // **The band is repainted flat rather than re-tiled with the pack's dirt.**
    // It is the one part of the screen that redraws without a page change --
    // every shoulder press moves the selection -- and a flat fill is a third of
    // the cost of the tiled one. It also reads as a bar, which is what a1.1.2's
    // own hotbar is: a strip laid over the scene rather than part of it.
    gui::fillRect(surface, 0, kHotbarTop, kScreenWidth, kHotbarHeight, px(kBackdrop));

    for (int i = 0; i < kHotbarColumns; ++i) {
        const item::ItemStack& stack = inventory.main[i];
        const bool lifted = i == held;
        drawCell(surface, hotbarSlotX(i), kHotbarSlotY, hotbarSlotWidth(i), kHotbarSlotHeight,
                 sheets, lifted || stack.empty() ? item::ItemId(0) : item::ItemId(stack.id),
                 lifted ? 0 : int(stack.count), i == inventory.selected, i == cursor);
    }
}

int hotbarSlotAt(int touchX, int touchY)
{
    if (!insideBox(touchX, touchY, 0, kHotbarTop, kScreenWidth, kHotbarHeight)) {
        return -1;
    }
    // The whole height of the band, not just the slot's own -- two pixels of
    // margin is not something a finger on a resistive screen can aim inside.
    // The columns are not all the same width, so this is a search rather than a
    // divide; nine compares on a touch is nothing.
    for (int i = kHotbarColumns - 1; i >= 0; --i) {
        if (touchX >= hotbarSlotX(i)) {
            return i;
        }
    }
    return 0;
}

int palettePageCount()
{
    const int size = item::paletteSize();
    return size <= 0 ? 1 : (size + kPalettePerPage - 1) / kPalettePerPage;
}

void drawBlocksPage(const gui::Surface& surface, const gui::IconSheets& sheets, int page,
                    int cursor, item::ItemId selected, const char* caption)
{
    panel(surface, kPalPanelX, kPalPanelY, kPalPanelW, kPalPanelH);

    const int pages = palettePageCount();
    const int shown = page < 0 ? 0 : (page >= pages ? pages - 1 : page);

    char title[40];
    std::snprintf(title, sizeof title, "Blocks  %d/%d", shown + 1, pages);
    textCentred(kPalTitleRow, kPalPanelX, kPalPanelW, kPanelText, kPanelFace, title);

    // The arrows are drawn on every page, including the ones where they do
    // nothing, and dimmed where they do. A control that disappears at the end
    // of a list moves the other one, and a control that moves is a control that
    // gets mis-tapped.
    for (int side = 0; side < 2; ++side) {
        const int x = side == 0 ? kArrowLeftX : kArrowRightX;
        const bool live = side == 0 ? shown > 0 : shown + 1 < pages;
        slot(surface, x, kArrowY, kArrowW, kArrowH);
        text(kPalTitleRow, arrowColumn(x), 1, live ? kPanelText : kPanelDark, kSlotFace,
             side == 0 ? "<" : ">");
    }

    const int first = shown * kPalettePerPage;
    for (int row = 0; row < kPaletteRows; ++row) {
        for (int column = 0; column < kPaletteColumns; ++column) {
            const int cell = row * kPaletteColumns + column;
            const item::ItemId id = item::paletteItem(first + cell);
            // No count on a palette cell: the palette is a catalogue and
            // nothing in it is owned, so a number on one would be a quantity of
            // something nobody has.
            drawCell(surface, kPalGridX + column * kSlotPixels, kPalGridY + row * kSlotPixels,
                     kSlotPixels,
                     sheets, id, 1, id != 0 && id == selected, cell == cursor);
        }
    }

    // **The one place a block's name fits**, and the reason the page has a
    // caption at all: 45 cells of 16-pixel tile are not self-describing, and
    // "which of these two greys is gravel" is a question a picture cannot
    // answer. The caller supplies it, because what it names is whatever the
    // player is pointing at and this file does not know what that is.
    textCentred(kPalCaptionRow, kPalPanelX, kPalPanelW, kPanelText, kPanelFace,
                caption != nullptr ? caption : "");
}

int paletteCellAt(int touchX, int touchY)
{
    if (!insideBox(touchX, touchY, kPalGridX, kPalGridY, kPaletteColumns * kSlotPixels,
                   kPaletteRows * kSlotPixels)) {
        return -1;
    }
    const int column = (touchX - kPalGridX) / kSlotPixels;
    const int row = (touchY - kPalGridY) / kSlotPixels;
    return row * kPaletteColumns + column;
}

int paletteArrowAt(int touchX, int touchY)
{
    // Generously larger than what is drawn, in both directions: the box is 16
    // pixels and a fingertip on a resistive screen is not.
    constexpr int kPad = 4;
    if (insideBox(touchX, touchY, kArrowLeftX - kPad, kArrowY - kPad, kArrowW + kPad * 2,
                  kArrowH + kPad * 2)) {
        return -1;
    }
    if (insideBox(touchX, touchY, kArrowRightX - kPad, kArrowY - kPad, kArrowW + kPad * 2,
                  kArrowH + kPad * 2)) {
        return 1;
    }
    return 0;
}


void drawFocusBanner(const gui::Surface& surface, const char* label)
{
    // The label's own row: flat, because a character cell has one background
    // colour and there is nothing to be done about that. Near-black rather than
    // black, so it reads as a shade over the page instead of a hole in it.
    constexpr u32 kBannerFace = 0x101014;
    constexpr u32 kBannerText = 0xFFD24A;
    gui::fillRect(surface, 0, kBannerTop, kScreenWidth, kFocusBannerLabelHeight, px(kBannerFace));
    text(kBannerTop / kCell + 1, 1, kColumns, kBannerText, kBannerFace, "%s",
         label != nullptr ? label : "");

    // ...and the fade under it, which is the part that actually says "there is
    // something over this page". **It darkens what is already there rather than
    // drawing a colour**, so the map or the palette shows through it -- which is
    // the difference between a shade and a lid.
    //
    // RGB565 unpacked, scaled and repacked per pixel. 320 x 16 is 5,120 of them
    // and this runs when the page redraws, not every frame; the map's own copy
    // next door is 34,944 pixels for comparison.
    const int top = kBannerTop + kFocusBannerLabelHeight;
    for (int row = 0; row < kFocusBannerFadeHeight; ++row) {
        // From fully dark at the label's edge to untouched at the bottom, so
        // the two halves of the banner meet with no seam.
        const int keep = 256 * (row + 1) / (kFocusBannerFadeHeight + 1);
        for (int x = 0; x < kScreenWidth; ++x) {
            gui::Pixel* out = surface.at(x, top + row);
            if (out == nullptr) {
                continue;
            }
            const int r = (*out >> 11) & 0x1F;
            const int g = (*out >> 5) & 0x3F;
            const int b = *out & 0x1F;
            *out = gui::Pixel((((r * keep) >> 8) << 11) | (((g * keep) >> 8) << 5)
                              | ((b * keep) >> 8));
        }
    }
}

}  // namespace mc::ctr::hud
