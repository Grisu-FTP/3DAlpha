#pragma once

// **Face textures that are a question about the neighbours**, which is the one
// thing the generated `faces` row cannot carry: it is a table, and this is a
// branch on the cells around the block. `BlockDef::worldTexture` says which
// rule a block follows and this is where the rules are; everything else in the
// engine keeps reading `faces`.
//
// One rule so far, and it is the chest -- `b.a(Lnm;IIII)I`, transcribed rather
// than tidied, because the tile it picks is the difference between a chest that
// faces the room and one that faces a wall, and between two chests beside each
// other and one large chest drawn across two cells.
//
// **No world type here.** The caller hands in a way to read a block id at an
// (dx, dz) offset in the chest's own layer, so the mesher can answer out of its
// padded scratch, a test can answer out of a small array, and neither needs the
// other's idea of what a world is. The chest reads eight cells at most: the
// four beside it and, when it is paired, the two beside its partner.

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"

namespace mc::block {

// `b.a(Lnm;IIII)I` -- BlockChest.getBlockTexture, all six faces of it.
//
// ```
// if (face == 1 || face == 0) return blockIndexInTexture - 1;      // the lid
// int zm = ..., zp = ..., xm = ..., xp = ...;                      // neighbours
// if (zm == blockID || zp == blockID) { ... }                      // paired along z
// else if (xm == blockID || xp == blockID) { ... }                 // paired along x
// else { ... }                                                     // on its own
// ```
//
// **A single chest turns its front away from stone.** The four tests run in
// order -- -z, +z, -x, +x -- and the last one that fires wins, so a chest in a
// corner faces out along x. `opaqueCubeLookup` is what they read, which is
// `BlockDef::opaqueCube` and **not** `opaque`; see that field for why the two
// are different questions.
//
// **A pair draws one picture across two cells.** The joining faces go plain
// (tile 26), the two long sides take the front pair (42/41) or the back pair
// (58/57), and which half of the pair a cell draws is `offset`: -1 for the
// left tile, 0 for the right. The -x face of a z-pair and the +z face of an
// x-pair see the picture mirrored, which is the original's `offset = -1 -
// offset`, and it is why the seam does not swap sides when you walk round.
//
// **Three chests in a row are not modelled by any of this**, and that is
// faithful: the middle one takes the first branch that fires, pairs with its
// -z or -x neighbour, and the third draws its own half of a pair that is not
// there. a1.1.2 cannot normally build that -- `BlockChest.canPlaceBlockAt`
// refuses -- but `World.canBlockBePlacedAt` returns early for a cell holding
// water, lava, fire or a snow layer and never asks, so a chest placed into any
// of those joins a cluster the texture rule was never written for. See
// core/tick/behaviour.hpp.
template <class BlockAt>
void chestFaces(BlockId self, u16 texture, BlockAt at, u16 out[6])
{
    const int base = int(texture);
    const auto opaque = [](BlockId id) { return def(id).opaqueCube; };

    const BlockId zm = at(0, -1);
    const BlockId zp = at(0, 1);
    const BlockId xm = at(-1, 0);
    const BlockId xp = at(1, 0);

    // The lid and the underside, which are the same tile and never move.
    out[0] = u16(base - 1);
    out[1] = u16(base - 1);

    if (zm == self || zp == self) {
        const int dz = zm == self ? -1 : 1;
        const int offset = zm == self ? -1 : 0;
        // The two cells beside the *partner*, so that a block at either end of
        // the pair turns the whole pair round rather than half of it.
        const BlockId farXm = at(-1, dz);
        const BlockId farXp = at(1, dz);

        int front = 5;
        if ((opaque(xm) || opaque(farXm)) && !opaque(xp) && !opaque(farXp)) {
            front = 5;
        }
        if ((opaque(xp) || opaque(farXp)) && !opaque(xm) && !opaque(farXm)) {
            front = 4;
        }

        out[2] = u16(base);
        out[3] = u16(base);
        out[4] = u16(base + (front == 4 ? 16 : 32) + (-1 - offset));
        out[5] = u16(base + (front == 5 ? 16 : 32) + offset);
        return;
    }

    if (xm == self || xp == self) {
        const int dx = xm == self ? -1 : 1;
        const int offset = xm == self ? -1 : 0;
        const BlockId farZm = at(dx, -1);
        const BlockId farZp = at(dx, 1);

        int front = 3;
        if ((opaque(zm) || opaque(farZm)) && !opaque(zp) && !opaque(farZp)) {
            front = 3;
        }
        if ((opaque(zp) || opaque(farZp)) && !opaque(zm) && !opaque(farZm)) {
            front = 2;
        }

        out[2] = u16(base + (front == 2 ? 16 : 32) + offset);
        out[3] = u16(base + (front == 3 ? 16 : 32) + (-1 - offset));
        out[4] = u16(base);
        out[5] = u16(base);
        return;
    }

    int front = 3;
    if (opaque(zm) && !opaque(zp)) {
        front = 3;
    }
    if (opaque(zp) && !opaque(zm)) {
        front = 2;
    }
    if (opaque(xm) && !opaque(xp)) {
        front = 5;
    }
    if (opaque(xp) && !opaque(xm)) {
        front = 4;
    }
    for (int face = 2; face < 6; ++face) {
        out[face] = u16(face == front ? base + 1 : base);
    }
}

// **Every tile a rule can ever put on a face**, written into `out` and counted,
// for the one piece of code that has to know before any world exists: the cube
// atlas hands out a repeat slot per tile a cube face can sample, and a face
// whose tile has no slot samples tile 0 instead -- grass, in a1.1.2, on the
// side of a double chest. That was this file's first bug.
//
// Listed rather than derived, because deriving it means running the rule, and
// the rule wants a world. Kept next to the rule so the two are read together:
// a tile added above without a line here is a tile the atlas will not hold.
inline constexpr int kMaxWorldTextureTiles = 7;

constexpr int worldTextureTiles(WorldTexture rule, u16 texture, u16 out[kMaxWorldTextureTiles])
{
    const int base = int(texture);
    switch (rule) {
    case WorldTexture::Chest: {
        // The lid, the plain side, the single front, and the four halves of a
        // double chest's front and back.
        const int tiles[kMaxWorldTextureTiles] = {base - 1,      base,          base + 1,
                                                  base + 16 - 1, base + 16,     base + 32 - 1,
                                                  base + 32};
        for (int i = 0; i < kMaxWorldTextureTiles; ++i) {
            out[i] = u16(tiles[i]);
        }
        return kMaxWorldTextureTiles;
    }
    default:
        return 0;
    }
}

// The six tiles a block shows given what is around it: the block's own rule
// where it has one, and the generated row where it has not. `metadata` is the
// cell's own, for the blocks whose faces follow that instead.
//
// `at(dx, dz)` reads a block id in the same layer, offset from this cell.
template <class BlockAt>
void worldTextureFaces(BlockId id, u8 metadata, BlockAt at, u16 out[6])
{
    const BlockDef& row = def(id);
    switch (row.worldTexture) {
    case WorldTexture::Chest:
        chestFaces(id, row.texture, at, out);
        return;
    default:
        break;
    }
    const u16* tiles = worldFaces(id, metadata);
    for (int face = 0; face < 6; ++face) {
        out[face] = tiles[face];
    }
}

}  // namespace mc::block
