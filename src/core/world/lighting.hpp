#pragma once

// Sky and block light, matching a1.1.2 exactly and computed the fast way.
//
// **The original does not light a chunk in one pass, and this does.** That is
// the one deliberate difference in the whole file, so it is worth being precise
// about why it is safe.
//
// a1.1.2 fills each column downward from its own height map in
// `Chunk.generateSkylightMap`, and leaves everything else -- light crossing a
// chunk boundary, block light, and the corrections needed once a neighbour
// finally loads -- to a **queue of bounding boxes** (`kn`) that
// `World.updateLights` drains a thousand at a time, re-scanning each box and
// rescheduling its neighbours until nothing changes. Its update rule is
//
//     light(p) = max( emitted(p), max over the six neighbours n of
//                                 ( light(n) - max(1, opacity(p)) ) )   , >= 0
//
// applied over and over. That is a fixed-point iteration of a monotone
// operator with a strictly positive decrement, so it has **exactly one**
// solution, and it is the familiar one: the light at p is the best any source
// can do after paying the opacity along a path. Every algorithm that finds
// that solution finds the same numbers.
//
// So this computes the same fixed point directly, with a bucketed
// breadth-first search -- sixteen queues, one per light level, drained from 15
// down to 1. Each cell is settled once instead of being re-scanned by every
// box that happens to contain it, which is what makes it affordable on a
// 268 MHz ARM11.
//
// The equivalence is not taken on faith. `tools/genref.java --light` runs a
// real a1.1.2 World, drains `updateLights` until its queue is empty -- and
// fails loudly rather than emitting a fixture if it ever does not converge --
// and `tests/light_test.cpp` compares every one of the 32,768 sky and 32,768
// block values.
//
// **Window.** Population needs a 3x3 of columns to write into; lighting needs
// one to read from, for a different reason. Light reaches at most 15 blocks, a
// 3x3 extends 16 past the centre in every direction, and a path that leaves
// the window and re-enters cannot be shorter than the direct one -- so no
// source and no route outside the window can change a single value inside the
// centre chunk. 3x3 is sufficient, and 2x2 provably is not.
//
// **Memory.** The window is 48 x 128 x 48 = 294,912 cells, and the engine holds
// two bytes per cell: a clamped opacity, and the light being solved for. That
// is 576 KB, allocated once for the lifetime of the engine and reused for
// every chunk -- **not** per chunk, and not on any stack. Sky and block light
// share the light buffer by running one after the other. See the note on
// LightEngine for why this lives on the chunk worker.

#include "core/block/registry.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::world {

// Blocks per chunk column, and the column layout: x << 11 | z << 7 | y.
inline constexpr int kColumnBlocks = 32768;
inline constexpr int kColumnHeight = 128;

// `Chunk.generateHeightMap`. For each of the 256 columns, the lowest y such
// that everything above it is fully transparent -- so `heightMap[z << 4 | x]`
// is the first block that *can* see the sky, and 128 means none of them can.
//
// Note it tests `lightOpacity != 0`, not "is solid": glass and air let the
// height map through, water and leaves stop it.
//
// `heightMap` is 256 bytes, indexed `z << 4 | x` -- **not** the `x << 4 | z`
// used elsewhere in this codebase. That is the original's order and the order
// the chunk file stores, so it is kept.
void computeHeightMap(const u8* blocks, u8* heightMap);

// **The two rules every light calculation in the project must agree on.** Free
// functions rather than private statics because there are now two engines --
// this one, which solves a whole column at generation time, and
// core/world/light_update.hpp, which repairs a few cells after a block change.
// If those two disagree anywhere, they disagree everywhere the disagreement
// occurs, and the symptom is a patch of the world that is lit differently
// depending on when it was last touched.

// Clamped to [1, 15]: the original substitutes 1 for an opacity of 0, and any
// opacity of 15 or more extinguishes light completely, so nothing above 15 is
// distinguishable. Storing the clamped value means the hot loop does no
// clamping at all.
inline u8 clampedOpacity(u8 blockId)
{
    const block::BlockDef& def = block::def(blockId);
    // **An id this version does not define has opacity 0, not 255.** The block
    // table gives unknown ids a solid opaque cube on purpose -- a visible wrong
    // block is a bug report -- but `Block.lightOpacity` in the jar is a plain
    // 256-entry array that was simply never written for those ids, so the
    // original lets light straight through. Faithful lighting has to agree with
    // the array, not with our fallback.
    const u16 opacity = def.known ? def.opacity : 0;
    return opacity < 1 ? u8(1) : (opacity > 15 ? u8(15) : u8(opacity));
}

// What the block itself gives off, on the same "unknown means the jar's array
// was never written" rule.
inline u8 emittedLight(u8 blockId)
{
    const block::BlockDef& def = block::def(blockId);
    return def.known ? def.light : u8(0);
}

class LightEngine {
public:
    // Columns per side of the window, and blocks per side.
    static constexpr int kWindow = 3;
    static constexpr int kWindowBlocks = kWindow * 16;
    static constexpr int kWindowCells = kWindowBlocks * kWindowBlocks * kColumnHeight;

    LightEngine();

    // Solves both light types for the centre column of the window and writes
    // them out.
    //
    // `columns` is nine pointers in row-major order with x slowest -- column
    // (ix, iz) is `columns[ix * 3 + iz]` -- matching PopulationView's
    // convention. A null pointer stands for a column that does not exist and
    // reads as air, which is what the original does when it lights against an
    // unloaded neighbour.
    //
    // `skyOut` and `blockOut` are 16,384-byte packed nibble arrays in the
    // chunk file's own layout, so they can be handed straight to storage.
    // `heightMapOut` is 256 bytes and may be null.
    void computeCentre(const u8* const* columns, u8* skyOut, u8* blockOut, u8* heightMapOut);

    // How many cells the last run actually touched, per light type. Exposed
    // because the whole argument for this design is that it settles each cell
    // once rather than re-scanning, and a number on the debug page is the only
    // way to notice if that stops being true.
    u32 lastSkyVisits() const { return skyVisits_; }
    u32 lastBlockVisits() const { return blockVisits_; }

private:
    // Window index. Y-fastest, like every other block array here, so a
    // vertical step is +-1 and stays in the same cache line more often than
    // not -- which matters because sky light propagates mostly downward.
    static constexpr int index(int wx, int y, int wz)
    {
        return (wx * kWindowBlocks + wz) * kColumnHeight + y;
    }

    static constexpr int kStepX = kWindowBlocks * kColumnHeight;
    static constexpr int kStepZ = kColumnHeight;

    // Reads the block array once and produces all three things derived from
    // it: the clamped opacity field, the window's height map, and the list of
    // light sources.
    void buildOpacity(const u8* const* columns);
    void seedSky();
    void seedBlock();
    // Returns how many cells it settled, for the counters above.
    u32 propagate();
    void clearLight();

    void push(int cell, u8 level)
    {
        light_[usize(cell)] = level;
        buckets_[level].push_back(u32(cell));
    }


    std::vector<u8> opacity_;  // kWindowCells
    std::vector<u8> light_;    // kWindowCells, reused between the two passes
    std::vector<u32> buckets_[16];

    struct Emitter {
        u32 cell;
        u8 value;
    };
    std::vector<Emitter> emitters_;

    u8 heights_[kWindowBlocks * kWindowBlocks] = {};

    u32 skyVisits_ = 0;
    u32 blockVisits_ = 0;
};

}  // namespace mc::world
