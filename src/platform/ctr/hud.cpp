#include "platform/ctr/hud.hpp"
#include "platform/ctr/bottom_screen.hpp"

#include "core/block/registry.hpp"
#include "core/gui/item_icon.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/texture_fx.hpp"

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
//
// **Everything hangs off the page's first row**, which is 48 in a mode with a
// hotbar and 8 in one without: the pad is the page and it takes whatever the
// page is given. It stops four pixels short of the tab strip -- a drag area
// that ran into a control would be a drag starting on one -- and four is the
// margin rather than eight, because the eight were dirt nobody was using.
struct LookLayout {
    int padX, padY, padW, padH;
    int ribbonX, ribbonY, ribbonW, ribbonH;
};

constexpr int kRibbonHeight = 32;

constexpr LookLayout lookLayout(int top)
{
    const int padX = 8;
    const int padW = kScreenWidth - padX * 2;
    const int padH = kTabTop - top - 4;
    return LookLayout{padX,      top,        padW,          padH,
                      padX + 8,  top + 8,    padW - 16,     kRibbonHeight};
}

static_assert(lookLayout(kBandedPageTop).padY + lookLayout(kBandedPageTop).padH <= kTabTop,
              "the look pad must clear the tab strip");
static_assert(lookLayout(kBarePageTop).padY >= kBannerHeight,
              "the look pad must clear the reserved row");
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

// **Which shape the screen is in.** One bottom screen, one flag; see the note
// on `setHotbarPresent` in the header for why it is not an argument. It starts
// true because every mode but Spectator has a hotbar and the Overlay sets it
// before the first frame either way.
bool gHotbarPresent = true;

}  // namespace

void setHotbarPresent(bool present) { gHotbarPresent = present; }
bool hotbarPresent() { return gHotbarPresent; }
int hotbarHeight() { return gHotbarPresent ? kHotbarHeight : 0; }
int bannerTop() { return hotbarHeight(); }
int pageTop() { return bannerTop() + kBannerHeight; }

bool bottomSurface(gui::Surface* out)
{
    // **The off-screen picture, not the framebuffer** -- see
    // platform/ctr/bottom_screen.hpp. It is in the framebuffer's orientation,
    // 240 down a column and 320 columns across. Asking for it marks nothing:
    // the overlay asks every frame and mostly draws nothing, and a copy a frame
    // for that would be 150 KB of nothing. A paint ends in `bottom::flush`.
    out->pixels = reinterpret_cast<gui::Pixel*>(bottom::pixels()) + (kScreenHeight - 1);
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
    const LookLayout lay = lookLayout(pageTop());
    readout(surface, lay.padX, lay.padY, lay.padW, lay.padH);
    gui::bevelBox(surface, lay.ribbonX, lay.ribbonY, lay.ribbonW, lay.ribbonH, px(kRibbonFace),
                  px(0x555555), px(0x000000), false);

    // Where the ribbon is read: a notch under it, pointing up at the mark the
    // player is facing. Under it rather than on it, so it never sits on top of
    // a tick and leaves you guessing which one it means -- and outside the
    // rectangle the ribbon clears, because the centre is the one place on it
    // that never moves.
    for (int i = 0; i < 5; ++i) {
        gui::hLine(surface, lay.ribbonX + lay.ribbonW / 2 - i, lay.ribbonY + lay.ribbonH + i,
                   i * 2 + 1, px(0xFFD24A));
    }

    // **The drag area, marked at its corners.** A blank rectangle says nothing
    // about whether it is for touching; four brackets say it is, and they cost
    // no row of text to say it with.
    const int dragTop = lay.ribbonY + lay.ribbonH + 8;
    const int dragLeft = lay.padX + 8;
    const int dragRight = lay.padX + lay.padW - 9;
    const int dragBottom = lay.padY + lay.padH - 9;
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
    const LookLayout lay = lookLayout(pageTop());

    // Cleared to the ribbon's own face rather than redrawn as a box, so the
    // bevel around it survives a turn.
    gui::fillRect(surface, lay.ribbonX + 1, lay.ribbonY + 1, lay.ribbonW - 2, lay.ribbonH - 2,
                  px(kRibbonFace));

    // **The bearing, not the yaw.** Minecraft's yaw is zero looking south and
    // increases towards west; a compass counts clockwise from north. The two
    // differ by half a turn, which is why this is an offset rather than a
    // table.
    float bearing = std::fmod(yawDegrees + 180.0f, 360.0f);
    if (bearing < 0.0f) {
        bearing += 360.0f;
    }

    const int centreX = lay.ribbonX + lay.ribbonW / 2;
    const gui::Pixel tickColour = px(0x7A7A7A);
    const gui::Pixel pointColour = px(0xFFFFFF);

    // Every fifteen degrees, placed by the pixel rather than by the cell, which
    // is the whole reason the four letters below are drawn here instead of
    // printed: a character cell cannot slide.
    for (int degrees = 0; degrees < 360; degrees += 15) {
        float offset = float(degrees) - bearing;
        offset = std::fmod(offset + 540.0f, 360.0f) - 180.0f;
        const int x = centreX + int(std::lround(offset * kPixelsPerDegree));
        if (x < lay.ribbonX + 2 || x >= lay.ribbonX + lay.ribbonW - 2) {
            continue;
        }
        const bool cardinal = (degrees % 45) == 0;
        gui::vLine(surface, x, lay.ribbonY + lay.ribbonH - (cardinal ? 12 : 7), cardinal ? 10 : 5,
                   cardinal ? pointColour : tickColour);
        if (!cardinal) {
            continue;
        }
        const u8* const* glyphs = kPointGlyphs[degrees / 45];
        const int letters = glyphs[1] != nullptr ? 2 : 1;
        const int width = letters * kGlyphWidth + (letters - 1);
        int letterX = x - width / 2;
        for (int i = 0; i < letters; ++i) {
            drawGlyph(surface, letterX, lay.ribbonY + 6, glyphs[i], pointColour);
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
constexpr int kPalGridX = kPalPanelX + 8;

// **The vertical half is a function of where the page starts**, which is 48 in
// a mode with a hotbar and 8 in one without. The panel runs to four pixels
// short of the tab strip rather than eight: the eight were backdrop with
// nothing in it.
struct PaletteLayout {
    int panelY, panelH;
    int gridY;
    int titleRow;
    int captionRow;
};

constexpr PaletteLayout paletteLayout(int top)
{
    const int panelH = kTabTop - top - 4;
    const int gridY = top + 24;  // under the title row
    // The first whole character row under the grid. `text` rows are 1-based, so
    // the +1 is what puts it below the last slot instead of across it.
    return PaletteLayout{top, panelH, gridY, top / kCell + 2,
                         (gridY + kPaletteRows * kSlotPixels) / kCell + 1};
}

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
constexpr int kArrowInset = (kArrowW - kCell) / 2;
constexpr int arrowY(int top) { return top + 4; }
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
// **Checked at both page tops**, which is what makes neither of the screen's
// two shapes the untested one.
constexpr bool paletteFits(int top)
{
    const PaletteLayout lay = paletteLayout(top);
    return arrowY(top) + kArrowH <= lay.gridY && lay.panelY >= kBannerHeight
           && lay.panelY + lay.panelH <= kTabTop
           && lay.gridY + kPaletteRows * kSlotPixels <= lay.panelY + lay.panelH
           && lay.captionRow * kCell <= lay.panelY + lay.panelH;
}
static_assert(paletteFits(kBandedPageTop), "the palette must fit under a hotbar");
static_assert(paletteFits(kBarePageTop), "the palette must fit without one");

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

// **The wear bar under a damaged item**, which is `ab.b(kd,ey,ev,II)V` --
// RenderItem.renderItemOverlayIntoGUI -- and it is the one part of that method
// this screen had never drawn. A tool that has been used is otherwise
// indistinguishable from a new one until it breaks in your hand.
//
// The jar's arithmetic exactly, and it is integer arithmetic rather than the
// `Math.round` of later versions:
//
//     int j = 13 - damage * 13 / maxDamage;     // how much bar is left
//     int k = 255 - damage * 255 / maxDamage;   // how green it still is
//
// then three quads over a 16-texel icon, all at (x + 2, y + 13): a black one 13
// wide and 2 tall, a dark one 12 wide and 1 tall in `(255 - k) / 4 << 16 |
// 16128`, and the bar itself `j` wide and 1 tall in `(255 - k) << 16 | k << 8`.
// So the empty part is a quarter-bright dark green and the full part runs green
// to red as the tool wears -- and the black quad's second row is what gives the
// whole thing an edge underneath.
//
// The icon is drawn larger than 16 pixels here, so every number above is scaled
// by the icon's own size. A bar row can round to nothing on a small cell, which
// is why the heights floor at one.
void drawWearBar(const gui::Surface& surface, int iconX, int iconY, int iconSize, int damage,
                 int maxDamage)
{
    if (damage <= 0 || maxDamage <= 0) {
        return;
    }
    if (damage > maxDamage) {
        damage = maxDamage;
    }
    constexpr int kIconTexels = 16;
    // **Texel *edges* rather than texel counts**, which is what keeps the three
    // quads in proportion at a size that is not a whole multiple of 16: the
    // black quad is two texel rows tall and the coloured one is the first of
    // them, so scaling each edge and subtracting gives 3 and 2 at a 24-pixel
    // icon rather than 3 and 1. A row still floors at one pixel, because an
    // icon small enough to round the bar away should show a thin bar and not
    // none.
    const auto edge = [&](int texel) { return texel * iconSize / kIconTexels; };
    const auto span = [&](int from, int to) {
        const int pixels = edge(to) - edge(from);
        return pixels > 0 ? pixels : 1;
    };
    const int j = 13 - damage * 13 / maxDamage;
    const int k = 255 - damage * 255 / maxDamage;

    const int left = iconX + edge(2);
    const int top = iconY + edge(13);
    gui::fillRect(surface, left, top, span(2, 15), span(13, 15), px(0x000000));
    gui::fillRect(surface, left, top, span(2, 14), span(13, 14),
                  px(u32(((255 - k) / 4) << 16) | 0x3F00u));
    if (j > 0) {
        gui::fillRect(surface, left, top, span(2, 2 + j), span(13, 14),
                      px(u32((255 - k) << 16) | u32(k << 8)));
    }
}

// One cell of a grid: the slot, the item's icon in the middle of it, its count
// if it has more than one, the wear bar if it has been used, and whichever of
// the two edges apply.
void drawCell(const gui::Surface& surface, int x, int y, int w, int h,
              const gui::IconSheets& sheets, item::ItemId id, int count, int damage,
              bool selected, bool cursor)
{
    slot(surface, x, y, w, h);
    if (id != 0) {
        // The icon is centred rather than stretched: a cell that is 35 wide and
        // 28 tall is not square, and an icon drawn to fill it would be too.
        const int size = kIconPixels < w - 2 ? kIconPixels : w - 2;
        const int fit = size < h - 2 ? size : h - 2;
        const int iconX = x + (w - fit) / 2;
        const int iconY = y + (h - fit) / 2;
        gui::drawItemIcon(surface, iconX, iconY, fit, sheets, id);
        drawWearBar(surface, iconX, iconY, fit, damage, int(item::def(id).durability));
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
              const gui::IconSheets& sheets, item::ItemId id, int count, int damage,
              bool selected, bool cursor)
{
    drawCell(surface, x, y, size, size, sheets, id, count, damage, selected, cursor);
}

// How far the carried stack is lifted off the cell it is hovering over, before
// the clamp below. Three pixels is what a 24-pixel icon has to spare inside a
// 30-pixel slot, so this is an intent rather than a measurement: a bigger cell
// would use all of it.
constexpr int kCarriedLift = 4;

// **The stack on the cursor, drawn over the cell it is hovering on** rather
// than in the slot it came out of -- which is what makes a move in progress
// visible at all. The source slot is drawn hollow (see `held` below), so the
// stack is on the screen exactly once.
//
// Up and to the left of centre, and never outside the cell: every cell around
// this one is already drawn and this is the last thing on the page, so an icon
// that overflowed would land on a neighbour and stay there until the next full
// redraw. The cell's own count is suppressed by the caller for the same reason
// two numbers in one corner would be unreadable -- the one that matters while a
// stack is in hand is the one in hand.
void drawCarried(const gui::Surface& surface, int x, int y, int w, int h,
                 const gui::IconSheets& sheets, const item::ItemStack& stack)
{
    if (stack.empty()) {
        return;
    }
    const int size = kIconPixels < w - 2 ? kIconPixels : w - 2;
    const int fit = size < h - 2 ? size : h - 2;
    const int marginX = (w - fit) / 2;
    const int marginY = (h - fit) / 2;
    const int liftX = marginX < kCarriedLift ? marginX : kCarriedLift;
    const int liftY = marginY < kCarriedLift ? marginY : kCarriedLift;
    const int iconX = x + marginX - liftX;
    const int iconY = y + marginY - liftY;
    gui::drawItemIcon(surface, iconX, iconY, fit, sheets, item::ItemId(stack.id));
    drawWearBar(surface, iconX, iconY, fit, int(stack.damage),
                int(item::def(item::ItemId(stack.id)).durability));
    // Lifted with the icon, so the number sits under the stack it belongs to
    // rather than in the corner of the slot the stack is only passing over.
    drawCount(surface, x - liftX, y - liftY, w, h, int(stack.count));
}

bool insideBox(int px_, int py_, int x, int y, int w, int h)
{
    return px_ >= x && px_ < x + w && py_ >= y && py_ < y + h;
}

}  // namespace

namespace {

// **The Items page fills the page band**, which is 320 x 168 between the
// reserved row and the tab strip. Nine columns of backpack at 32 pixels is 288, an
// armour column of the same beside it is 320, and 320 is the screen -- so the
// two grids are laid out from the panel's inside edges rather than centred, and
// the panel is the page.
//
// **At file scope so the drawing and the hit test share them**, which they
// previously did not: `itemsCellAt` used to be able to disagree with
// `drawItemsPage` about where the grid was, and a hit test that is a few pixels
// out is the kind of bug that is felt as "the touchscreen is bad".
constexpr int kItemsPanelX = 1;
constexpr int kItemsPanelW = kScreenWidth - 2;                       // 318
constexpr int kItemsGridWidth = kItemsColumns * kItemsSlotPixels;    // 279
constexpr int kItemsGridX = kScreenWidth - 2 - kItemsGridWidth;      // 39
constexpr int kItemsGridHeight = kItemsRows * kItemsSlotPixels;      // 93

// The armour column is on the left, where a paper doll would be, and the
// backpack fills what is left. Four armour slots stand taller than three rows
// of backpack, so the panel's height is the armour's.
constexpr int kItemsArmourX = kItemsPanelX + 4;                      // 5

// **The vertical half hangs off the page's first row**, as the palette's does.
struct ItemsLayout {
    int panelY, panelH;
    int titleRow;
    int gridY;      // the backpack, and the armour column beside it
    int captionRow;
};

constexpr ItemsLayout itemsLayout(int top)
{
    const int gridY = top + 22;
    return ItemsLayout{top, kTabTop - top - 1, top / kCell + 1, gridY,
                       (gridY + kItemsGridHeight) / kCell + 2};
}

constexpr bool itemsFit(int top)
{
    const ItemsLayout lay = itemsLayout(top);
    return lay.panelY >= kBannerHeight && lay.panelY + lay.panelH <= kTabTop
           && lay.gridY + item::kArmourSlots * kItemsSlotPixels <= lay.panelY + lay.panelH
           && lay.captionRow * kCell <= lay.panelY + lay.panelH;
}
static_assert(itemsFit(kBandedPageTop), "the inventory must fit under a hotbar");
static_assert(itemsFit(kBarePageTop), "the inventory must fit without one");
static_assert(kItemsArmourX + kItemsSlotPixels <= kItemsGridX,
              "the armour column must not run into the backpack grid");
static_assert(kItemsGridX + kItemsGridWidth <= kItemsPanelX + kItemsPanelW,
              "the backpack grid must fit the panel");
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

int itemsCellForSlot(int slot)
{
    if (slot >= item::kHotbarSlots && slot < item::kMainSlots) {
        return slot - item::kHotbarSlots;
    }
    // `armourIndexForRow` is its own inverse -- it is `n - 1 - i` -- so the row
    // a slot is drawn on goes through the same line the slot a row addresses
    // does, and the two cannot drift apart.
    if (slot >= item::kArmourBase && slot < item::kArmourBase + item::kArmourSlots) {
        return item::kBackpackSlots + armourIndexForRow(slot - item::kArmourBase);
    }
    return -1;
}

void drawItemsPage(const gui::Surface& surface, const item::Inventory& inventory,
                   const gui::IconSheets& sheets, int cursor, int held, int carried)
{
    const ItemsLayout lay = itemsLayout(pageTop());
    panel(surface, kItemsPanelX, lay.panelY, kItemsPanelW, lay.panelH);
    textCentred(lay.titleRow, kItemsPanelX, kItemsPanelW, kPanelText, kPanelFace, "Inventory");

    // The backpack: slots 9..35, three rows of nine.
    for (int row = 0; row < kItemsRows; ++row) {
        for (int column = 0; column < kItemsColumns; ++column) {
            const int cell = row * kItemsColumns + column;
            const int slotNumber = itemsSlotForCell(cell);
            const item::ItemStack& stack = inventory.at(slotNumber);
            // A stack that has been picked up is drawn as an empty cell with
            // its outline still on it: it is hovering over `carried` below, so
            // leaving its icon behind would show it in two places at once.
            const bool lifted = slotNumber == held;
            drawCell(surface, kItemsGridX + column * kItemsSlotPixels,
                     lay.gridY + row * kItemsSlotPixels, kItemsSlotPixels, sheets,
                     lifted || stack.empty() ? item::ItemId(0) : item::ItemId(stack.id),
                     // No count on the cell the carried stack is hovering over:
                     // its own number goes there instead. See `drawCarried`.
                     lifted || cell == carried ? 0 : int(stack.count), int(stack.damage), false,
                     cell == cursor);
        }
    }

    // **The armour**, slots 100..103, which `Inventory` has held since the item
    // table landed and which round-trip through `level.dat`. It is worn now:
    // `player_vitals.cpp`'s `armourValue` reads these four and `damageArmour`
    // wears them, so a helmet in slot 103 takes a share of every hit. Creative
    // carries it and is invulnerable, which is the mode rather than the slot.
    for (int row = 0; row < item::kArmourSlots; ++row) {
        const int cell = item::kBackpackSlots + row;
        const int slotNumber = itemsSlotForCell(cell);
        const item::ItemStack& stack = inventory.at(slotNumber);
        const bool lifted = slotNumber == held;
        drawCell(surface, kItemsArmourX, lay.gridY + row * kItemsSlotPixels, kItemsSlotPixels,
                 sheets, lifted || stack.empty() ? item::ItemId(0) : item::ItemId(stack.id),
                 lifted || cell == carried ? 0 : int(stack.count), int(stack.damage), false,
                 cell == cursor);
    }

    // **Last, over everything.** The stack in hand belongs on top of the grid
    // rather than in it, and the cell it is over has already been drawn.
    if (carried >= 0 && held >= 0) {
        const item::ItemStack& stack = inventory.at(held);
        if (carried < item::kBackpackSlots) {
            drawCarried(surface, kItemsGridX + (carried % kItemsColumns) * kItemsSlotPixels,
                        lay.gridY + (carried / kItemsColumns) * kItemsSlotPixels,
                        kItemsSlotPixels, kItemsSlotPixels, sheets, stack);
        } else if (carried < kItemsCells) {
            drawCarried(surface, kItemsArmourX,
                        lay.gridY + (carried - item::kBackpackSlots) * kItemsSlotPixels,
                        kItemsSlotPixels, kItemsSlotPixels, sheets, stack);
        }
    }

    // Inside the panel rather than under it, dimmer than the title, because it
    // is an explanation rather than a heading -- and because a row of text
    // outside the panel would have to guess the backdrop's colour, which is the
    // pack's dirt and not a constant.
    textCentred(lay.captionRow, kItemsGridX, kItemsGridWidth, kPanelDark, kPanelFace,
                held >= 0 ? "A puts it down, X throws it" : "A picks a stack up");
}

int itemsCellAt(int touchX, int touchY)
{
    const ItemsLayout lay = itemsLayout(pageTop());
    if (insideBox(touchX, touchY, kItemsGridX, lay.gridY, kItemsGridWidth, kItemsGridHeight)) {
        return ((touchY - lay.gridY) / kItemsSlotPixels) * kItemsColumns
               + (touchX - kItemsGridX) / kItemsSlotPixels;
    }
    if (insideBox(touchX, touchY, kItemsArmourX, lay.gridY, kItemsSlotPixels,
                  item::kArmourSlots * kItemsSlotPixels)) {
        return item::kBackpackSlots + (touchY - lay.gridY) / kItemsSlotPixels;
    }
    return -1;
}

void drawHotbar(const gui::Surface& surface, const item::Inventory& inventory,
                const gui::IconSheets& sheets, int cursor, int held, int carried)
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
                 lifted || i == carried ? 0 : int(stack.count), int(stack.damage),
                 i == inventory.selected, i == cursor);
    }

    // The stack in hand, hovering over the band rather than over the grid --
    // the two calls are handed the same carried stack and only one of them is
    // ever told where it is. See `Overlay::carriedPosition`.
    if (carried >= 0 && carried < kHotbarColumns && held >= 0) {
        drawCarried(surface, hotbarSlotX(carried), kHotbarSlotY, hotbarSlotWidth(carried),
                    kHotbarSlotHeight, sheets, inventory.at(held));
    }
}

namespace {

static_assert(gui::kLayoutScreenWidth == kScreenWidth && gui::kLayoutHotbarSlotY == kHotbarSlotY
                  && gui::kLayoutHotbarSlotHeight == kHotbarSlotHeight
                  && gui::kLayoutPageTop == kBandedPageTop && gui::kLayoutPageBottom == kTabTop,
              "the container layout must agree with the bands it is laid out in");

constexpr u32 kProgressFill = 0xFFFFFF;

// An arrow pointing right across `r`: a shaft a third of its height, then a
// head as tall as the box. The first `filled` columns are drawn in `fill` and
// the rest in `empty`, which is the furnace's cook progress; a crafting arrow
// is all one colour.
void drawLayoutArrow(const gui::Surface& surface, const gui::SlotRect& r, int filled, u32 fill,
                     u32 empty)
{
    const int head = r.h / 2 + 1;
    const int shaft = r.w - head;
    const int middle = r.y + r.h / 2;
    const int shaftHalf = r.h / 6;
    for (int c = 0; c < r.w; ++c) {
        const gui::Pixel colour = px(c < filled ? fill : empty);
        int half = shaftHalf;
        if (c >= shaft) {
            half = (r.h / 2) * (head - (c - shaft)) / head;
        }
        gui::vLine(surface, r.x + c, middle - half, 2 * half + 1, colour);
    }
}

// The flame is the same tile the fire block and a burning entity draw --
// `texture::flameTile(0)`, settled into the decoded atlas at pack load the
// way every other icon on this screen is. Fuel drains it from the top, so the
// gauge reveals more of a fixed flame image rather than rescaling one, the
// same sense `height` already had when this filled a flat square.
void drawFlame(const gui::Surface& surface, const gui::SlotRect& r, int height,
               const gui::IconSheets& sheets)
{
    gui::fillRect(surface, r.x, r.y, r.w, r.h, px(kPanelFace));
    gui::frameRect(surface, r.x, r.y, r.w, r.h, px(kSlotFace));
    if (height <= 0) {
        return;
    }
    const int tile = texture::flameTile(0);
    if (tile < 0) {
        return;
    }
    gui::drawTerrainTileBottom(surface, r.x + 2, r.y + 1, r.w - 4, r.h - 2, sheets.terrain, tile,
                               height);
}

// **The chest's scroll furniture**, which nothing but a chest joined to more
// than one other ever shows. Two arrow buttons in slot bevels with a triangle
// in each, and between them a sunken track with a thumb as long a fraction of
// it as the window is of the chest -- so the bar says both "there is more" and
// "this much more", which an arrow on its own does not.
//
// A triangle rather than a `^` glyph: the character grid is eight pixels and
// these boxes are sixteen on a four-pixel offset, so a letter could not be
// centred in one without moving the box off the grid the slots are on.
void drawScrollArrow(const gui::Surface& surface, const gui::SlotRect& r, bool up, bool live)
{
    slot(surface, r.x, r.y, r.w, r.h);
    const gui::Pixel colour = px(live ? kPanelText : kPanelDark);
    // Five rows, widening by two a row from the point: 1, 3, 5, 7, 9 pixels in
    // a sixteen-wide box.
    constexpr int kRows = 5;
    const int centre = r.x + r.w / 2;
    const int top = r.y + (r.h - kRows) / 2;
    for (int i = 0; i < kRows; ++i) {
        const int width = 1 + 2 * i;
        const int y = up ? top + i : top + (kRows - 1 - i);
        gui::hLine(surface, centre - width / 2, y, width, colour);
    }
}

void drawScrollBar(const gui::Surface& surface, const gui::ContainerLayout& layout)
{
    if (layout.scrollUp.w <= 0) {
        return;
    }
    drawScrollArrow(surface, layout.scrollUp, true, layout.chestFirstRow > 0);
    drawScrollArrow(surface, layout.scrollDown, false,
                    layout.chestFirstRow + layout.chestWindowRows < layout.chestRows);
    if (layout.scrollTrack.w > 0) {
        // **A sunken dark track with a raised thumb in it.** The first version
        // filled the thumb flat in the panel's own face, which is the colour of
        // the page behind it -- so the thumb read as a hole and the track read
        // as the bar. The bevel is what says which of the two is the control.
        const gui::SlotRect& t = layout.scrollTrack;
        readout(surface, t.x, t.y, t.w, t.h);
        const gui::SlotRect& thumb = layout.scrollThumb;
        gui::bevelBox(surface, thumb.x, thumb.y, thumb.w, thumb.h, px(kSlotFace),
                      px(kPanelFace), px(kSlotDark), true);
    }
}

void drawProgress(const gui::Surface& surface, const gui::ContainerLayout& layout,
                  const item::ContainerSession& session, const gui::IconSheets& sheets)
{
    if (layout.arrow.w > 0) {
        if (layout.arrowIsProgress) {
            gui::fillRect(surface, layout.arrow.x, layout.arrow.y, layout.arrow.w,
                          layout.arrow.h, px(kPanelFace));
            drawLayoutArrow(surface, layout.arrow, session.furnaceCookScaled(layout.arrow.w),
                            kProgressFill, kSlotFace);
        } else {
            drawLayoutArrow(surface, layout.arrow, 0, kPanelDark, kPanelDark);
        }
    }
    if (layout.flame.w > 0) {
        // Two pixels of frame off the top and bottom.
        drawFlame(surface, layout.flame, session.furnaceBurnScaled(layout.flame.h - 2), sheets);
    }
}

}  // namespace

void drawContainerPage(const gui::Surface& surface, const gui::ContainerLayout& layout,
                       const item::ContainerSession& session, const item::Inventory& inventory,
                       const gui::IconSheets& sheets, int cursor, bool showCursor)
{
    panel(surface, layout.panel.x, layout.panel.y, layout.panel.w, layout.panel.h);
    for (int i = 0; i < layout.titles; ++i) {
        text(layout.titleRow[i], layout.titleColumn[i], int(std::strlen(layout.title[i])),
             kPanelText, kPanelFace, "%s", layout.title[i]);
    }

    const item::ItemStack& carried = session.cursor();
    const int pageSlots = layout.slots - item::kHotbarSlots;
    for (int i = 0; i < pageSlots; ++i) {
        const gui::SlotRect& r = layout.rect[i];
        if (r.w <= 0) {
            continue;  // a chest row scrolled out of the window
        }
        const item::ItemStack& stack = session.slotAt(inventory, i);
        const bool here = i == cursor;
        drawCell(surface, r.x, r.y, r.w, r.h, sheets,
                 stack.empty() ? item::ItemId(0) : item::ItemId(stack.id),
                 here && !carried.empty() ? 0 : int(stack.count), int(stack.damage), false,
                 here && showCursor);
    }
    drawProgress(surface, layout, session, sheets);
    drawScrollBar(surface, layout);

    if (cursor >= 0 && cursor < pageSlots && !carried.empty()) {
        const gui::SlotRect& r = layout.rect[cursor];
        if (r.w > 0) {
            drawCarried(surface, r.x, r.y, r.w, r.h, sheets, carried);
        }
    }
}

void drawContainerBand(const gui::Surface& surface, const gui::ContainerLayout& layout,
                       const item::ContainerSession& session, const item::Inventory& inventory,
                       const gui::IconSheets& sheets, int cursor, bool showCursor)
{
    const int first = layout.slots - item::kHotbarSlots;
    const int hand = cursor >= first && cursor < layout.slots ? cursor - first : -1;
    const bool carrying = !session.cursor().empty();
    // `held` -1: nothing is lifted out of a slot on these screens, the cursor
    // holds its own stack. `carried` still names the cell, so its count gives
    // way to the one in hand.
    drawHotbar(surface, inventory, sheets, showCursor ? hand : -1, -1, carrying ? hand : -1);
    if (carrying && hand >= 0) {
        drawCarried(surface, hotbarSlotX(hand), kHotbarSlotY, hotbarSlotWidth(hand),
                    kHotbarSlotHeight, sheets, session.cursor());
    }
}

void drawContainerProgress(const gui::Surface& surface, const gui::ContainerLayout& layout,
                           const item::ContainerSession& session, const gui::IconSheets& sheets)
{
    drawProgress(surface, layout, session, sheets);
}

int hotbarSlotAt(int touchX, int touchY)
{
    // A mode with no hotbar has no band to touch, and the pages start where it
    // would have been -- so this has to refuse before it measures.
    if (!hotbarPresent()
        || !insideBox(touchX, touchY, 0, kHotbarTop, kScreenWidth, kHotbarHeight)) {
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
    const PaletteLayout lay = paletteLayout(pageTop());
    panel(surface, kPalPanelX, lay.panelY, kPalPanelW, lay.panelH);

    const int pages = palettePageCount();
    const int shown = page < 0 ? 0 : (page >= pages ? pages - 1 : page);

    char title[40];
    std::snprintf(title, sizeof title, "Blocks  %d/%d", shown + 1, pages);
    textCentred(lay.titleRow, kPalPanelX, kPalPanelW, kPanelText, kPanelFace, title);

    // The arrows are drawn on every page, including the ones where they do
    // nothing, and dimmed where they do. A control that disappears at the end
    // of a list moves the other one, and a control that moves is a control that
    // gets mis-tapped.
    for (int side = 0; side < 2; ++side) {
        const int x = side == 0 ? kArrowLeftX : kArrowRightX;
        const bool live = side == 0 ? shown > 0 : shown + 1 < pages;
        slot(surface, x, arrowY(lay.panelY), kArrowW, kArrowH);
        text(lay.titleRow, arrowColumn(x), 1, live ? kPanelText : kPanelDark, kSlotFace,
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
            drawCell(surface, kPalGridX + column * kSlotPixels, lay.gridY + row * kSlotPixels,
                     kSlotPixels, sheets, id, 1, 0, id != 0 && id == selected, cell == cursor);
        }
    }

    // **The one place a block's name fits**, and the reason the page has a
    // caption at all: 45 cells of 16-pixel tile are not self-describing, and
    // "which of these two greys is gravel" is a question a picture cannot
    // answer. The caller supplies it, because what it names is whatever the
    // player is pointing at and this file does not know what that is.
    textCentred(lay.captionRow, kPalPanelX, kPalPanelW, kPanelText, kPanelFace,
                caption != nullptr ? caption : "");
}

int paletteCellAt(int touchX, int touchY)
{
    const PaletteLayout lay = paletteLayout(pageTop());
    if (!insideBox(touchX, touchY, kPalGridX, lay.gridY, kPaletteColumns * kSlotPixels,
                   kPaletteRows * kSlotPixels)) {
        return -1;
    }
    const int column = (touchX - kPalGridX) / kSlotPixels;
    const int row = (touchY - lay.gridY) / kSlotPixels;
    return row * kPaletteColumns + column;
}

int paletteArrowAt(int touchX, int touchY)
{
    // Generously larger than what is drawn, in both directions: the box is 16
    // pixels and a fingertip on a resistive screen is not.
    constexpr int kPad = 4;
    const int y = arrowY(pageTop());
    if (insideBox(touchX, touchY, kArrowLeftX - kPad, y - kPad, kArrowW + kPad * 2,
                  kArrowH + kPad * 2)) {
        return -1;
    }
    if (insideBox(touchX, touchY, kArrowRightX - kPad, y - kPad, kArrowW + kPad * 2,
                  kArrowH + kPad * 2)) {
        return 1;
    }
    return 0;
}


namespace {

// The game-over panel, on the character grid so every line of text lands on a
// whole cell and the face behind it is one colour.
constexpr int kGameOverPanelX = 40;
constexpr int kGameOverPanelW = 240;
constexpr int kGameOverPanelH = 144;
constexpr int kGameOverButtonX = 64;
constexpr int kGameOverButtonW = 192;
constexpr int kGameOverButtonH = 24;

// Eight pixels into the page, wherever the page starts.
int gameOverPanelY() { return pageTop() + 8; }
int gameOverButtonY(int index) { return gameOverPanelY() + (index == 0 ? 64 : 96); }

// The text row a pixel band of `height` starting at `top` is centred on.
int rowCentredIn(int top, int height)
{
    return (top + (height - kCell) / 2) / kCell + 1;
}

}  // namespace

void drawGameOverPage(const gui::Surface& surface, int score, int cursor)
{
    const int panelY = gameOverPanelY();
    panel(surface, kGameOverPanelX, panelY, kGameOverPanelW, kGameOverPanelH);
    textCentred((panelY + 16) / kCell + 1, kGameOverPanelX, kGameOverPanelW, kPanelText,
                kPanelFace, "Game over!");

    // `"Score: &e" + player.getScore()` -- the `&e` is the colour code for
    // yellow in the original's font, and yellow on a light panel is unreadable,
    // so the number takes the panel's ink instead.
    char line[32];
    std::snprintf(line, sizeof(line), "Score: %d", score);
    textCentred((panelY + 36) / kCell + 1, kGameOverPanelX, kGameOverPanelW, kPanelText,
                kPanelFace, line);

    const char* labels[2] = {"Respawn", "Title menu"};
    for (int i = 0; i < 2; ++i) {
        const int y = gameOverButtonY(i);
        slot(surface, kGameOverButtonX, y, kGameOverButtonW, kGameOverButtonH);
        if (i == cursor) {
            slotEdge(surface, kGameOverButtonX, y, kGameOverButtonW, kGameOverButtonH,
                     kCursorEdge);
        }
        textCentred(rowCentredIn(y, kGameOverButtonH), kGameOverButtonX, kGameOverButtonW,
                    kReadoutText, kSlotFace, labels[i]);
    }
}

int gameOverButtonAt(int touchX, int touchY)
{
    for (int i = 0; i < 2; ++i) {
        const int y = gameOverButtonY(i);
        if (touchX >= kGameOverButtonX && touchX < kGameOverButtonX + kGameOverButtonW
            && touchY >= y && touchY < y + kGameOverButtonH) {
            return i;
        }
    }
    return -1;
}

}  // namespace mc::ctr::hud
