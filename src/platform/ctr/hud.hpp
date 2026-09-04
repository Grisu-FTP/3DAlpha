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
// The layout is 320 x 240, in three bands:
//
//     +----------------------------------------+  y = 0
//     |  [ Map ] [ Items ] [ Blocks ] [ Look ] |  the tab strip, rows 1-3
//     +----------------------------------------+  y = 24
//     |  " Bottom screen focused "  + a fade    |  the focus banner, or backdrop
//     +----------------------------------------+  y = 40
//     |                                        |
//     |               the page                 |  rows 6-26
//     |                                        |
//     +----------------------------------------+  y = 208
//     |  the hotbar, on every player page      |
//     +----------------------------------------+  y = 240
//
// **There was a third band once and it said what the buttons did.** It is gone:
// the hints were the same three lines on every page and they were spending a
// twelfth of the screen to repeat themselves. The debug pages still carry
// theirs, which is where SELECT + Y is worth naming -- it is the one binding
// that is not discoverable by touching the screen.
//
// **The band at the bottom now is the hotbar, and it is reserved in every
// gamemode.** A page whose height depended on whether the mode had a hotbar
// would be two layouts, two sets of constants and two map window sizes to
// measure against the console; Spectator gets 32 pixels of backdrop instead.
// What that costs is a slightly shorter map and what it buys is that the hotbar
// never moves and the page above it never resizes.
//
// **It is here rather than over the world**, which is docs/3ds-performance.md
// section 11 and not a new decision: the top screen draws nothing but the
// world, so the HUD costs no overdraw at all on a fill-bound device. It is also
// the only screen that can be touched.

#include "core/block/block_def.hpp"
#include "core/gui/paint.hpp"
#include "core/item/hotbar.hpp"
#include "core/util/types.hpp"

namespace mc::ctr::hud {

// The console's cell, and the screen in the orientation a player looks at it.
inline constexpr int kCell = 8;
inline constexpr int kScreenWidth = 320;
inline constexpr int kScreenHeight = 240;
inline constexpr int kColumns = kScreenWidth / kCell;   // 40
inline constexpr int kRows = kScreenHeight / kCell;     // 30

// The three bands. The tab strip is three character rows so a label sits in the
// middle of it with a row of pixels either side; the page is what is left once
// the hotbar has taken the bottom 32.
inline constexpr int kTabTop = 0;
inline constexpr int kTabHeight = 24;
inline constexpr int kBodyTop = kTabTop + kTabHeight;   // 24

// **Sixteen pixels under the tab strip that nothing but the backdrop draws
// into**, and that is a load-bearing promise rather than spare margin. The
// focus banner *darkens* what is under it, which is an operation that cannot be
// applied twice to the same pixels -- so it has to sit somewhere that is
// painted exactly once, on a page clear, and never repainted underneath it. The
// map alone would otherwise redraw through it on every block the player walks
// and darken the strip a shade further each time, down to black over a minute
// or so.
//
// Unfocused it is backdrop, and reads as the page having a margin.
inline constexpr int kBannerTop = kBodyTop;                    // 24
inline constexpr int kBannerHeight = 16;
inline constexpr int kPageTop = kBannerTop + kBannerHeight;    // 40

// **The hotbar band, and the slot size every grid here is built from.** 24
// pixels is what a 16-pixel atlas tile needs with a border either side that is
// still visible on a 320-pixel screen -- so an icon is blitted at 1:1 and never
// resampled, which with a nearest-neighbour copy is the difference between a
// readable block and a smear. Nine of them is 216 pixels, centred with 52
// either side.
inline constexpr int kSlotPixels = 24;
inline constexpr int kHotbarHeight = 32;
inline constexpr int kHotbarTop = kScreenHeight - kHotbarHeight;    // 208
inline constexpr int kHotbarSlotY = kHotbarTop + 4;
inline constexpr int kHotbarColumns = item::kHotbarSlots;
inline constexpr int kHotbarWidth = kHotbarColumns * kSlotPixels;   // 216
inline constexpr int kHotbarX = (kScreenWidth - kHotbarWidth) / 2;  // 52

inline constexpr int kBodyHeight = kHotbarTop - kBodyTop;           // 184
inline constexpr int kPageHeight = kHotbarTop - kPageTop;          // 168

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

// **The inventory, still drawn empty -- and Creative is why that is not a
// contradiction.** Creative has a palette and a hotbar, and neither of them is
// an inventory: nothing is carried, nothing is picked up and nothing is stored,
// so there is no content for these 27 slots until Survival puts some there.
// What the page is today is the frame they will be dealt into, nine across and
// three rows, at the size and spacing they will keep.
//
// **It lost its own hotbar row**, because the hotbar is a band of its own on
// every page now. Two hotbars on one screen would have had to agree about which
// slot was selected, and the way they agree is by there being one.
void drawItemsPage(const gui::Surface& surface);

// **One block as a 16 x 16 icon, blitted at 1:1 out of the terrain atlas.**
//
// `atlasRgba` is `texture::AtlasImage::rgba` -- 256 x 256, RGBA8, top row
// first -- or null, in which case nothing is drawn and the cell stays an empty
// slot rather than a wrong one. `tile` is the block's own atlas index, which is
// `BlockDef::texture`.
//
// **A flat tile and not a cube, which is a deviation worth naming.** a1.1.2
// draws a block in a slot through `RenderBlocks.renderBlockAsItem` -- a small
// three-quarter view built by the same block renderer that draws the world.
// Doing that here would mean a second geometry path on a screen that has no GPU
// access at all: this is a CPU blit into libctru's framebuffer. So a cell shows
// the block's `texture` column, which the table already defines as "what the
// block shows anywhere a single tile is wanted". Grass reads as grass from
// above and a log as its bark.
//
// Alpha is a cutout rather than a blend -- a texel under the threshold is
// skipped -- which is what leaves a torch or a sapling standing on the slot
// instead of in a black box. The atlas carries no partial alpha worth blending.
inline constexpr int kIconPixels = 16;
inline constexpr u8 kIconAlphaCutoff = 128;
void drawBlockIcon(const gui::Surface& surface, int x, int y, const u8* atlasRgba, int tile);

// **The hotbar, on every player page.** `cursor` is the slot the bottom-screen
// focus is sitting on, or -1 when the focus is off or on the palette above --
// drawn differently from `hotbar.selected`, because the two are different
// questions: one is what is in your hand and the other is what A would act on.
void drawHotbar(const gui::Surface& surface, const item::Hotbar& hotbar, const u8* atlasRgba,
                int cursor);

// Which hotbar slot a touch landed on, or -1 -- including every touch outside
// the band, so a page can pass it every press it sees.
int hotbarSlotAt(int touchX, int touchY);

// **The Creative block palette, which is its own page and not the inventory.**
// They are different things: the palette is a catalogue of every block this
// version defines and holds nothing, the inventory is what a player is carrying.
// Drawing a catalogue inside an inventory frame would imply the blocks in it
// were owned, which in Creative is exactly the confusion worth avoiding.
//
// Nine columns by five rows, so a page is 45 cells and a1.1.2's 70 blocks are
// two pages. `page` is 0-based; `cursor` is the cell the focus is on within
// this page, or -1; `selected` is the block in hand, outlined wherever on this
// page it appears. `caption` is the line under the grid -- the name of whatever
// the player is pointing at, which is the only place a block's name fits.
inline constexpr int kPaletteColumns = 9;
inline constexpr int kPaletteRows = 5;
inline constexpr int kPalettePerPage = kPaletteColumns * kPaletteRows;

// How many pages the palette fills, at least one.
int palettePageCount();

void drawBlocksPage(const gui::Surface& surface, const u8* atlasRgba, int page, int cursor,
                    block::BlockId selected, const char* caption);

// Which cell of the drawn page a touch landed on, or -1. The index is
// page-relative: add `page * kPalettePerPage` for a palette index.
int paletteCellAt(int touchX, int touchY);

// **The focus banner: a dark gradient under the tab strip with a line of text
// on it.** Drawn while X has the bottom screen focused, and it exists because
// the focus is otherwise invisible -- the d-pad quietly means something else
// and nothing on the screen says so, which is the worst kind of mode.
//
// It lives in the reserved band at `kBannerTop`, which nothing but the backdrop
// ever paints -- see the note there. That is what makes it safe for the fade to
// be *destructive*: it darkens the pixels it finds, so a strip anything else
// redrew under it would darken a shade further every time. Leaving the focus is
// a full page redraw rather than an attempt to undo it.
//
// The label row is flat rather than graded, and that is libctru's doing, not a
// choice: `consoleDrawChar` writes all 64 pixels of a cell, so a character row
// has exactly one background colour. The gradient is the rows underneath it,
// fading from that colour back into the page.
inline constexpr int kFocusBannerLabelHeight = kCell;
inline constexpr int kFocusBannerFadeHeight = kBannerHeight - kFocusBannerLabelHeight;
void drawFocusBanner(const gui::Surface& surface, const char* text);

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

// The first pixel row of the look pad's drag area. A touch above it belongs to
// the tab strip and to the banner band, neither of which is the camera's.
inline constexpr int kLookPadTop = kPageTop;

}  // namespace mc::ctr::hud
