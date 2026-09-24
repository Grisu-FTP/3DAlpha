#pragma once

// The bottom screen's furniture: the hotbar along the top, the tab strip along
// the bottom, the panels and slots the pages are built out of, and the two
// pages that are nothing but furniture -- the inventory and the look pad.
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
// The layout is 320 x 240, in four bands:
//
//     +----------------------------------------+  y = 0
//     |  the hotbar, nine slots edge to edge    |  the hand
//     +----------------------------------------+  y = 40
//     |                                        |  a reserved row, backdrop
//     +----------------------------------------+  y = 48
//     |                                        |
//     |               the page                 |
//     |                                        |
//     +----------------------------------------+  y = 216
//     |  [ Inv. ] [ Items ] [ Map ] [ Look ]   |  the tab strip
//     +----------------------------------------+  y = 240
//
// ...in a gamemode that has a hotbar. **Spectator has none, and the whole
// arrangement above it moves up forty pixels**: the reserved row goes to y = 0
// and the page starts at y = 8 with 208 rows instead of 168. See `pageTop`.
//
// **The hotbar and the tab strip changed places**, and the hotbar grew to the
// full width of the screen while it was at it. Both were asked for and both are
// improvements on what was there, for reasons worth writing down:
//
//   * The hotbar is the one control on this screen that is read while the
//     player is looking at the *top* screen -- it is what is in your hand. At
//     the top of the bottom screen it is as close to the world as a second
//     screen can put it; at the bottom it was as far away as it could be.
//   * The tab strip is a control you look at deliberately, so the bottom is
//     where it costs least, and it is now next to the thumbs rather than under
//     the eyes.
//   * Nine slots across 320 pixels is 35.55, which does not divide. The slots
//     are therefore **not all the same width**: `hotbarSlotX` is
//     `i * 320 / 9`, so the widths come out 35 or 36 and the row lands exactly
//     on both edges with no leftover margin. A row of nine 35s centred would
//     have left five pixels of nothing, and five pixels of nothing at the edge
//     of a touch target is five pixels a finger can miss.
//
// **There was a third band once and it said what the buttons did.** It is gone:
// the hints were the same three lines on every page and they were spending a
// twelfth of the screen to repeat themselves. The debug pages still carry
// theirs, which is where SELECT + Y is worth naming -- it is the one binding
// that is not discoverable by touching the screen.
//
// **The hotbar band used to be reserved in every gamemode**, on the argument
// that a page whose height depended on the mode would be two layouts, two sets
// of constants and two map window sizes to measure against the console.
// Spectator got 32 pixels of backdrop and a smaller map for the sake of one
// number.
//
// It is two layouts now, and the argument is answered rather than ignored:
// every page is laid out by a `constexpr` function of the page's first row, so
// there is one set of constants written once and evaluated twice, and both
// answers are checked at compile time by static assertions against
// `kBandedPageTop` and `kBarePageTop`. The map window is the one place that
// really is two sizes, and the cost of the larger one is written down beside
// it in map_screen.hpp.
//
// **It is here rather than over the world**, which is docs/3ds-performance.md
// section 11 and not a new decision: the top screen draws nothing but the
// world, so the HUD costs no overdraw at all on a fill-bound device. It is also
// the only screen that can be touched.

#include "core/block/block_def.hpp"
#include "core/gui/container_layout.hpp"
#include "core/gui/paint.hpp"
#include "core/gui/item_icon.hpp"
#include "core/item/inventory.hpp"
#include "core/util/types.hpp"

namespace mc::ctr::hud {

// The console's cell, and the screen in the orientation a player looks at it.
inline constexpr int kCell = 8;
inline constexpr int kScreenWidth = 320;
inline constexpr int kScreenHeight = 240;
inline constexpr int kColumns = kScreenWidth / kCell;   // 40
inline constexpr int kRows = kScreenHeight / kCell;     // 30

// **The hotbar band, at the top.** 40 pixels: a 36-pixel slot with two either
// side, which is what a 24-pixel icon needs with a bevel that is still visible
// on a 320-pixel screen.
//
// **It was 32, and the eight it grew are the eight the focus banner gave up**
// when its gradient went (see `kBannerHeight`).
// A slot is 35 or 36 pixels wide (see `hotbarSlotX`) and was 28 tall, which is
// a landing strip rather than a cell: a1.1.2's own hotbar slot is square and
// the one thing a player reads off this band at a glance is which square is
// lit. 36 tall against 35 and 36 wide is that square, to the pixel, and the
// page below it did not move to pay for it.
inline constexpr int kHotbarHeight = 40;
inline constexpr int kHotbarTop = 0;
inline constexpr int kHotbarSlotY = kHotbarTop + 2;
inline constexpr int kHotbarSlotHeight = kHotbarHeight - 4;   // 36
inline constexpr int kHotbarColumns = item::kHotbarSlots;

// Where slot `i` starts, and where it ends -- **not a fixed width**, because
// nine does not divide 320. `hotbarSlotX(kHotbarColumns)` is the screen's right
// edge exactly, which is the property the row is built on.
constexpr int hotbarSlotX(int index)
{
    return index * kScreenWidth / kHotbarColumns;
}
constexpr int hotbarSlotWidth(int index)
{
    return hotbarSlotX(index + 1) - hotbarSlotX(index);
}
static_assert(hotbarSlotX(0) == 0, "the hotbar starts at the left edge");
static_assert(hotbarSlotX(kHotbarColumns) == kScreenWidth,
              "the hotbar ends at the right edge");

// **One character row under the hotbar that nothing but the backdrop draws
// into**: the margin between the hand and the page.
//
// **It used to be the focus banner**, sixteen pixels of it, half a gradient and
// half a row of yellow text saying the screen was focused. The gradient went
// first, because darkening the pixels it found is an operation that cannot be
// applied twice and every page underneath had to know it; the text went after
// it, to the top screen, where the player who needs telling is actually
// looking. The row it all stood in is kept because the layout below hangs off
// it and it reads as the page having a margin.
inline constexpr int kBannerHeight = kCell;                    // 8

// The tab strip, at the bottom. Three character rows so a label sits in the
// middle of it with a row of pixels either side.
inline constexpr int kTabHeight = 24;
inline constexpr int kTabTop = kScreenHeight - kTabHeight;     // 216

// **Where the page starts in a mode that has a hotbar**, which is every mode
// but Spectator. It is the number every page was laid out from when there was
// only one answer, and it is still what the static assertions measure against.
inline constexpr int kBandedPageTop = kHotbarHeight + kBannerHeight;   // 48

// **And the answer for a mode that has no hotbar**: the band is not reserved,
// it is simply not there, and the reserved row and the page move up into it.
inline constexpr int kBarePageTop = kBannerHeight;                     // 8

// **Which of the two is in force**, set once per gamemode change by the Overlay
// and read by every layout below.
//
// It is a setting on this file rather than an argument threaded through forty
// call sites because it is a property of the *screen*, not of any one thing
// drawn on it: there is one bottom screen, it is in one of two shapes, and a
// drawing call and the hit test that has to agree with it must never be handed
// different answers. Both page tops are laid out at compile time by the same
// constexpr functions, so neither shape is the untested one.
void setHotbarPresent(bool present);
bool hotbarPresent();

// 40 or 0 -- the band the hotbar occupies, and what everything else hangs off.
int hotbarHeight();
// The reserved row between the hotbar and the page.
int bannerTop();
// The first row a page may paint.
int pageTop();

// The slot size the palette grid is built from. 24 pixels is what a 16-pixel
// atlas tile needs with a border either side that is still visible on a
// 320-pixel screen.
inline constexpr int kSlotPixels = 24;

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
// **It is the off-screen picture, not the framebuffer** -- see
// platform/ctr/bottom_screen.hpp -- so it is always there and this always
// returns true. The check that the framebuffer is still RGB565 at 240 x 320
// moved to `bottom::flush`, which is the one thing that writes it: if either has
// changed under us, nothing is copied rather than bytes at computed offsets.
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

// **The inventory, and it is no longer drawn empty.** It used to be twenty-seven
// blank slots and a line of text reading "carried items: Survival", on the
// argument that Creative carries nothing. That argument was wrong in the way
// that matters: a1.1.2 writes all thirty-six slots into `level.dat` whatever
// mode you are in, so there was always something to show and somewhere to put
// it -- the only thing missing was an item table to name it with.
//
// So these are slots 9..35 of `Inventory`, nine across and three rows, drawn
// from the same stacks the hotbar band above is drawing 0..8 of, **with the
// four armour slots beside them** -- slots 100..103, which the save file has
// always carried and the screen had nowhere to show. `cursor` is the cell the
// bottom-screen focus is on, or -1; `held` is the slot a stack has been picked
// up from, or -1, and is drawn hollow so a move in progress is visible.
//
// **It lost its own hotbar row**, because the hotbar is a band of its own on
// every page now. Two hotbars on one screen would have had to agree about which
// slot was selected, and the way they agree is by there being one.
//
// **And it fills the page rather than sitting in a small panel in the middle of
// it.** The old frame was 232 x 112 with the pack's darkened dirt showing on
// every side of it, which reads as an unfinished screen; there is nothing else
// on this page, so the panel is the page. That is what pays for the 31-pixel
// slots -- 67 % more area than the 24-pixel ones, on a resistive screen where
// that is the difference between aiming and prodding. 31 rather than 32 because
// nine columns and an armour column have to share 320 pixels: ten 32s is 320
// exactly and leaves the panel no edge at all, and a bevel a player cannot see
// is a slot they cannot find the corner of. The panel's own margin is down to
// one pixel a side for the same reason the slot grew -- see `itemsLayout`.
inline constexpr int kItemsColumns = 9;
inline constexpr int kItemsRows = item::kBackpackSlots / kItemsColumns;
inline constexpr int kItemsSlotPixels = 31;

// The cursor space of the Items page: the backpack, and then the armour. So
// cell `kBackpackSlots + n` is armour slot `n`, and `itemsSlotForCell` is the
// one place that arithmetic lives.
inline constexpr int kItemsArmourCells = item::kArmourSlots;
inline constexpr int kItemsCells = item::kBackpackSlots + kItemsArmourCells;

// The save-file slot number a cell addresses -- 9..35 for the backpack and
// 100..103 for the armour, which is `Inventory::at`'s numbering and not a
// second one.
int itemsSlotForCell(int cell);

// The same mapping the other way about, or -1 for a slot this page does not
// draw -- which is the nine in the hand, since those are the band's.
int itemsCellForSlot(int slot);

// `carried` is the cell the picked-up stack is **hovering over**, or -1 when it
// is hovering somewhere else or nothing is in hand. It is a separate question
// from `held`: `held` is where the stack came from and is drawn hollow,
// `carried` is where it is now. See `Overlay::carriedPosition` for how the two
// are decided, and `drawCarried` in hud.cpp for why the stack is drawn exactly
// once.
void drawItemsPage(const gui::Surface& surface, const item::Inventory& inventory,
                   const gui::IconSheets& sheets, int cursor, int held, int carried);

// Which cell of the Items page a touch landed on, or -1. Covers the armour
// column as well as the backpack grid; put it through `itemsSlotForCell`.
int itemsCellAt(int touchX, int touchY);

// How big an icon is drawn inside a slot. The drawing itself is
// core/gui/item_icon.hpp's -- a cube seen from the corner for a plain block, a
// flat sprite for everything else -- and it lives in core rather than here so
// it can be exercised on the host, which is where the geometry is worth
// checking.
//
// **24 rather than 16, and that is a trade rather than a free win.** A flat
// sprite is a 16-pixel tile and `drawFlat` is nearest-neighbour, so 24 doubles
// every other row; 16 was 1:1 and crisp. What it buys is that the icon fills
// the slot -- a 16-pixel picture in a 32-pixel cell reads as a mostly empty
// box. The cube path does not pay it at all: `drawBox` inverse-maps each
// destination pixel and is exact at any size, and cubes are most of what a
// slot holds.
inline constexpr int kIconPixels = 24;

// **The hotbar, on every player page**, across the whole width of the screen.
// `cursor` is the slot the bottom-screen focus is sitting on, or -1 when the
// focus is off or on the grid below -- drawn differently from
// `inventory.selected`, because the two are different questions: one is what is
// in your hand and the other is what A would act on.
//
// `carried` is a hotbar index the picked-up stack is hovering over, or -1, and
// means on this band what it means on the Items page above.
void drawHotbar(const gui::Surface& surface, const item::Inventory& inventory,
                const gui::IconSheets& sheets, int cursor, int held, int carried);

// Which hotbar slot a touch landed on, or -1 -- including every touch outside
// the band, so a page can pass it every press it sees.
int hotbarSlotAt(int touchX, int touchY);

// **An open container screen**: the workbench, the furnace, a chest, or the
// Survival inventory with its 2 x 2 grid. Where the slots go is
// core/gui/container_layout.hpp's; this draws them.
//
// `cursor` is a slot index in the session's numbering -- the slot the next
// press acts on, and the one the stack on the cursor hovers over -- and
// `showCursor` whether it is outlined, which is only while the screen has the
// buttons. The page and the band are two calls for the reason the hotbar is
// always its own: the hand is in the band, and a cursor moving between the two
// has to redraw both while a furnace's arrow redraws neither.
void drawContainerPage(const gui::Surface& surface, const gui::ContainerLayout& layout,
                       const item::ContainerSession& session, const item::Inventory& inventory,
                       const gui::IconSheets& sheets, int cursor, bool showCursor);
void drawContainerBand(const gui::Surface& surface, const gui::ContainerLayout& layout,
                       const item::ContainerSession& session, const item::Inventory& inventory,
                       const gui::IconSheets& sheets, int cursor, bool showCursor);

// **Only the furnace's arrow and flame**, which move on the world's ticks
// rather than on a press -- a whole page of icons redrawn twenty times a second
// to grow an arrow by a pixel would be the bottom screen's most expensive
// frame.
void drawContainerProgress(const gui::Surface& surface, const gui::ContainerLayout& layout,
                           const item::ContainerSession& session, const gui::IconSheets& sheets);

// **The Creative block palette, which is its own page and not the inventory.**
// They are different things: the palette is a catalogue of every block this
// version defines and holds nothing, the inventory is what a player is carrying.
// Drawing a catalogue inside an inventory frame would imply the blocks in it
// were owned, which in Creative is exactly the confusion worth avoiding.
//
// Nine columns by five rows, so a page is 45 cells and a1.1.2's 147 offered
// items are four pages -- the palette offers the whole item table now, not only
// the two thirds of it that place a block. See core/item/creative_palette.hpp. `page` is 0-based; `cursor` is the cell the focus is on within
// this page, or -1; `selected` is the block in hand, outlined wherever on this
// page it appears. `caption` is the line under the grid -- the name of whatever
// the player is pointing at, which is the only place a block's name fits.
inline constexpr int kPaletteColumns = 9;
inline constexpr int kPaletteRows = 5;
inline constexpr int kPalettePerPage = kPaletteColumns * kPaletteRows;

// How many pages the palette fills, at least one.
int palettePageCount();

void drawBlocksPage(const gui::Surface& surface, const gui::IconSheets& sheets, int page,
                    int cursor, item::ItemId selected, const char* caption);

// Which cell of the drawn page a touch landed on, or -1. The index is
// page-relative: add `page * kPalettePerPage` for a palette index.
int paletteCellAt(int touchX, int touchY);

// **There is no focus banner here any more.** It was one dark row under the
// hotbar with a line of yellow text saying the screen was focused and what the
// buttons meant; the reserved row at `bannerTop()` is what is left of it, and
// it is backdrop now whether the screen is focused or not.
//
// The mode still has to be reported -- the d-pad quietly means something else,
// and an unannounced mode is the worst kind -- but not *here*. A player who has
// focused the bottom screen is looking at the bottom screen; a line of text on
// it is read by nobody, and a player still looking at the world got no signal
// at all. The report is on the top screen now, as a translucent band with an
// arrowhead pointing down at the screen that has the buttons: see
// `Renderer::setFocusHint`.

// The two page arrows on the palette's title row: -1 for the previous page, +1
// for the next, 0 for neither. They exist because the shoulder buttons are
// break and place, so paging cannot have them unless the screen is focused --
// and a page control that is invisible to a player who never focuses the screen
// is not a page control.
int paletteArrowAt(int touchX, int touchY);

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

// **The game-over page** -- `au`, GuiGameOver: "Game over!", the score, and the
// two buttons, Respawn over Title menu. `cursor` is the button the d-pad is on,
// 0 or 1. Drawn over the page band only; the tabs and the hotbar stay where
// they are, emptied by the death that put this up.
//
// The original scales its title to twice the size of everything else. A
// console cell cannot be scaled, so the title is marked by the panel's own row
// instead.
void drawGameOverPage(const gui::Surface& surface, int score, int cursor);

// Which of the two buttons a touch landed on: 0 Respawn, 1 Title menu, -1
// neither.
int gameOverButtonAt(int touchX, int touchY);

// The first pixel row of the look pad's drag area. A touch above it belongs to
// the tab strip and to the reserved row, neither of which is the camera's.
inline int lookPadTop() { return pageTop(); }

}  // namespace mc::ctr::hud
