#pragma once

// Water and lava.
//
// 2.73 % of the blocks in the measured 660-chunk world and 99.8 % of everything
// that is not a cube, so this is the one non-cube render type a world visibly
// misses. It is also the only one whose *shape* depends on its neighbours: a
// fluid's top face is not flat, it is four independent corner heights sampled
// from the four cells meeting at each corner, which is what makes a lake's edge
// slope down into the shore instead of ending in a wall.
//
// Every rule here was read out of a1.1.2's own RenderBlocks and BlockFluid
// rather than remembered; the transcriptions are in the .cpp next to the code
// that implements them, and docs/status.md records the method signatures. The
// pieces are exposed here separately from the emitter because each of them is
// worth pinning on its own -- a corner height is a number a test can state.

#include "core/block/block_def.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/scratch.hpp"
#include "core/util/types.hpp"

namespace mc::mesh {

// A fluid cell's surface height as a fraction of the block, from its metadata
// level. `jp.b(I)F` in the jar: **ninths, not eighths**, so a source block's
// surface sits at 8/9 of a block and not 7/8. Levels 8..15 are the "falling"
// flag and read as a source.
float fluidHeightPercent(int level);

// The four corner heights of a fluid block's top face, in the order the
// original samples them -- (x,z), (x,z+1), (x+1,z+1), (x+1,z) -- each in
// [0,1] as an offset above the block's own base.
struct FluidCorners {
    float h[4];
};

FluidCorners fluidCorners(const MeshScratch& scratch, int x, int y, int z, u8 material);

// The angle the top face's texture is rotated by, in radians, or kNoFlow when
// the fluid is standing still. The sentinel is the original's own -1000, kept
// rather than replaced by an optional because the renderer branches on it twice
// and both branches are transcribed.
inline constexpr float kNoFlow = -1000.0f;

float fluidFlowAngle(const MeshScratch& scratch, int x, int y, int z, u8 material);

// Whether a fluid draws the face looking at the cell (x,y,z). `face` is the
// face of the *fluid* that this cell is behind, in mc::mesh::Face numbering.
bool fluidFaceVisible(const MeshScratch& scratch, int x, int y, int z, int face, u8 material);

// The light a fluid face reads from a cell: `jp.c(nm,III)F` overrides
// Block.getBlockBrightness with **the brighter of the cell and the one above
// it**, and every face in the fluid renderer goes through that override.
//
// The original compares two floats out of the brightness table; we carry two
// packed (sky << 4) | block bytes and take the maximum of each nibble. Those
// agree exactly, because a texel of the lightmap is
// `lightBrightness(effectiveLightLevel(sky, block, subtracted))` and both of
// those are monotone, so the brighter of two cells is the cell-wise brighter
// of their two levels.
u8 fluidLight(const MeshScratch& scratch, int x, int y, int z);

// Appends one fluid block's geometry to `out`, in the 16-byte DetailVertex
// stream. Emits nothing when every face is hidden.
void addFluid(const MeshScratch& scratch, int x, int y, int z, const block::BlockDef& def,
              MeshBuilder& out);

}  // namespace mc::mesh
