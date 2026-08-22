#pragma once

// The world vertex format, the face numbering, and the cube geometry every
// render type builds on.
//
// The vertex is 12 bytes and a quad is 4 vertices -- 48 bytes per quad against
// craftus_reloaded's 96. Field order here IS the memory layout: citro3d's
// AttrInfo_AddLoader takes no offset, it accumulates component sizes in
// declaration order, so reordering these fields silently reinterprets the
// buffer. shaders/world.v.pica documents the matching attribute setup.
//
// See docs/3ds-performance.md section 1 for why 12 and not 11 or 8.

#include "core/util/types.hpp"

namespace mc::mesh {

// Face numbering is the original game's, not a convention of ours: a1.1.2's
// RenderBlocks calls its six face routines in this order, and Block's
// getBlockTexture takes the same index. Keeping the numbering aligned is what
// lets the per-face texture table be read straight out of the jar.
enum Face : u8 {
    kFaceNegY = 0,  // bottom
    kFacePosY = 1,  // top
    kFaceNegZ = 2,  // north
    kFacePosZ = 3,  // south
    kFaceNegX = 4,  // west
    kFacePosX = 5,  // east
    kFaceCount = 6,
};

struct FaceOffset {
    i8 dx, dy, dz;
};

inline constexpr FaceOffset kFaceOffset[kFaceCount] = {
    {0, -1, 0},  // kFaceNegY
    {0, +1, 0},  // kFacePosY
    {0, 0, -1},  // kFaceNegZ
    {0, 0, +1},  // kFacePosZ
    {-1, 0, 0},  // kFaceNegX
    {+1, 0, 0},  // kFacePosX
};

// Directional shading, the flat per-face darkening the original applies before
// any lighting. These four values were read out of the a1.1.2 client jar rather
// than remembered: RenderBlocks' standard-block routine loads 0.5, 1.0, 0.8 and
// 0.6 into four locals and multiplies each face's colour by one of them. The
// two Z faces share a value and so do the two X faces, which is why the table
// has six entries but only four distinct numbers.
inline constexpr float kFaceShadeFloat[kFaceCount] = {
    0.5f,  // bottom
    1.0f,  // top
    0.8f,  // north
    0.8f,  // south
    0.6f,  // west
    0.6f,  // east
};

// The vertex carries colour as bytes, so the shade is pre-scaled here rather
// than per quad. Rounding rather than truncating matters: 0.8 * 255 truncates
// to 203, and a face one level too dark against a neighbour that rounded the
// other way is visible as a seam.
constexpr u8 shadeToByte(float shade)
{
    return static_cast<u8>(shade * 255.0f + 0.5f);
}

inline constexpr u8 kFaceShade[kFaceCount] = {
    shadeToByte(kFaceShadeFloat[0]), shadeToByte(kFaceShadeFloat[1]),
    shadeToByte(kFaceShadeFloat[2]), shadeToByte(kFaceShadeFloat[3]),
    shadeToByte(kFaceShadeFloat[4]), shadeToByte(kFaceShadeFloat[5]),
};

// The four corners of each face as unit-cube offsets, wound counter-clockwise
// seen from outside the block so the GPU's default front-face test keeps them.
// Every entry was checked by taking the cross product of the first two edges
// and confirming it points along the face normal -- a single corner in the
// wrong order makes a face vanish only from one side, which is a miserable bug
// to find on hardware.
struct Corner {
    u8 x, y, z;
};

inline constexpr Corner kFaceCorner[kFaceCount][4] = {
    {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},  // kFaceNegY
    {{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}},  // kFacePosY
    {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}},  // kFaceNegZ
    {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},  // kFacePosZ
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},  // kFaceNegX
    {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}},  // kFacePosX
};

// Tile-local UV for each corner above, in units of one tile. v runs downward
// because terrain.png's first row is its top row. The side faces all come out
// with the same sequence, which is a consequence of the corner winding rather
// than a coincidence worth compressing away.
inline constexpr u8 kFaceCornerUV[kFaceCount][4][2] = {
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},  // kFaceNegY: u,v follow x,z
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}},  // kFacePosY: u,v follow x,z
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}},  // kFaceNegZ
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}},  // kFacePosZ
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}},  // kFaceNegX
    {{0, 1}, {1, 1}, {1, 0}, {0, 0}},  // kFacePosX
};

// The pre-1.5 terrain atlas is a 16x16 grid of tiles, which is what makes a
// texture index a single number in every era format we target.
inline constexpr int kAtlasTilesPerEdge = 16;
inline constexpr int kAtlasTileCount = kAtlasTilesPerEdge * kAtlasTilesPerEdge;

// UVs are s16 in 1/16384 units, so the whole atlas spans 0..16384 and one tile
// is exactly 1024. Fixed point rather than float keeps the vertex at 12 bytes,
// and 16384 was chosen over 32768 so the far edge still fits a signed short.
inline constexpr int kUvUnitsPerAtlas = 16384;
inline constexpr int kUvUnitsPerTile = kUvUnitsPerAtlas / kAtlasTilesPerEdge;

struct WorldVertex {
    i16 u, v;    // atlas coordinates, 1/16384 units
    u8 x, y, z;  // position inside a 16^3 section, 0..16
    u8 face;     // face index; the geometry-shader path needs it, the rest is padding
    // Static colour, never time-dependent. Face shade today; face shade x AO once
    // smooth lighting lands.
    //
    // Three bytes rather than one, even though a1.1.2 only ever needs the one
    // grey level: its Block.colorMultiplier returns 0xFFFFFF for every block in
    // the game and no subclass overrides it, so there is no tint in this
    // version at all. The Beta-era biome colouring turns that same path on, and
    // having the channels here means porting fills a field instead of changing
    // the vertex format. See docs/status.md.
    u8 r, g, b;
    u8 light;    // (skyLevel << 4) | blockLevel, unpacked into a lightmap UV by the shader
};

static_assert(sizeof(WorldVertex) == 12, "the vertex format is load-bearing; see world.v.pica");

// ---------------------------------------------------------------------------
// The third format: one 8-byte vertex per quad, expanded by a geometry shader
// ---------------------------------------------------------------------------
//
// The M2 gate failed on the format above: profiled on a New 3DS XL, the cube
// pass costs a flat 0.208 us per quad and is indifferent to how much of the
// screen it covers, so it is bound by what comes *before* the fragment stage.
// 48 bytes of vertex plus 12 bytes of index per quad is 60 bytes the GPU has to
// pull through FCRAM for every quad on screen; this is 8. See
// docs/3ds-performance.md section 2.
//
// **This is an experiment with a decisive answer either way.** It cuts vertex
// bytes 7.5x and vertex-shader invocations 4x while leaving the triangle count
// exactly as it was. If the pass gets faster, the cost was the vertex side and
// this becomes the path; if it does not move, the cost is per-triangle setup,
// nothing on the vertex side will ever help, and the only remaining lever is
// emitting fewer triangles. Both outcomes are worth the shader.
//
// What it gives up, recorded because it is the reason this cannot simply
// replace WorldVertex:
//
//   * **Per-quad colour.** Face shade comes from a uniform table indexed by the
//     face, so every quad of a given face direction is the same brightness.
//     Exact for a1.1.2 -- its Block.colorMultiplier is white for every block --
//     and wrong for any version with biome tint, which is Beta onward.
//   * **Per-corner anything**, so smooth lighting and AO are out. `ao` below is
//     a reserved byte, not an implementation.
//   * **Sub-block geometry**, the same limit WorldVertex has. Cubes only.
struct QuadVertex {
    u8 x, y, z;  // block position inside the section, 0..15 (not 0..16: this is a cell)
    u8 face;     // face index, which selects the corner basis and the shade

    // The tile split into its atlas row and column rather than left as one
    // index, because the shader would otherwise have to divide by 16 and floor
    // it -- three instructions per quad to undo an arithmetic the mesher did
    // for free.
    u8 tileX, tileY;
    u8 light;  // (skyLevel << 4) | blockLevel, exactly as WorldVertex carries it
    u8 ao;     // reserved; zero. See the note above about per-corner values
};

static_assert(sizeof(QuadVertex) == 8, "8 bytes per quad is the entire point; see quad.v.pica");

// The corner basis a geometry shader expands a quad from.
//
// Each face's four corners are `base + i*e1 + j*e2` over (i,j) = (0,0), (1,0),
// (1,1), (0,1) -- which is kFaceCorner's own order, so the geometry shader
// reproduces the 12-byte path's winding rather than inventing one. The
// static_asserts below check exactly that, for every face and every corner, so
// a wrong sign here fails the build instead of turning a face inside out on
// hardware.
//
// `uvSign` is the whole of the UV difference between the faces. Tile-local u is
// i for every face; tile-local v is j for the bottom face and 1-j for the other
// five, which is one number: v = (1 - uvSign)/2 + j*uvSign.
struct FaceBasis {
    i8 base[3];
    i8 e1[3];
    i8 e2[3];
    i8 uvSign;
};

inline constexpr FaceBasis kFaceBasis[kFaceCount] = {
    {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, +1},  // kFaceNegY
    {{0, 1, 1}, {1, 0, 0}, {0, 0, -1}, -1},  // kFacePosY
    {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, -1},  // kFaceNegZ
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, -1},  // kFacePosZ
    {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, -1},  // kFaceNegX
    {{1, 0, 1}, {0, 0, -1}, {0, 1, 0}, -1},  // kFacePosX
};

// (i, j) for corners 0..3, the order kFaceCorner lists them in.
inline constexpr u8 kCornerIJ[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

constexpr bool faceBasisMatchesCorners()
{
    for (int face = 0; face < kFaceCount; ++face) {
        const FaceBasis& b = kFaceBasis[face];
        for (int c = 0; c < 4; ++c) {
            const int i = kCornerIJ[c][0];
            const int j = kCornerIJ[c][1];
            const Corner& want = kFaceCorner[face][c];
            if (b.base[0] + i * b.e1[0] + j * b.e2[0] != want.x) return false;
            if (b.base[1] + i * b.e1[1] + j * b.e2[1] != want.y) return false;
            if (b.base[2] + i * b.e1[2] + j * b.e2[2] != want.z) return false;

            const int v = (1 - b.uvSign) / 2 + j * b.uvSign;
            if (i != kFaceCornerUV[face][c][0] || v != kFaceCornerUV[face][c][1]) return false;
        }
    }
    return true;
}

static_assert(faceBasisMatchesCorners(),
              "the geometry shader's basis must rebuild kFaceCorner and kFaceCornerUV exactly");

// Which of the two cube encodings a section's cube range is written in. Carried
// in MeshRanges rather than kept as a global, so a mesh describes itself and the
// draw loop cannot read one format's bytes as the other's.
enum class CubeFormat : u8 {
    Vertices,  // WorldVertex, four per quad, drawn through the shared index buffer
    Quads,     // QuadVertex, one per quad, expanded by shaders/quad.g.pica
};

constexpr usize cubeBytesPerQuad(CubeFormat format)
{
    return format == CubeFormat::Quads ? sizeof(QuadVertex) : 4 * sizeof(WorldVertex);
}

// ---------------------------------------------------------------------------
// The second vertex format: everything that is not a cube
// ---------------------------------------------------------------------------
//
// WorldVertex stores position as one byte per axis at one unit per block, which
// is exact for cubes and cannot express anything else. The original's non-cube
// shapes are not on the block grid and several are not even on the 1/16 model
// grid -- read out of a1.1.2's RenderBlocks rather than assumed:
//
//     cross (flowers, saplings, sugar cane)   +-0.45 from the centre
//     ladder                                   0.05 from the wall
//     torch                                    0.4 tilt, 0.2 rise
//     fluid                                    corner heights in ninths, averaged over
//                                              four cells, less a 0.01-texel lip
//     fence, crops, rail, stairs               multiples of 1/16
//
// So non-cube geometry gets its own format rather than widening the one that
// carries 97 % of the world's blocks -- doing that would add about 10 MB to a
// real world's geometry and put render distance 8 over the 12 MB the old 3DS
// has for it. Non-cube blocks are 2.7 % of a measured world, so they can afford
// to be four bytes bigger and the terrain does not pay a byte.
//
// **1/1024 of a block, in a signed short.** That makes every value on any
// power-of-two grid -- 1/8, 1/16, 1/32, 3/8, 7/16 -- exact, and the five
// decimal constants above land within 1/2048 of a block, which at point-blank
// range on a 400-pixel screen is a tenth of a pixel. Float positions would be
// bit-exact and cost four more bytes per vertex to be invisibly more correct.
// 16384 is the whole section, so a short has room to spare.
inline constexpr int kDetailUnitsPerBlock = 1024;
inline constexpr int kDetailUnitsPerSection = kDetailUnitsPerBlock * 16;

static_assert(kDetailUnitsPerSection < 32767,
              "a section must fit a signed short at this precision");

// 16 bytes rather than the 14 the fields need. Every attribute then starts on a
// 4-byte boundary at every multiple of the stride, which matters because the
// only alignment rule this project has actually confirmed on hardware is the
// 2-byte one in the M0 probe, and this format has never run on a console. The
// two spare bytes go where WorldVertex keeps its face index, for the same
// reason: the geometry-shader path will want it.
struct DetailVertex {
    i16 x, y, z;  // position inside the section, 1/1024 block, 0..16384
    i16 face;     // face index where one applies; padding otherwise

    i16 u, v;     // atlas coordinates, the same 1/16384 units WorldVertex uses

    u8 r, g, b;   // face shade, or full brightness for the shapes the original
                  // draws unshaded -- crossed squares among them
    u8 light;     // (skyLevel << 4) | blockLevel
};

static_assert(sizeof(DetailVertex) == 16, "see the alignment note above");

// Block coordinates within a section, in detail units.
//
// Rounds half away from zero rather than truncating, which matters because the
// offsets come in pairs about the block centre: 0.95 of a block is written as
// the +1 block edge minus 0.05, and truncating a negative offset toward zero
// would put that edge a unit further out than its mirror image is in. One unit
// is invisible; a cross whose two halves are not symmetric is not.
constexpr i16 detailPos(int block, float offsetInBlock)
{
    const float units = offsetInBlock * float(kDetailUnitsPerBlock);
    const int rounded = int(units >= 0.0f ? units + 0.5f : units - 0.5f);
    return static_cast<i16>(block * kDetailUnitsPerBlock + rounded);
}

// 0.05 and 0.95 of a block, the cross inset, to the nearest 1/1024.
static_assert(detailPos(0, 0.05f) == 51, "");
static_assert(detailPos(1, -0.05f) == 973, "");

// How one section's mesh is laid out inside the single block of vertex memory
// it occupies. Three ranges, back to back in this order, any of them empty:
//
//   cube         12-byte WorldVertex, drawn first, depth writes on
//   detail       16-byte DetailVertex, the opaque non-cube shapes
//   translucent  16-byte DetailVertex, drawn last, sorted back to front
//
// One allocation and one pool slot per section regardless, because the split is
// about *draw order*, not about ownership: the passes are what change the
// shader, the attribute layout and the blend state, and each of those should
// change a fixed number of times per frame rather than once per section.
//
// Which blocks land in the third range is not a rendering judgement of ours --
// it is a1.1.2's own `getRenderBlockPass`, in `BlockDef::translucent`. Water and
// ice are in it; lava, glass and leaves are not.
struct MeshRanges {
    usize cubeBytes = 0;
    usize detailBytes = 0;
    usize translucentBytes = 0;

    // Which encoding `cubeBytes` is in. The other two ranges have only ever had
    // one format, so they need no such field.
    CubeFormat cubeFormat = CubeFormat::Vertices;

    usize total() const { return cubeBytes + detailBytes + translucentBytes; }

    // Quads in the cube range, whichever encoding it is in. The draw loop asks
    // this rather than dividing by a stride it decided on its own -- which is
    // how a mesh built before a format switch would get drawn as garbage.
    usize cubeQuads() const { return cubeBytes / cubeBytesPerQuad(cubeFormat); }

    // Where each range starts, for a caller holding the base pointer.
    usize detailOffset() const { return cubeBytes; }
    usize translucentOffset() const { return cubeBytes + detailBytes; }
};

// A section is 16^3, and the arrangement that exposes the most faces is a
// checkerboard: half the cells solid, each showing all six faces.
//
//     4096 / 2 * 6 = 12288 quads
//
// This is what sizes the shared index buffer, so it is a hard bound rather than
// a guess -- meshSection() asserts it and the tests build the checkerboard.
inline constexpr int kMaxQuadsPerSection = 12288;
inline constexpr int kMaxVerticesPerSection = kMaxQuadsPerSection * 4;

}  // namespace mc::mesh
