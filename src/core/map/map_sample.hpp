#pragma once

// One chunk, reduced to what a map draws: the block you would see looking
// straight down, how high it is, and how deep the water over it goes.
//
// **a1.1.2 has no maps at all** -- there is no map item, no `MapData`, no
// `MapColor`, and nothing in the client that projects a column onto a plane. So
// unlike almost everything else in this tree there is no oracle to be exact
// against, and the rule that replaces it is the one the request asked for: be
// the map a *later* version would draw of the same world. That is a real
// specification -- `MapData.updateVisitedBlocks` -- and every choice below
// either follows it or says where it cannot.
//
// What is followed:
//
//   * **One pixel is one block**, which is the later `scale = 0`. So a map
//     pixel and a chunk edge line up by construction: sixteen pixels to a
//     chunk, always, and a pixel of ours names the same block as the same
//     pixel of a later version's map of the same ground.
//   * **The surface is found by scanning down from the height map**, not by
//     taking the topmost solid block: later versions start at
//     `getHeightValue()` and walk down past anything whose map colour is the
//     air colour. Our heightMap is the same array -- one above the highest
//     block that stops light -- so the scan starts in the same place.
//   * **Water is measured rather than drawn flat.** A liquid surface records
//     how many blocks of liquid are under it, because the later shading uses
//     that depth and not the height difference, which is what makes shallows
//     read as shallow.
//
// What cannot be followed, and why: later versions get the colour from
// `Material`'s `MapColor`, and a1.1.2's materials are coarser than the ones
// that table was written against -- grass, dirt and farmland are all one
// material here, so a material-keyed table would paint every meadow brown. The
// colour therefore comes from the texture pack instead; see map_palette.hpp.

#include "core/block/block_def.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

namespace mc::map {

// The chunk edge, in map pixels. One pixel per block is the later `scale = 0`,
// so this is the chunk width and is not a free choice -- see the header note.
inline constexpr int kChunkPixels = world::ChunkColumn::kWidth;
inline constexpr int kChunkSamples = kChunkPixels * kChunkPixels;

// A whole chunk's worth of surface, indexed **`[x * 16 + z]`**.
//
// **That is x-major, which is the opposite of ChunkColumn::heightMap, and it is
// the difference between a redraw that fits in a frame and one that does not.**
// The renderer walks a column of the map at a time -- it has to, because a
// pixel's brightness is the height step to the block one to the north, and
// carrying that forward costs a register in this order and a buffer the width
// of the screen in the other. Storing z-major would then make every pixel of
// that walk a 32-byte stride, which is a fresh cache line per pixel on a 16 KB
// L1. Storing x-major makes a whole chunk's worth of it one line. Measured on
// the dev host at -O3, that one change took a 192x192 redraw from 361 us to the
// figure in docs/status.md.
//
// The cost is paid where it does not matter: `sampleChunk` writes strided
// instead of reading strided, once per chunk ever, against a scan that is
// already touching the block arrays.
//
// **Block ids rather than colours**, which is what lets a texture pack change
// under a map that has already been drawn: the pack decides what stone looks
// like, the sample decides that it is stone. It is also what makes the saved
// form pack-independent (see map_store.hpp).
struct MapChunkSample {
    // `[x * 16 + z]`; see the note above.
    // The block a viewer looking straight down would see. `block::kAir` where
    // the column is empty all the way to bedrock, which is a real answer and
    // not a missing one -- a chunk of sky over the void.
    block::BlockId surface[kChunkSamples] = {};

    // Where that block is. Bounded by the world height, so a byte holds it for
    // every version this project targets.
    u8 height[kChunkSamples] = {};

    // Blocks of liquid at and under the surface, 0 when the surface is not a
    // liquid. Saturates rather than wraps: a column of lava taller than 255 is
    // "as deep as it gets", which is all the shading asks.
    u8 depth[kChunkSamples] = {};
};

// **Does this block show up on a map at all?**
//
// Later versions ask `Material`: air, glass, fire and `circuits` -- rails,
// levers, buttons, redstone and torches -- all carry the air colour, so the
// downward scan walks straight past them and paints whatever is underneath. Our
// equivalent is the render type, which is where that same set already lives in
// this tree: every one of those blocks is drawn by something other than a cube,
// and none of them would read as anything at one pixel across.
//
// The one deliberate divergence is **glass**, which later versions make
// invisible on a map and this does not: glass is a cube here, it has no
// material distinction from stone that this could key on, and inventing one
// would be exactly the version-specific `switch` CONTRIBUTING.md forbids. A
// glass roof therefore shows as glass, tinted by whatever the pack draws it as.
bool showsOnMap(block::BlockId id);

// **Is this the block later versions would shade by water depth?**
//
// `MapData` branches on `getMapColor() == MapColor.waterColor`, and there is no
// map-colour table here to compare against -- see map_palette.hpp for why there
// cannot be one. What distinguishes the two fluids a1.1.2 has, without naming
// either of them, is that one of them is a light source: water emits nothing
// and lava emits fifteen. So a dark fluid takes the depth shading and a bright
// one takes the ordinary height shading, which is also what later versions do
// with lava -- its map colour is the TNT colour, not the water one.
bool isMapWater(block::BlockId id);

// Fills `out` from one column. Cheap enough to run inside a frame -- the scan
// starts at the height map rather than at the world ceiling, so an ordinary
// surface chunk touches two or three blocks per pixel -- but it is still 256 of
// them, so callers budget it rather than sampling everything at once.
void sampleChunk(const world::ChunkColumn& column, MapChunkSample* out);

}  // namespace mc::map
