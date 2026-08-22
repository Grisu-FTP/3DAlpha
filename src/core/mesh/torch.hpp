#pragma once

// Torches, and the two redstone torches that share their shape.
//
// None of the three appears in the measured 660-chunk world, so this render
// type buys no bytes and no frame time. It is here because an *invisible*
// torch is worse than a wrong one: a player who places a light source and sees
// nothing has no way to tell a missing emitter from a broken light update, and
// every cave in the game is lit by one.
//
// The shape is not the cuboid it looks like. a1.1.2 draws a torch as four
// full-block quads and one small cap, the stick carved out of them by the
// texture's own transparency, and the lean of a wall torch is a *shear* rather
// than a rotation -- the bottom edge of each quad is pushed out while the top
// edge stays put. Both facts are transcribed in the .cpp from `bc.b(ly,III)`
// and `bc.a(ly,DDDDD)` rather than remembered.
//
// The pieces are exposed separately from the emitter for the same reason
// fluid.hpp exposes its corner heights: a mount offset is a number a test can
// state, and getting one of the five metadata cases wrong puts a torch inside
// the wall it is hanging on.

#include "core/block/block_def.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/scratch.hpp"
#include "core/util/types.hpp"

namespace mc::mesh {

// The three constants `bc.b` loads before its switch. Named here because the
// switch's five cases are built out of them and nothing else.
inline constexpr float kTorchTilt = 0.4f;         // how far the bottom is pushed out
inline constexpr float kTorchOffset = 0.5f - kTorchTilt;  // 0.1, the base's shift toward the wall
inline constexpr float kTorchRise = 0.2f;         // how far up the wall the base sits

// Half the stick's width, and the height its top sits at. `bc.a`'s d9 and d10.
inline constexpr float kTorchHalfWidth = 1.0f / 16.0f;
inline constexpr float kTorchTopHeight = 0.625f;

// Where a torch's stick stands and which way it leans, from block metadata.
//
// `bc.b` switches on the metadata itself, so these five cases are the whole
// rule: 1..4 are the four walls and everything else -- 0 and 5 in practice --
// stands on the floor. The offsets are of the block's own corner.
struct TorchMount {
    float ox, oy, oz;  // the base's offset from the block corner
    float dx, dz;      // how far the *bottom* is pushed out; the top leans back
};

TorchMount torchMount(int metadata);

// The light a torch's geometry is drawn at.
//
// `bc.b` asks the cell for its brightness and then discards the answer for any
// block that emits light at all: `if (ly.t[blockID] > 0) brightness = 1.0f`.
// That is not the same as reading the cell, because a lit torch stores light
// **14** and would otherwise draw at 0.93 of full. The unlit redstone torch
// emits 0 and so does read its cell, which is what makes it look dead.
u8 torchLight(const MeshScratch& scratch, int x, int y, int z, const block::BlockDef& def);

// Appends one torch's geometry to `out`, in the 16-byte DetailVertex stream.
// Always five quads: four sides and a cap, no bottom.
void addTorch(const MeshScratch& scratch, int x, int y, int z, const block::BlockDef& def,
              MeshBuilder& out);

}  // namespace mc::mesh
