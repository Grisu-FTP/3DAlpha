#pragma once

// What the engine knows about a block.
//
// The renderer and the physics never see a block id: they see a RenderType and
// a handful of flags. That is the whole anti-hardcoding rule for blocks -- add
// a version whose stone is id 1 and whose "stone stairs" is id 109, and nothing
// outside the generated table changes. See CONTRIBUTING.md.
//
// The table itself is generated from data/<version>/blocks.json into
// build/<version>/gen/blocks.hpp, and that JSON is in turn recovered from an
// original client jar by tools/extract_blocks.py. Nothing in this file is
// version-specific.

#include "core/util/types.hpp"

namespace mc::block {

// A numeric block id as it appears in the world arrays and on the wire.
// Deliberately a plain integer and not the generated enum: a world file or a
// server can contain an id this build does not know about, and storage has to
// round-trip it rather than reject it. 16 bits because later versions exceed
// 255 ids, and widening it after the fact would touch every array.
using BlockId = u16;

// Air is 0 in every version this project targets, and the emptiness checks that
// let the mesher skip whole sections need to know it.
inline constexpr BlockId kAir = 0;

// How a block turns into geometry. The numbering matches the original game's
// getRenderType, because that is where the values were read from and keeping
// them aligned makes the extraction re-checkable.
enum class RenderType : u8 {
    None = 0,          // drawn by something else entirely (signs)
    Cube,              // the ordinary case, and the only one greedy meshing
    Cross,             // two intersecting quads: flowers, saplings, sugar cane
    Torch,
    Fire,
    Fluid,
    RedstoneWire,
    Crops,
    Door,
    Ladder,
    Rail,
    Stairs,
    Fence,
    Lever,
    Cactus,
    Count,
};

const char* renderTypeName(RenderType type);

struct BlockDef {
    const char* name;

    float hardness;    // -1 means unbreakable
    float resistance;  // against explosions

    // Index into the terrain atlas: the value the original constructor was
    // given, and what the block shows anywhere a single tile is wanted.
    u16 texture;

    // Per-face atlas tiles, in the game's face order (`mc::mesh::Face`, which
    // this deliberately matches so the table can be read straight out of the
    // jar): grass is dirt underneath and grass on top, a log's rings face up,
    // a furnace's mouth faces one way. Always filled -- a block with one
    // texture on every side repeats it -- so the mesher indexes rather than
    // branching, and `faces[f]` is always the answer.
    //
    // Recovered by interpreting getBlockTexture out of the client jar rather
    // than by reading it, because every one of them is a branch on the face
    // index. See tools/javap.py.
    //
    // Faces whose tile depends on the surrounding world (which way a chest
    // faces, snow on grass) or on block metadata (wheat's growth stage) hold
    // the no-metadata, no-neighbours answer, and blocks.json records which
    // those are. Nothing reads metadata yet.
    u16 faces[6];

    RenderType render;

    // The original's own material grouping, as a dense index -- 0 is air and
    // whatever else this version never constructs. Two questions are asked of
    // it and no others: whether two blocks share a material, which is how a
    // fluid recognises its own kind rather than by block id, and `solid`
    // below.
    //
    // Recovered from the jar like everything else here: `Block`'s constructor
    // names the Material class in its signature, and the field it lands in is
    // found by running the constructor with a marker. See
    // tools/extract_blocks.py.
    u8 material;

    u8 light;    // 0..15 emitted
    u8 opacity;  // 0..255 absorbed as light passes through

    // The face-culling test: a face touching an opaque neighbour is not
    // emitted. This is the single most consulted property in the mesher, which
    // is why it is a byte and not a call.
    bool opaque;

    // Renders as a full cube. Distinct from `opaque` -- glass and leaves are
    // full cubes that do not block light.
    bool fullCube;

    // `Block.opaqueCubeLookup[id]`, which is **not** the same question as
    // `opaque` above even though it agrees with it for all but two blocks.
    //
    // The array is filled once in the Block constructor. `opaque` is what
    // `isOpaqueCube()` answers at *runtime*, and for leaves that is
    // `!fancyGraphics` -- a video option, defaulted to fancy, applied long
    // after the array was built. So the cached answer for leaves is "solid"
    // and the live one is "see-through", and both are correct.
    //
    // Which to use is decided by what the original consults. Face culling in
    // the mesher calls the live method, so it wants `opaque`. **World
    // generation reads the array**: BlockMushroom's ground test is a plain
    // lookup into it, which is why a1.1.2 will grow a mushroom on a leaf
    // block and why that needs this column rather than the one next to it.
    //
    // Pinned against a loaded jar by tests/opaque_cube_test.cpp.
    bool opaqueCube;

    // `getRenderBlockPass() == 1`: this block's geometry belongs in the sorted,
    // blended pass rather than the opaque one.
    //
    // Three blocks in a1.1.2 -- still water, flowing water and ice -- and
    // notably **not** lava, which shares a class with water and is told apart
    // only by its material. Glass and leaves are not translucent either: they
    // are cut out by the alpha test in the opaque pass, which is why this is
    // not the same question as `opaque`.
    bool translucent;

    // `Material.isSolid()`. Distinct from **both** of the above and from the
    // render type, which is exactly why it is its own column rather than
    // something derived: glass and leaves are solid but not opaque, stairs and
    // doors are solid but not full cubes, and a stone button and a snow layer
    // are neither solid nor anything else that would give them away. Any rule
    // inferred from the other columns gets at least four of a1.1.2's blocks
    // wrong.
    //
    // What consults it: a fluid's surface height, where a solid neighbour
    // leaves the height alone and a non-solid one pulls it down.
    bool solid;

    // False for ids this version does not define. A world or a server can name
    // a block we have never heard of, and the mesher has to survive it rather
    // than index past the end of the table. Unknown ids are given a solid
    // opaque cube on purpose: a visible wrong block is a bug report, an
    // invisible one is a mystery.
    bool known;
};

}  // namespace mc::block
