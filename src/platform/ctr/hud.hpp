#pragma once

// The bottom screen's furniture: the tab strip along the top, the panels and
// slots the pages are built out of, and the two pages that are nothing but
// furniture -- the inventory and the look pad.
//
// **It is drawn into libctru's console framebuffer, and that is still
// deliberate.** The bottom screen belongs to the console -- every message this
// shell says to the player goes through it, and the three debug pages are built
// on it -- so taking it away to make a citro3d target would mean rewriting all
// of that. See map_screen.hpp for the whole of that argument. What is new is
// that the screen is no longer *only* text: panels, slots, frames and the map
// are written straight into the RGB565 framebuffer by the CPU through
// core/gui/paint.hpp, and the console's own glyphs are printed on top of them.
//
// **The two coexist because the console can be told what colour to draw on.**
// libctru's `consoleDrawChar` writes all 64 pixels of a cell -- the glyph in
// `fg` and the rest in `bg` -- so text over a panel used to mean a black
// rectangle punched through it. `\x1b[48;2;R;G;Bm` sets that background to an
// exact RGB565 value (`con_write` packs the three parameters itself), so a row
// of text can be given the panel's own colour and disappear into it. Every
// label here is printed that way, which is why nothing in the layout has to
// route around the character grid.
//
// The layout is 320 x 240, in two bands:
//
//     +----------------------------------------+  y = 0
//     |  [ Map ]  [ Items ]  [ Look ]          |  the tab strip, rows 1-3
//     +----------------------------------------+  y = 24
//     |                                        |
//     |               the page                 |  rows 4-30
//     |                                        |
//     +----------------------------------------+  y = 240
//
// **There was a third band, and it said what the buttons did.** It is gone, and
// the page has the 24 pixels: the hints were the same three lines on every page
// and they were spending a twelfth of the screen to repeat themselves. The
// debug pages still carry theirs, which is where SELECT + Y is worth naming --
// it is the one binding that is not discoverable by touching the screen.

#include "core/gui/paint.hpp"
#include "core/util/types.hpp"

namespace mc::ctr::hud {

// The console's cell, and the screen in the orientation a player looks at it.
inline constexpr int kCell = 8;
inline constexpr int kScreenWidth = 320;
inline constexpr int kScreenHeight = 240;
inline constexpr int kColumns = kScreenWidth / kCell;   // 40
inline constexpr int kRows = kScreenHeight / kCell;     // 30

// The two bands. The tab strip is three character rows so a label sits in the
// middle of it with a row of pixels either side; the page is everything below.
inline constexpr int kTabTop = 0;
inline constexpr int kTabHeight = 24;
inline constexpr int kBodyTop = kTabTop + kTabHeight;   // 24
inline constexpr int kBodyHeight = kScreenHeight - kBodyTop;  // 216

// **a1.1.2's own GUI colours, and only its own.** A panel is the face plus a
// light bevel and a dark one; a slot is the same two bevels the other way
// round. The dark readout is not the original's -- the original has no
// second screen to put one on -- and it is the one place a colour here was
// chosen rather than read: it is what makes a number legible over a picture of
// terrain without a border thick enough to eat the picture.
inline constexpr u32 kBackdrop = 0x2B2B2B;
inline constexpr u32 kPanelFace = 0xC6C6C6;
inline constexpr u32 kPanelLight = 0xFFFFFF;
inline constexpr u32 kPanelDark = 0x555555;
inline constexpr u32 kPanelText = 0x404040;
inline constexpr u32 kSlotFace = 0x8B8B8B;
inline constexpr u32 kSlotDark = 0x373737;
inline constexpr u32 kReadoutFace = 0x171717;
inline constexpr u32 kReadoutText = 0xFFFFFF;
inline constexpr u32 kReadoutLabel = 0x9A9A9A;
inline constexpr u32 kTabIdleFace = 0x8B8B8B;
inline constexpr u32 kTabIdleText = 0x2E2E2E;

// The bottom screen as something that can be drawn on: 240 pixels down a
// column, 320 columns across, bottom-up -- so `strideX` is 240 and `strideY` is
// -1 and (0, 0) is the top-left corner a player sees.
//
// **False rather than a guess** when libctru does not hand back the framebuffer
// this expects. `consoleInit` leaves the screen in RGB565 with double buffering
// off; if either has changed under us, writing pixels at computed offsets is
// the last thing to do about it.
bool bottomSurface(gui::Surface* out);

// One row of text, placed absolutely, clipped and padded to exactly `columns`
// drawn characters, in colours of the caller's choosing. `column` is 1-based,
// like the console's own cursor addressing.
//
// **Padded rather than blanked.** libctru's `\x1b[2K` clears to the end of the
// line, which is forty columns -- the whole screen, map included. Nothing here
// ever writes past the columns it was given.
void text(int row, int column, int columns, u32 fg, u32 bg, const char* fmt, ...)
    __attribute__((format(printf, 6, 7)));

// The same, centred over a span of pixels: the nearest character cell to the
// middle, since a glyph cannot start half way through one.
void textCentred(int row, int left, int width, u32 fg, u32 bg, const char* string);

// **Seven letters drawn as pixels rather than printed**, five wide and seven
// tall, placed anywhere rather than on the character grid.
//
// Two things need that and nothing else does. The compass ribbon's points slide
// by the pixel as the player turns, and a character cell cannot slide. And a
// coordinate readout is 96 pixels wide against a Far Lands coordinate that is
// nine characters long -- so the label beside it cannot afford a whole cell of
// its own, and gets five pixels instead.
enum class Letter { N, E, S, W, X, Y, Z };
void drawLetter(const gui::Surface& surface, int x, int y, Letter letter, u32 colour);
inline constexpr int kLetterWidth = 5;
inline constexpr int kLetterHeight = 7;

// The three shapes every page is made of.
void panel(const gui::Surface& surface, int x, int y, int w, int h);
void slot(const gui::Surface& surface, int x, int y, int w, int h);
void readout(const gui::Surface& surface, int x, int y, int w, int h);

// The backdrop behind all of them: the pack's darkened dirt if there is one,
// and a flat colour if there is not. `tile` is 32 x 32 -- `texture::buildBackground`'s
// own size -- or null.
inline constexpr int kTileEdge = 32;
void drawBackdrop(const gui::Surface& surface, const gui::Pixel* tile);

// The tab strip. **Which tabs there are is the gamemode's business and not
// this file's**: the caller passes the labels it has, so a mode with no
// inventory simply does not offer one.
inline constexpr int kMaxTabs = 4;
struct TabStrip {
    int count = 0;
    const char* labels[kMaxTabs] = {};
    int selected = 0;
};

void drawTabs(const gui::Surface& surface, const TabStrip& tabs);

// Which tab a touch landed on, or -1 for none -- including every touch below
// the strip, which is what keeps a drag on the page from changing the page.
int tabAt(const TabStrip& tabs, int touchX, int touchY);

// **The inventory, drawn empty.** There is no inventory yet -- M3 has not
// started -- so what this page is today is the frame the slots will be dealt
// into, at the size and spacing they will keep: nine across, three rows and a
// hotbar, each slot 24 pixels so a 16-pixel item can be drawn into it at 1.5x
// or a 32-pixel one at 1:1 with room for a border.
void drawItemsPage(const gui::Surface& surface);

// **The look pad**, which is a page rather than a mode because the bottom
// screen is both the game's UI and the only pointing device an old 3DS has, and
// exactly one of the two has to own a drag. Everywhere else the UI owns it;
// here the camera does.
//
// **Two calls, because they change at different rates.** The pad and its
// crosshair are 52,000 pixels and never move, so they are drawn once when the
// page opens. The compass ribbon along the top moves whenever the player turns,
// and it is 9,000 pixels -- so it is its own call, and turning costs a fifth of
// what redrawing the page would.
void drawLookPage(const gui::Surface& surface);
void drawCompassRibbon(const gui::Surface& surface, float yawDegrees);

// The first pixel row of the look pad's drag area. A touch above it belongs to
// the tab strip.
inline constexpr int kLookPadTop = kBodyTop;

}  // namespace mc::ctr::hud
