#pragma once

// The writable window population works through.
//
// Terrain, the surface pass and caves each own one 32,768-byte column and never
// look outside it. **Population is not like that.** Its generators address the
// world in absolute block coordinates and routinely write across chunk
// boundaries -- an ore vein is placed at `chunkX*16 + nextInt(16)` and then
// grows several blocks in every direction, so it reaches into the neighbour
// perhaps a third of the time.
//
// In the original that is invisible, because `World.setBlock` just fetches
// whichever chunk owns the coordinate and generates it if it is missing. We
// cannot generate on demand from inside a generator -- that is unbounded
// recursion on a console with a 32 KB stack -- so instead the caller hands
// population a window of already-generated columns and the generators write
// through it.
//
// **The window must be wide enough that no generator can reach past it.** For
// ores and clay a 3x3 is provably enough: the placement point is inside the
// centre column and the largest vein spans about seven blocks from its centre,
// well under the sixteen a neighbouring column provides. Trees and dungeons
// have not been measured yet and must be re-checked before they are added --
// see docs/worldgen-a1.1.2.md.
//
// Out-of-window writes are **refused, not clamped**, and refusing is the
// faithful answer for the two cases that also occur in the original:
// `World.setBlock` returns false for y outside [0, 128) and for coordinates
// past the world's horizontal limit. A write refused because our window is too
// small is a different thing and a bug; `refusedOutOfWindow()` counts those so
// a test can assert it stays zero.
//
// ---------------------------------------------------------------------------
// **The view also carries light, and it is not the light the world ends up
// with.** That distinction cost some digging, so it is written down here.
//
// Four of population's generators ask the world how bright a spot is before
// they will plant anything -- flowers and both mushrooms directly, trees
// through the height map. What they get is **not** the converged lighting that
// core/world/lighting.hpp computes. Population runs inside chunk generation,
// and `World.updateLights` only ever runs from the game tick, so at that
// moment the box queue has not been drained at all. Confirmed against a real
// World rather than reasoned about:
//
//   * **Block light is exactly zero** during population -- every byte of the
//     freshly generated chunk's block-light array. Nothing writes it
//     synchronously; the only thing that does is the queue.
//   * **`skyLightSubtracted` is zero**, because a new world starts at time 0,
//     so `getBlockLightValue` is just the sky value.
//   * **Sky light is the per-column fill** that `Chunk.relightBlock` writes on
//     the spot: 15 at and above the height map, then decaying downward by each
//     block's opacity until it reaches zero. No horizontal spreading, because
//     that is precisely the part that is queued.
//
// So the light a generator sees depends only on its own column, which is why
// this lives here rather than needing the light engine. It is also why a
// flower can appear under a tree -- the canopy drops the height map, the decay
// under it is 14, 13, 12 rather than 0, and eight is enough to plant on.
//
// The height map and the column fill are kept up to date as blocks are placed,
// following `Chunk.setBlock`'s own rule for when it bothers to relight: see
// setBlock in the .cpp.

#include "core/util/types.hpp"

namespace mc::worldgen {

class PopulationView {
public:
    // The largest window any caller needs. Bounded so the view itself holds no
    // allocation and can live on a worker's stack.
    static constexpr int kMaxColumns = 5;

    // `columns` is countX * countZ pointers in row-major order, x varying
    // slowest -- column (ix, iz) is columns[ix * countZ + iz] and covers chunk
    // (originChunkX + ix, originChunkZ + iz).
    void reset(i32 originChunkX, i32 originChunkZ, int countX, int countZ, u8* const* columns);

    // Absolute block coordinates. Reads outside the window, or outside
    // [0, 128) in y, come back as air -- which is what a generator testing for
    // stone will treat as "not stone", the same conclusion the original reaches
    // when it looks into ungenerated space.
    u8 blockAt(i32 x, i32 y, i32 z) const;

    // Returns whether the write landed, mirroring World.setBlock's own bool.
    bool setBlock(i32 x, i32 y, i32 z, u8 id);

    // Writes refused because the window was too small, as opposed to refused
    // because the original would have refused them too. Should always be zero;
    // anything else means a generator out-reached its window.
    u32 refusedOutOfWindow() const { return refusedOutOfWindow_; }
    void clearRefusals() { refusedOutOfWindow_ = 0; }

    // One bit per column of the window that setBlock has actually landed in,
    // indexed `ix * kMaxColumns + iz` -- so it fits in a u32 for the largest
    // window this supports.
    //
    // Population is expected to write only into the 2x2 quadrant east and south
    // of the chunk it is running for; that is the +8 offset made visible, and
    // it is measured across the whole oracle fixture rather than argued
    // (`tests/populate_test.cpp`, `changedColumns == 4`). The chunk generator
    // depends on it -- a column is final once the four passes that can reach it
    // have run -- so it checks the claim on every chunk it makes rather than
    // trusting six fixture cases to speak for a whole world.
    u32 writtenColumns() const { return writtenColumns_; }

    // `World.getHeightValue`. The lowest y whose whole column above is
    // transparent, so 128 means nothing in it sees the sky. Outside the
    // window, 0 -- which is what the original returns for a chunk that is not
    // loaded, and is why trees at a window edge would be planted at the
    // bedrock rather than on the ground.
    int heightAt(i32 x, i32 z) const;

    // `World.canBlockSeeTheSky`.
    bool canSeeSky(i32 x, i32 y, i32 z) const { return int(y) >= heightAt(x, z); }

    // `World.getBlockLightValue`, as it answers during generation -- the
    // column sky fill, with no block light and no daylight subtraction. See
    // the note at the top of this file for why those two are zero.
    int lightAt(i32 x, i32 y, i32 z) const;

private:
    // Alpha's column layout, x << 11 | z << 7 | y, same as everywhere else.
    static constexpr int localIndex(int x, int y, int z) { return (x << 11) | (z << 7) | y; }

    u8* columnFor(i32 x, i32 z) const;

    // Index into heights_ for a block coordinate, or -1 outside the window.
    int heightIndex(i32 x, i32 z) const;

    // `Chunk.relightBlock`, the synchronous half: recompute this column's
    // height. The sky fill itself is derived on demand in lightAt rather than
    // stored, which is exact because the fill is a pure function of the column
    // and the height -- and 15 steps deep at most, since every step costs at
    // least one.
    void relightColumn(i32 x, i32 z);

    u8* columns_[kMaxColumns * kMaxColumns] = {};

    // One byte per column of the window, indexed the same way as columns_ but
    // 256 deep. 6,400 bytes at the largest window, which is small enough to
    // sit in the view itself rather than needing an allocation.
    u8 heights_[kMaxColumns * kMaxColumns * 256] = {};

    i32 originChunkX_ = 0;
    i32 originChunkZ_ = 0;
    int countX_ = 0;
    int countZ_ = 0;
    u32 refusedOutOfWindow_ = 0;
    u32 writtenColumns_ = 0;
};

}  // namespace mc::worldgen
