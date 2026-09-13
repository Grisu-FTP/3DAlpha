#pragma once

// **Which rows of a menu list are worth having ready**, for the bottom-screen
// previews on the main menu's Skins, Texture Pack and World screens.
//
// Each of those screens shows something that costs a card read to build -- a
// skin, a pack's terrain.png, 576 chunks of a world -- and each wants moving the
// cursor to feel free. The way that is bought is the same for all three: the
// rows around the cursor are built in the background before the cursor gets
// there, nearest first, and what has drifted far behind is given back.
//
// This is that ordering and nothing else. No row is loaded here and no memory
// is counted; the screens decide what "resident" means for them (a texture
// slot, a vertex buffer, a decoded grid) and ask this which to load next and
// which to drop.

#include "core/util/types.hpp"

namespace mc::preview {

// The rows within `radius` of `cursor`, nearest first, the row after the cursor
// before the row before it: cursor, +1, -1, +2, -2, ... Only rows inside
// [0, count) are written, so a cursor at the top of the list simply gets more
// of the rows below it. Returns how many were written, at most `max`.
int wantedOrder(int cursor, int count, int radius, int* out, int max);

// Whether `index` is within `radius` rows of `cursor`.
inline bool inWindow(int cursor, int index, int radius)
{
    const int distance = index - cursor;
    return distance >= -radius && distance <= radius;
}

// Of `count` resident rows, the **position in `resident`** of the one furthest
// from the cursor that is outside the window -- the thing to give back first
// when a new row needs its room. Negative entries are empty slots and are
// skipped. -1 when every resident row is inside the window.
int farthestOutside(const int* resident, int count, int cursor, int radius);

}  // namespace mc::preview
