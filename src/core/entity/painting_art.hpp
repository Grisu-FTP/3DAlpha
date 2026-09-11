#pragma once

// **What pictures exist**, which is `er` -- EnumArt -- and is a table rather
// than code.
//
// It is its own header because the generated `mcver::kPaintings` has to include
// something, and `painting.hpp` includes the generated table in turn. The same
// split `block_def.hpp` and the generated block table already have.
//
// Every column is read out of the class file by `tools/genref.java --art`:
//
//   * `name` is the game's own string -- "Kebab", "SkullAndRoses" -- and is
//     **what a painting's NBT stores**, so it is not a label of ours to
//     shorten. `Motive` in the save file is this string.
//   * `width` and `height` are in texels of `art/kz.png` and are always
//     multiples of 16, because a painting is a whole number of blocks.
//   * `u` and `v` are where in that 256 x 256 sheet the picture begins.

#include "core/util/types.hpp"

namespace mc::entity {

struct PaintingArt {
    const char* name;
    // Texels, and a multiple of 16. Divide by 16 for the size in blocks.
    u8 width;
    u8 height;
    // Texels into art/kz.png.
    u8 u;
    u8 v;

    int blocksWide() const { return int(width) / 16; }
    int blocksTall() const { return int(height) / 16; }
};

}  // namespace mc::entity
