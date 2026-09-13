#pragma once

// **Where an open container screen's slots are on the bottom screen**, as
// rectangles in the order `item::ContainerSession` numbers them -- so a touch,
// a d-pad step and a redraw all name a slot by the same index the click takes.
//
// a1.1.2 lays its four screens out in an 176-pixel-wide window with 18-pixel
// slots and a mouse to point at them (`hx`, `id`, `ea`, `lo`). The 3DS has a
// 320 x 168 page band, a finger and a d-pad, so the arrangement is the
// original's -- the container's own slots above, the backpack below, the hand
// last -- and the sizes are the band's:
//
//   * **The hand is the hotbar band's own nine cells** at the top of the
//     screen, which every player page already draws. A second copy of the
//     hotbar inside the page would be two views of one set of slots.
//   * **The slots are 24 pixels** wherever the screen can afford it, which is
//     every screen but a double chest. Nine rows of 24 is 216 and the band is
//     168, so a large chest takes 17-pixel slots instead: smaller to aim at,
//     and still all fifty-four on one page, which a scrolled list would not be.
//   * **And a chest bigger than a double one scrolls**, because there is no
//     third size that would fit. a1.1.2 never needed this -- its own screen is
//     as tall as its chest -- but a cluster of three, four or five chests is
//     reachable in it (see `tick::chestInventoryParts`), and a screen 15 rows
//     deep is 255 pixels of slots in a 106-pixel space. Six rows are shown and
//     the rest scroll under them; a slot outside the window has a **zero-sized
//     rectangle**, so it is drawn by nothing, hit by no touch and stepped on by
//     no d-pad press, and every other index stays exactly where it was.
//   * **The d-pad walks the rectangles by position**, not by index. The
//     screens are four different shapes and a result slot, an armour row and a
//     furnace's two stacked inputs have no row-and-column numbering to step
//     through; the nearest slot in the pressed direction is right on all four.
//
// Core rather than platform code so the arithmetic is host-tested: a hit test
// a few pixels out of step with the drawing is felt as a bad touch screen.

#include "core/item/container_session.hpp"
#include "core/util/types.hpp"

namespace mc::gui {

struct SlotRect {
    i16 x = 0;
    i16 y = 0;
    i16 w = 0;
    i16 h = 0;
};

// The bottom screen's bands, which platform/ctr/hud.hpp owns and asserts
// against these.
//
// **The page's top is passed in rather than named here**, because it is no
// longer one number: a gamemode with no hotbar starts its pages forty pixels
// higher, and the container screens have to be laid out from wherever the page
// actually begins. `kLayoutPageTop` is the value a mode *with* a hotbar uses,
// kept as the name the tests and the asserts in hud.cpp measure against.
inline constexpr int kLayoutScreenWidth = 320;
inline constexpr int kLayoutHotbarSlotY = 2;
inline constexpr int kLayoutHotbarSlotHeight = 36;
inline constexpr int kLayoutPageTop = 48;
inline constexpr int kLayoutPageBottom = 216;

inline constexpr int kMaxLayoutSlots = item::kMaxChestScreenSlots + item::kMainSlots;
inline constexpr int kMaxLayoutTitles = 2;

struct ContainerLayout {
    int slots = 0;
    SlotRect rect[kMaxLayoutSlots];

    // **The chest's scroll window**, and zeroes for every other screen.
    // `chestRows` is how many rows the screen has in all, `chestFirstRow` the
    // top one drawn (already clamped, so the caller can hand in whatever it
    // was holding and read back what it should hold), `chestWindowRows` how
    // many fit. A screen that fits has `chestWindowRows == chestRows` and no
    // furniture: the four rectangles below are empty.
    int chestRows = 0;
    int chestFirstRow = 0;
    int chestWindowRows = 0;

    // The scroll furniture, all `w` 0 when nothing scrolls: two arrow buttons
    // and the track they sit at the ends of, with the thumb showing where in
    // the chest the window is.
    SlotRect scrollUp;
    SlotRect scrollDown;
    SlotRect scrollTrack;
    SlotRect scrollThumb;

    // The panel behind the page's slots.
    SlotRect panel;

    // The crafting arrow, or the furnace's progress arrow; `w` 0 for none.
    SlotRect arrow;
    bool arrowIsProgress = false;
    // The furnace's flame; `w` 0 for none.
    SlotRect flame;

    // Up to two lines of title, on the console's 1-based character grid.
    int titles = 0;
    const char* title[kMaxLayoutTitles] = {};
    int titleRow[kMaxLayoutTitles] = {};
    int titleColumn[kMaxLayoutTitles] = {};
};

// The layout of whatever `session` has open, with the page band running from
// `pageTop` to `kLayoutPageBottom`. An unopened session lays out nothing.
//
// **`pageTop` is `kLayoutPageTop` in practice**, and that is a fact about the
// game rather than a limitation here: the last nine slots of every one of these
// screens are the player's hand, so a mode that can open one is a mode that has
// a hotbar band for them to be drawn in. The argument exists so the band and
// the screens laid out under it cannot drift apart, not because a container is
// expected above the band.
//
// `chestFirstRow` is the top chest row to show, clamped here and reported back
// in `out->chestFirstRow`; it is ignored by every screen but a chest too tall
// to fit, and 0 is the unscrolled answer.
void buildContainerLayout(const item::ContainerSession& session, int pageTop,
                          ContainerLayout* out, int chestFirstRow = 0);

// The scroll control a touch landed on: -1 for the up arrow, 1 for the down
// one, 0 for neither. Generous by a few pixels in both directions, as every
// other target on this screen is -- a fingertip on a resistive screen is not
// sixteen pixels wide.
int containerScrollAt(const ContainerLayout& layout, int x, int y);

// The slot a touch landed on, or -1.
int containerSlotAt(const ContainerLayout& layout, int x, int y);

// The slot one d-pad press away from `from` in the direction (`dx`, `dy`), each
// -1, 0 or 1, or `from` itself when nothing lies that way.
int containerStep(const ContainerLayout& layout, int from, int dx, int dy);

}  // namespace mc::gui
