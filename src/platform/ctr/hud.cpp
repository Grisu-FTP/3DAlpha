#include "platform/ctr/hud.hpp"

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
constexpr int kPadY = 32;
constexpr int kPadW = kScreenWidth - kPadX * 2;
constexpr int kPadH = 200;
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
        if (chosen) {
            gui::bevelBox(surface, left, 0, width, kTabHeight, px(kPanelFace), px(kPanelLight),
                          px(kPanelDark), true);
        } else {
            gui::bevelBox(surface, left, 4, width, kTabHeight - 4, px(kTabIdleFace), px(kSlotFace),
                          px(kSlotDark), true);
        }

        textCentred(2, left, width, chosen ? kPanelText : kTabIdleText,
                    chosen ? kPanelFace : kTabIdleFace, tabs.labels[i]);
    }
}

int tabAt(const TabStrip& tabs, int touchX, int touchY)
{
    if (tabs.count <= 0 || touchY < 0 || touchY >= kTabHeight) {
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

void drawItemsPage(const gui::Surface& surface)
{
    // Nine slots across at 24 pixels, three rows and a hotbar under them, which
    // is the original's layout at the largest size a 320-pixel screen will take
    // it. The panel is eight pixels of margin around that, a title row above
    // and a line below saying why the slots are empty.
    constexpr int kSlotPixels = 24;
    constexpr int kSlotColumns = 9;
    constexpr int kGridWidth = kSlotColumns * kSlotPixels;
    constexpr int kPanelX = (kScreenWidth - kGridWidth) / 2 - 8;
    constexpr int kPanelW = kGridWidth + 16;
    constexpr int kPanelY = 56;
    constexpr int kPanelH = 152;
    constexpr int kGridX = kPanelX + 8;
    constexpr int kGridY = 80;
    constexpr int kHotbarY = kGridY + 3 * kSlotPixels + 8;

    panel(surface, kPanelX, kPanelY, kPanelW, kPanelH);
    textCentred(9, kPanelX, kPanelW, kPanelText, kPanelFace, "Inventory");

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < kSlotColumns; ++column) {
            slot(surface, kGridX + column * kSlotPixels, kGridY + row * kSlotPixels, kSlotPixels,
                 kSlotPixels);
        }
    }
    for (int column = 0; column < kSlotColumns; ++column) {
        slot(surface, kGridX + column * kSlotPixels, kHotbarY, kSlotPixels, kSlotPixels);
    }

    // Row 25 is the eight pixels under the hotbar. Dimmer than the title,
    // because it is an apology rather than a heading.
    textCentred(25, kPanelX, kPanelW, kPanelDark, kPanelFace, "empty until M3");
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

}  // namespace mc::ctr::hud
