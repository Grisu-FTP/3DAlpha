#pragma once

// The cube atlas: every tile a cube face can show, each stored as a 3x3 repeat
// of itself inside a gutter, so a merged face can repeat its tile without the
// GPU's help.
//
// **Why a second atlas exists at all.** Greedy meshing turns a run of equal
// faces into one quad, and one quad over three blocks has to show its tile
// three times. The PICA cannot do that inside a shared atlas: its wrap mode
// belongs to the whole texture, so GPU_REPEAT repeats all of terrain.png rather
// than one tile of it, and the fragment stage has no `fract` to take a
// coordinate modulo a tile. What it can do is sample a region that already *is*
// the tile three times over. So each tile the cube pass can reach gets a 64x64
// slot holding 3x3 copies of it, and a merged face maps [0, w] x [0, h] tiles
// onto the start of those copies. That is the whole trick, and it is why a run
// is capped at kCubeRepeat: past three copies there is nothing left in the slot
// to sample.
//
// **Why three and not four, and why a gutter.** The first layout had 4x4
// copies filling the slot edge to edge, and on hardware every block showed a few
// texels of its *neighbouring slot* -- red TNT on the corners of grass tops,
// which is slot 8, directly below grass's slot 0. The eighth-of-a-texel inset
// that fixed the same symptom in terrain.png (vertex.hpp) is half as large in
// texture coordinates on a 512-texel atlas as on a 256 one, and a merged quad
// interpolates over four tiles instead of one; together that was more error
// than the inset absorbs. So each slot is now
//
//     8 px gutter | tile | tile | tile | 8 px gutter
//
// and the gutters repeat the tile's own first and last texel. A sample that
// falls outside a run's edge by anything under eight texels reads the texel
// that belongs there, whatever the hardware's rounding -- the fix is a margin
// sixty-four times the inset's rather than a better guess at the inset. The
// room came from the fourth copy: three copies and two gutters are the same 64
// texels, and on the real world a run of three costs 5.8 % more quads than a
// run of four (docs/status.md §22).
//
// What the gutter cannot cover is the far edge of a run *shorter* than three,
// which ends against the next copy of the same tile. A stray sample there
// reads the tile's own opposite edge -- the texel the next identical block
// would show -- rather than another tile's, and the inset still stands in
// front of it.
//
// **The size is set by VRAM, not by what would merge best.** A slot for every
// one of the 256 tiles would be a 1024x1024 atlas -- 4 MB at RGBA8, which is the
// figure `core/texture/atlas_image.hpp` already refused for HD packs, on a
// console with 6 MB of VRAM and 1.5 MB of it in render targets. But the cube
// pass cannot reach 256 tiles: it draws only what a cube block's six faces
// name, and a1.1.2 has 54 of those. So the slots are handed out to those tiles
// alone, 8x8 of them in a 512x512 texture -- **1 MB**. A static_assert below
// turns "a later version has more cube tiles than slots" into a build failure
// rather than a black face.
//
// The detail and translucent passes keep the ordinary 256x256 atlas. Fluid
// reads across tile boundaries on purpose (see core/mesh/fluid.cpp) and the
// animated tiles live there, so moving them would change what they sample for
// no merge in return: nothing but cubes is ever merged.

#include "core/block/registry.hpp"
#include "core/block/world_texture.hpp"
#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"

namespace mc::mesh {

// Copies of a tile along each edge of its slot, which is also the longest run
// the mesher may merge along either axis.
inline constexpr int kCubeRepeat = 3;
inline constexpr int kCubeTilePixels = 16;
inline constexpr int kCubeGutterPixels = 8;
inline constexpr int kCubeSlotPixels = 2 * kCubeGutterPixels + kCubeRepeat * kCubeTilePixels;
inline constexpr int kCubeSlotsPerEdge = 8;
inline constexpr int kCubeSlotCount = kCubeSlotsPerEdge * kCubeSlotsPerEdge;
inline constexpr int kCubeAtlasEdge = kCubeSlotPixels * kCubeSlotsPerEdge;
static_assert(kCubeSlotPixels == 64, "a slot is a power of two so the atlas is one");
static_assert(kCubeAtlasEdge == 512, "the VRAM figure in the header is for 512x512");

// UVs use the same 1/16384-of-the-texture units the ordinary atlas does, so the
// 12-byte vertex needs no second encoding. One texel is 32 units, one copy of
// the tile 512, a gutter 256 and a slot 2048.
inline constexpr int kCubeUvPerTexel = kUvUnitsPerAtlas / kCubeAtlasEdge;
inline constexpr int kCubeUvPerTile = kCubeUvPerTexel * kCubeTilePixels;
inline constexpr int kCubeUvGutter = kCubeUvPerTexel * kCubeGutterPixels;
inline constexpr int kCubeUvPerSlot = kCubeUvPerTexel * kCubeSlotPixels;
static_assert(kCubeUvPerTexel * kCubeAtlasEdge == kUvUnitsPerAtlas,
              "a texel must be a whole number of UV units");

// **The same eighth of a texel `kUvInset` keeps**, which the gutter now stands
// behind rather than replaces: it is still what keeps a sample off the boundary
// between two copies at the end of a short run. See vertex.hpp.
inline constexpr int kCubeUvInset = kCubeUvPerTexel / 8;
static_assert(kCubeUvInset * 8 == kCubeUvPerTexel, "the inset must be a whole number of units");

inline constexpr u8 kNoCubeSlot = 0xFF;

struct CubeAtlasLayout {
    u8 slotOfTile[kAtlasTileCount];  // kNoCubeSlot for a tile no cube face shows
    u16 tileOfSlot[kCubeSlotCount];  // meaningful below slotCount only
    int slotCount;
};

namespace detail {

constexpr void claimCubeSlot(CubeAtlasLayout& layout, int tile)
{
    // The mesher sends any texture past the atlas to tile 0, so that is the
    // tile such a face actually shows.
    const int t = tile >= 0 && tile < kAtlasTileCount ? tile : 0;
    if (layout.slotOfTile[t] != kNoCubeSlot) {
        return;
    }
    // Counted past the end rather than stopped at it, so the static_assert
    // below can say how many slots the version actually needs.
    if (layout.slotCount < kCubeSlotCount) {
        layout.slotOfTile[t] = u8(layout.slotCount);
        layout.tileOfSlot[layout.slotCount] = u16(t);
    }
    ++layout.slotCount;
}

constexpr void wantTile(bool* wanted, int tile)
{
    wanted[tile >= 0 && tile < kAtlasTileCount ? tile : 0] = true;
}

}  // namespace detail

// Every tile `MeshBuilder::addQuad` can be handed, in ascending order so the
// layout is the same on every build of the same version.
//
// That is the six faces of every block whose render type is a cube -- known or
// not, because an unknown id is drawn as a cube on purpose -- the in-world rows
// of the metadata face table for those blocks (a furnace's mouth), **every tile
// a world-texture rule can produce** (a chest's front, and the four halves of a
// double chest's picture, which appear in no block's `faces` row at all), the
// unknown block itself, and tile 0 for a texture index outside the atlas. Bounded cubes
// (slabs) are in the list although they draw through the detail stream: the
// question is what a cube-type block *could* send, and asking it any narrower
// would make the answer depend on the mesher's dispatch staying exactly as it
// is.
constexpr CubeAtlasLayout buildCubeAtlasLayout()
{
    CubeAtlasLayout layout{};
    for (int t = 0; t < kAtlasTileCount; ++t) {
        layout.slotOfTile[t] = kNoCubeSlot;
    }

    bool wanted[kAtlasTileCount] = {};
    detail::wantTile(wanted, 0);
    for (int face = 0; face < kFaceCount; ++face) {
        detail::wantTile(wanted, mcver::kUnknownBlock.faces[face]);
    }
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (def.render != block::RenderType::Cube) {
            continue;
        }
        for (int face = 0; face < kFaceCount; ++face) {
            detail::wantTile(wanted, def.faces[face]);
        }
        // A block whose faces are a rule rather than a row shows tiles its
        // `faces` never names -- without these four, a double chest samples
        // tile 0 and wears grass down its side.
        u16 ruleTiles[block::kMaxWorldTextureTiles] = {};
        const int ruleCount = block::worldTextureTiles(def.worldTexture, def.texture, ruleTiles);
        for (int i = 0; i < ruleCount; ++i) {
            detail::wantTile(wanted, ruleTiles[i]);
        }

        const int row = mcver::kMetadataFaceRow[id];
        if (row == 0) {
            continue;
        }
        for (int metadata = 0; metadata < 16; ++metadata) {
            for (int face = 0; face < kFaceCount; ++face) {
                detail::wantTile(wanted, mcver::kMetadataFaces[row - 1][metadata][face]);
            }
        }
    }

    for (int t = 0; t < kAtlasTileCount; ++t) {
        if (wanted[t]) {
            detail::claimCubeSlot(layout, t);
        }
    }
    return layout;
}

inline constexpr CubeAtlasLayout kCubeAtlas = buildCubeAtlasLayout();

static_assert(kCubeAtlas.slotCount <= kCubeSlotCount,
              "this version's cube blocks show more tiles than the cube atlas has slots; "
              "a 2x2 repeat in the same 512x512 would hold all 256 -- see cube_atlas.hpp");

// The slot a cube face with this texture samples. Never kNoCubeSlot: a tile
// the layout does not know is one `buildCubeAtlasLayout` was not told about,
// and tile 0's slot is where the mesher sends every other texture it cannot
// place.
constexpr int cubeSlotOf(int texture)
{
    const int tile = texture >= 0 && texture < kAtlasTileCount ? texture : 0;
    const u8 slot = kCubeAtlas.slotOfTile[tile];
    return slot != kNoCubeSlot ? slot : kCubeAtlas.slotOfTile[0];
}

// Which texel of a tile a slot holds at `offset` texels along one axis: the
// gutters repeat the tile's first and last texel, and the copies between them
// are the tile over and over.
constexpr int cubeSlotTexel(int offset)
{
    const int inCopies = offset - kCubeGutterPixels;
    if (inCopies < 0) {
        return 0;
    }
    if (inCopies >= kCubeRepeat * kCubeTilePixels) {
        return kCubeTilePixels - 1;
    }
    return inCopies % kCubeTilePixels;
}

// Which texel of the ordinary atlas the cube atlas holds at (x, y), both in
// image order (row 0 at the top, as terrain.png is). False for the unused
// slots past `slotCount`, which the upload leaves transparent.
constexpr bool cubeAtlasSource(int x, int y, int* atlasX, int* atlasY)
{
    const int slot = (y / kCubeSlotPixels) * kCubeSlotsPerEdge + x / kCubeSlotPixels;
    if (slot >= kCubeAtlas.slotCount) {
        return false;
    }
    const int tile = kCubeAtlas.tileOfSlot[slot];
    *atlasX = (tile % kAtlasTilesPerEdge) * kCubeTilePixels + cubeSlotTexel(x % kCubeSlotPixels);
    *atlasY = (tile / kAtlasTilesPerEdge) * kCubeTilePixels + cubeSlotTexel(y % kCubeSlotPixels);
    return true;
}

// The UV of a run's near edge, and of a point `tiles` copies into it, both on
// one axis. `cubeUvStart` / `cubeUvEnd` are what a quad's two edges carry: past
// the slot's gutter, and in by kCubeUvInset from the copies' start at one end
// and from the end of the run at the other.
constexpr i16 cubeUvStart(int slotAxis)
{
    return i16(slotAxis * kCubeUvPerSlot + kCubeUvGutter + kCubeUvInset);
}

constexpr i16 cubeUvEnd(int slotAxis, int tiles)
{
    return i16(slotAxis * kCubeUvPerSlot + kCubeUvGutter + tiles * kCubeUvPerTile - kCubeUvInset);
}

static_assert(cubeUvEnd(kCubeSlotsPerEdge - 1, kCubeRepeat) < 32767,
              "the far edge of the last slot must fit a signed short");

}  // namespace mc::mesh
