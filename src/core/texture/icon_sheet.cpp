// The generated stand-in for `gui/icons.png`. See icon_sheet.hpp.

#include "core/texture/icon_sheet.hpp"

namespace mc::texture {

namespace {

// 9 x 9 masks, one row a string, '#' set. Drawn for legibility at 1:1 on a
// 400 x 240 screen rather than to resemble anybody's art.
constexpr const char* kHeartMask[kIconCellSize] = {
    ".##...##.",
    "####.####",
    "#########",
    "#########",
    ".#######.",
    "..#####..",
    "...###...",
    "....#....",
    ".........",
};

constexpr const char* kChestMask[kIconCellSize] = {
    "##.....##",
    "###...###",
    "#########",
    ".#######.",
    ".#######.",
    ".#######.",
    ".#######.",
    ".#######.",
    ".........",
};

constexpr const char* kBubbleMask[kIconCellSize] = {
    "..#####..",
    ".#.....#.",
    "#.......#",
    "#.##....#",
    "#.#.....#",
    "#.......#",
    "#.......#",
    ".#.....#.",
    "..#####..",
};

bool setAt(const char* const* mask, int x, int y)
{
    return x >= 0 && y >= 0 && x < kIconCellSize && y < kIconCellSize && mask[y][x] == '#';
}

// A texel is on the outline when it is set and one of its four neighbours is
// not -- which is what turns a filled mask into a container.
bool edgeAt(const char* const* mask, int x, int y)
{
    return setAt(mask, x, y)
           && (!setAt(mask, x - 1, y) || !setAt(mask, x + 1, y) || !setAt(mask, x, y - 1)
               || !setAt(mask, x, y + 1));
}

struct Rgba {
    u8 r, g, b, a;
};

void put(std::vector<u8>* rgba, int x, int y, Rgba c)
{
    const usize i = (usize(y) * usize(kIconSheetEdge) + usize(x)) * 4;
    (*rgba)[i] = c.r;
    (*rgba)[i + 1] = c.g;
    (*rgba)[i + 2] = c.b;
    (*rgba)[i + 3] = c.a;
}

enum class Paint : u8 { Outline, Fill, LeftHalfFill };

// Paints one cell: `Outline` the mask's edge in `edge` and its interior in
// `inner`; `Fill` the whole mask inside the edge in `inner`; `LeftHalfFill` the
// same for the left four columns only.
void paintCell(std::vector<u8>* rgba, IconCell cell, const char* const* mask, Paint paint,
               Rgba edge, Rgba inner)
{
    for (int y = 0; y < kIconCellSize; ++y) {
        for (int x = 0; x < kIconCellSize; ++x) {
            if (!setAt(mask, x, y)) {
                continue;
            }
            const int tx = int(cell.u) + x;
            const int ty = int(cell.v) + y;
            const bool onEdge = edgeAt(mask, x, y);
            switch (paint) {
            case Paint::Outline:
                put(rgba, tx, ty, onEdge ? edge : inner);
                break;
            case Paint::Fill:
                if (!onEdge) {
                    put(rgba, tx, ty, inner);
                }
                break;
            case Paint::LeftHalfFill:
                if (!onEdge && x <= 4) {
                    put(rgba, tx, ty, inner);
                }
                break;
            }
        }
    }
}

}  // namespace

void buildIconStandIn(std::vector<u8>* rgba)
{
    rgba->assign(kIconSheetBytes, 0);

    constexpr Rgba kBlack{0, 0, 0, 255};
    constexpr Rgba kWhite{255, 255, 255, 255};
    constexpr Rgba kHollow{48, 16, 16, 255};
    constexpr Rgba kRed{220, 24, 24, 255};
    constexpr Rgba kPale{255, 200, 200, 255};
    constexpr Rgba kSteel{190, 190, 200, 255};
    constexpr Rgba kSteelDark{70, 70, 80, 255};
    constexpr Rgba kWater{120, 200, 255, 255};
    constexpr Rgba kClear{0, 0, 0, 0};

    // Hearts.
    paintCell(rgba, kIconHeartContainer, kHeartMask, Paint::Outline, kBlack, kHollow);
    paintCell(rgba, kIconHeartContainerFlash, kHeartMask, Paint::Outline, kWhite, kHollow);
    paintCell(rgba, kIconHeartFull, kHeartMask, Paint::Fill, kClear, kRed);
    paintCell(rgba, kIconHeartHalf, kHeartMask, Paint::LeftHalfFill, kClear, kRed);
    paintCell(rgba, kIconHeartFlashFull, kHeartMask, Paint::Fill, kClear, kPale);
    paintCell(rgba, kIconHeartFlashHalf, kHeartMask, Paint::LeftHalfFill, kClear, kPale);

    // Armour.
    paintCell(rgba, kIconArmourEmpty, kChestMask, Paint::Outline, kSteelDark, kClear);
    paintCell(rgba, kIconArmourHalf, kChestMask, Paint::Outline, kSteelDark, kClear);
    paintCell(rgba, kIconArmourHalf, kChestMask, Paint::LeftHalfFill, kClear, kSteel);
    paintCell(rgba, kIconArmourFull, kChestMask, Paint::Outline, kSteelDark, kSteel);

    // Air. **Both are outlines**: the bubble mask is a ring one texel thick, so
    // every texel in it is an edge and a fill would paint nothing. The popping
    // one is told apart by colour instead.
    constexpr Rgba kFoam{230, 250, 255, 255};
    paintCell(rgba, kIconBubble, kBubbleMask, Paint::Outline, kWater, kClear);
    paintCell(rgba, kIconBubblePopping, kBubbleMask, Paint::Outline, kFoam, kClear);
}

}  // namespace mc::texture
