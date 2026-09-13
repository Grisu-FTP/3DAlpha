#pragma once

// Drawing into a software framebuffer: the bottom screen's UI, and the arrow
// that marks a player on the map.
//
// **The bottom screen is not a GPU target, with one exception.** It is
// libctru's text console -- every message this shell says to the player goes
// through it, from "Saving level.." to a failed texture pack, and the three
// debug pages are built on it -- so anything drawn beside that text is written
// straight into the RGB565 framebuffer by the CPU. The exception is the main
// menu's Skins, Texture Pack and World screens, which link a render target to
// the bottom screen while they are up and hand it back to the console when
// they are left; nothing in game does. See platform/ctr/menu_preview.hpp. See map_screen.hpp for the
// whole of that argument. What was missing was somewhere for the drawing itself
// to live: the map had a marker routine of its own, the HUD wanted panels and
// slots, and neither had any business knowing what a framebuffer looks like.
//
// So this is the drawing, and nothing in it knows what it is drawing into. A
// `Surface` is a pointer, two signed strides and a size -- the console's bottom
// screen is stored in columns from the bottom up, which is a strideX of 240 and
// a strideY of -1, and a host test is an ordinary row-major buffer. Every
// function clips, so a caller may ask for a rectangle that hangs off the edge
// and get the part that fits.

#include "core/util/types.hpp"

namespace mc::gui {

// The console's bottom screen is RGB565 once `consoleInit` has had it, so that
// is what everything here writes. Nothing else about it is platform specific.
using Pixel = u16;

inline constexpr Pixel rgb565(int r, int g, int b)
{
    return Pixel(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// The same, from an 0x00RRGGBB word -- which is how the block palette, the
// font and every colour written down in this project already carries a colour.
inline constexpr Pixel rgb565(u32 rgb)
{
    return rgb565(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

// Where the pixels are. `strideX` is the step to the pixel one to the right and
// `strideY` the step to the one below, both signed, so a framebuffer that runs
// up the screen is a negative stride rather than a second code path.
//
// `pixels` addresses (0, 0) -- the top-left corner -- whatever the strides do
// afterwards.
struct Surface {
    Pixel* pixels = nullptr;
    int strideX = 1;
    int strideY = 1;
    int width = 0;
    int height = 0;

    bool valid() const { return pixels != nullptr && width > 0 && height > 0; }

    // Null outside the surface, so a caller that computed a coordinate rather
    // than iterating one does not have to bounds-check twice.
    Pixel* at(int x, int y) const
    {
        if (pixels == nullptr || x < 0 || y < 0 || x >= width || y >= height) {
            return nullptr;
        }
        return pixels + x * strideX + y * strideY;
    }
};

// Every one of these clips against the surface and does nothing at all when
// what is left is empty.
void fillRect(const Surface& surface, int x, int y, int w, int h, Pixel colour);
void hLine(const Surface& surface, int x, int y, int w, Pixel colour);
void vLine(const Surface& surface, int x, int y, int h, Pixel colour);

// A one-pixel outline just inside the rectangle.
void frameRect(const Surface& surface, int x, int y, int w, int h, Pixel colour);

// **a1.1.2's own GUI, which is two rectangles and two bevels.** The original
// draws a panel as a face with a light edge along the top and left and a dark
// one along the bottom and right (`GuiScreen`'s widgets), and a slot the other
// way round, which is what makes one read as standing out of the screen and the
// other as cut into it. `raised` picks between them.
void bevelBox(const Surface& surface, int x, int y, int w, int h, Pixel face, Pixel light,
              Pixel dark, bool raised);

// Repeats `tile`, a `tileEdge` by `tileEdge` square, over the rectangle. The
// phase is taken from the surface origin rather than from the rectangle, so two
// rectangles filled separately line up.
void tilePattern(const Surface& surface, int x, int y, int w, int h, const Pixel* tile,
                 int tileEdge);

// **The player marker: an arrowhead, pointing wherever it is told to point.**
//
// The one it replaces was a diamond with a tick made of whole blocks stepped
// along one of eight compass directions, and it had two faults that are the
// same fault: it could only ever point eight ways, and its tick was a block
// step, so the diagonal tick was the square root of two longer than the
// straight one and the marker changed size as it turned. An arrowhead is a
// rotated shape rather than a stepped one, so it is the same length at every
// angle and it points at whatever angle it is given.
//
// `length` is centre to tip, `halfWidth` half the span across the back corners,
// `tail` how far behind the centre those corners sit, and `notch` how far
// forward the bite in the back cuts -- so the shape is a four-point polygon and
// the caller sizes it rather than getting a sprite.
// The defaults are the player marker, tuned by looking at it: an arrowhead
// about eleven pixels long and six across, which is the smallest that still
// reads as an arrow at every angle rather than as a lump at some of them.
struct ArrowShape {
    float length = 6.5f;
    float halfWidth = 3.0f;
    float tail = 4.0f;
    float notch = 2.8f;
};

// The largest arrow that may be asked for. It bounds a stack buffer, and the
// 3DSX main thread has 32 KB of stack that no symbol in the binary can enlarge
// -- see docs/status.md -- so it is a hard cap and not a hint. 16 gives a
// 35 x 35 coverage buffer, which is 1,225 bytes.
inline constexpr float kMaxArrowLength = 16.0f;

// `angle` is radians **clockwise from straight up the surface**, which is the
// direction north points on a map and on a compass. The outline is every empty
// pixel touching a filled one, worked out here rather than drawn by hand, so
// the marker stays legible over any terrain colour at any angle without a
// second sprite per direction.
void drawArrow(const Surface& surface, float centreX, float centreY, float angle,
               const ArrowShape& shape, Pixel fill, Pixel outline);

}  // namespace mc::gui
