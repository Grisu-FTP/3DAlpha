#include "core/mesh/shapes.hpp"

#include "core/block/collision.hpp"
#include "core/block/model.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/mesh/vertex.hpp"
#include "core/tick/redstone.hpp"
#include "core/util/math_helper.hpp"

namespace mc::mesh {

using block::BlockDef;
using block::BlockId;
using block::RenderType;

namespace {

// Everything below draws with the block's own brightness on every face and no
// per-face shade, which is what the original does everywhere outside
// `renderStandardBlock`: those methods call `setColorOpaque_F` once and never
// consult the face table. `addBox` is told `shaded = false` for the same reason.
constexpr u8 kUnshaded = 255;
constexpr bool kFlat = false;

// Sky and block light both full, for the shapes that emit light.
constexpr u8 kFullBright = (15 << 4) | 15;

// One row of the atlas. Several blocks answer with "the tile above this one"
// for a second state -- the top half of a door, a curved rail -- which is a
// consequence of terrain.png's layout rather than a rule, and is spelled out
// where it is used.
constexpr int kTileRow = kAtlasTilesPerEdge;

u8 lightAt(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def)
{
    return def.light > 0 ? kFullBright : scratch.light(x, y, z);
}

i16 pos(int block, double offsetInBlock)
{
    return detailPos(block, float(offsetInBlock));
}

// One flat quad and its mirror, so a sheet with a cutout texture is visible
// from both sides. Everything here that is not a box is built from this: the
// original draws a single quad and has no back-face culling to satisfy, and a
// single quad on the PICA disappears when seen from behind.
void addSheet(const i16 corner[4][3], const i16 uv[4][2], u8 light, MeshBuilder& out)
{
    out.addDetailQuad(corner, uv, kUnshaded, light, 0, DetailPass::Opaque);

    const i16 back[4][3] = {
        {corner[3][0], corner[3][1], corner[3][2]}, {corner[2][0], corner[2][1], corner[2][2]},
        {corner[1][0], corner[1][1], corner[1][2]}, {corner[0][0], corner[0][1], corner[0][2]},
    };
    const i16 backUv[4][2] = {{uv[3][0], uv[3][1]},
                              {uv[2][0], uv[2][1]},
                              {uv[1][0], uv[1][1]},
                              {uv[0][0], uv[0][1]}};
    out.addDetailQuad(back, backUv, kUnshaded, light, 0, DetailPass::Opaque);
}

// The four corners of a whole tile in the order a sheet wants them: top-left,
// bottom-left, bottom-right, top-right. The same order mesher.cpp's cross
// emitter documents at length -- writing them in rectangle order instead
// mirrors the texture across its own diagonal.
struct TileUv {
    i16 corner[4][2];
};

TileUv wholeTile(int texture)
{
    const int tile = texture >= 0 && texture < kAtlasTileCount ? texture : 0;
    const i16 u0 = tileUvMin(tile % kAtlasTilesPerEdge);
    const i16 u1 = tileUvMax(tile % kAtlasTilesPerEdge);
    const i16 v0 = tileUvMin(tile / kAtlasTilesPerEdge);
    const i16 v1 = tileUvMax(tile / kAtlasTilesPerEdge);
    return TileUv{{{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}}};
}

// **A point inside a tile, given in texels.** `wholeTile` maps a tile onto a
// quad and that is all most shapes need; the wire and the lever both cut
// rectangles out of one instead, because that is what the original does -- the
// wire pulls its edge in by five texels on a side it is not reaching towards,
// and the lever's handle is a two-texel-wide strip of its tile.
//
// The result is clamped into the tile's inset range, so a coordinate at texel 0
// or texel 16 comes out exactly where `tileUvMin`/`tileUvMax` would put it and
// the boundary-sampling fix in vertex.hpp is not quietly undone here. See the
// note on `kUvInset`.
i16 tileUv(int tileAxis, double texels)
{
    const double lo = double(tileAxis) * kUvUnitsPerTile + kUvInset;
    const double hi = double(tileAxis + 1) * kUvUnitsPerTile - kUvInset;
    double at = double(tileAxis) * kUvUnitsPerTile + texels * kUvUnitsPerTexel;
    at = at < lo ? lo : (at > hi ? hi : at);
    return i16(at + 0.5);
}

// The two axes of a tile index, so a caller can spell `tileUv(column(t), 7.0)`.
int tileColumn(int texture) { return texture % kAtlasTilesPerEdge; }
int tileRow(int texture) { return texture / kAtlasTilesPerEdge; }

// Six copies of one tile, for the boxes whose faces all show the same texture.
struct Tiles {
    u16 face[6];
};

Tiles uniform(u16 tile)
{
    return Tiles{{tile, tile, tile, tile, tile, tile}};
}

// ---------------------------------------------------------------------------
// The four that are already measured
// ---------------------------------------------------------------------------

// `bc.n` -- two boxes, both of which `core/block/collision.cpp` already holds.
// Stairs are the only shape in a1.1.2 with more than one collision box, and the
// two the renderer draws are the two you walk into.
void addStairs(const MeshScratch& scratch, int x, int y, int z, BlockId id, const BlockDef& def,
               u8 metadata, MeshBuilder& out)
{
    AABB boxes[block::kMaxCollisionBoxes];
    const int count = block::collisionBoxes(id, metadata, boxes, block::kMaxCollisionBoxes);
    const u8 light = lightAt(scratch, x, y, z, def);
    for (int i = 0; i < count; ++i) {
        // Every face of both boxes: the seam between them is inside the block
        // and the original draws it too, because `renderStandardBlock` is
        // called twice with no knowledge of the other half.
        addBox(x, y, z, boxes[i], def.faces, light, true, kAllBoxFaces, out);
    }
}

// `fw.a(II)I` -- **BlockDoor.getBlockTexture(face, metadata)**, whole. It
// answers a *negative* tile to mean "mirror u", and `renderBlockDoor` turns
// that into its `flipTexture` field for the one face; `mirrored` carries the
// sign so the tile can stay an index.
//
// **The mirror is how a door shows its hinge.** A door whose hinge
// `ItemDoor.onItemUse` flipped -- the second of a pair -- is written as the
// facing a quarter back with bit 2 set, which is the same box as a closed door
// of the plain facing. The two differ only in `k`'s bit 2 term below, so a
// renderer that drops the sign draws the second door of a pair hinged on the
// wrong side, and that is exactly what this used to do.
//
// The top and bottom, and the two thin edges, answer `blockIndexInTexture` in
// **both halves**: only the two broad faces of the upper half take the tile one
// row up, `(metadata & 8) * 2` being sixteen tiles back in a 16-wide atlas.
// The block table carries the lower tile, because that is what a no-metadata
// query returns. Checked against the running jar for every face and metadata
// of both doors; tests/shapes_test.cpp holds the table.
struct DoorFace {
    int tile;
    bool mirrored;
};

DoorFace doorFace(int face, u8 metadata, int texture)
{
    if (face == kFaceNegY || face == kFacePosY) {
        return DoorFace{texture, false};
    }
    // `fw.c(I)I`: which wall the door is against, open or not.
    const int state = (metadata & 4) == 0 ? (metadata - 1) & 3 : metadata & 3;
    const bool edge = (state == 0 || state == 2) != (face <= kFacePosZ);
    if (edge) {
        return DoorFace{texture, false};
    }
    int k = state / 2 + ((face & 1) ^ state);
    k += (metadata & 4) / 4;
    const int tile = texture - (metadata & 8) * 2;
    // A door tile in the atlas's top row has no row above it. a1.1.2's is
    // tile 97, so this is a guard against a table it was not written for.
    return DoorFace{tile < 0 ? texture : tile, (k & 1) != 0};
}

// `bc.o` -- one three-sixteenths box against a wall, which is the door's
// collision box exactly, with each face's tile and mirror from `doorFace`.
void addDoor(const MeshScratch& scratch, int x, int y, int z, BlockId id, const BlockDef& def,
             u8 metadata, MeshBuilder& out)
{
    AABB boxes[block::kMaxCollisionBoxes];
    const int count = block::collisionBoxes(id, metadata, boxes, block::kMaxCollisionBoxes);
    if (count == 0) {
        return;
    }
    u16 tiles[kFaceCount];
    int mirror = 0;
    for (int face = 0; face < kFaceCount; ++face) {
        const DoorFace f = doorFace(face, metadata, int(def.texture));
        tiles[face] = u16(f.tile);
        mirror |= f.mirrored ? 1 << face : 0;
    }
    addBox(x, y, z, boxes[0], tiles, lightAt(scratch, x, y, z, def), kFlat, kAllBoxFaces, out,
           DetailPass::Opaque, mirror);
}

// `bc.g` -- **one flat quad, five hundredths of a block off the wall**, and
// that is the whole of a ladder.
//
// This used to draw the collision box, which is two sixteenths thick and has
// six faces. It looked wrong in two ways at once: the back face sat *inside*
// the wall the ladder hangs on and z-fought with it, and the four thin edges
// gave the ladder a depth a1.1.2's has not got. `renderBlockLadder` emits four
// vertices and stops -- there is no box anywhere in it.
//
// The offset is `0.05f`, not a sixteenth: it is the smallest gap that keeps the
// sheet off the wall, and it is why nothing z-fights.
//
// **The only deviation is that the quad is drawn from both sides.** The
// original emits one facing quad and relies on the wall behind it being opaque;
// a ladder on glass is invisible from outside in a1.1.2. Two quads on a block
// there are never many of costs nothing measurable and does not make the
// renderer's cull state load-bearing -- the fire and rail sheets are two-sided
// for the same reason.
void addLadder(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def,
               u8 metadata, MeshBuilder& out)
{
    // Metadata outside 2..5 is not a ladder the game ever writes, and there is
    // no wall for the sheet to lie against. `collisionBoxes` answers a full
    // cube there; drawing one would be a solid block of ladder texture.
    if (metadata < 2 || metadata > 5) {
        return;
    }

    constexpr double kOff = 0.05;   // `float f1 = 0.05F`
    const TileUv uv = wholeTile(int(def.texture));
    const u8 light = lightAt(scratch, x, y, z, def);

    // The four cases in the class file's own order, each written as the four
    // `addVertexWithUV` calls it makes. `uvIndex` picks out of `TileUv::corner`,
    // which is (u0,v0), (u0,v1), (u1,v1), (u1,v0) -- so the two orders below are
    // the two ways the tile is turned to face out of its wall.
    i16 corner[4][3];
    int uvIndex[4];

    switch (metadata) {
    case 2:   // hanging on the +Z wall, sheet at z = 1 - 0.05
        corner[0][0] = pos(x, 1.0); corner[0][1] = pos(y, 1.0); corner[0][2] = pos(z, 1.0 - kOff);
        corner[1][0] = pos(x, 1.0); corner[1][1] = pos(y, 0.0); corner[1][2] = pos(z, 1.0 - kOff);
        corner[2][0] = pos(x, 0.0); corner[2][1] = pos(y, 0.0); corner[2][2] = pos(z, 1.0 - kOff);
        corner[3][0] = pos(x, 0.0); corner[3][1] = pos(y, 1.0); corner[3][2] = pos(z, 1.0 - kOff);
        uvIndex[0] = 0; uvIndex[1] = 1; uvIndex[2] = 2; uvIndex[3] = 3;
        break;
    case 3:   // -Z wall, sheet at z = 0.05
        corner[0][0] = pos(x, 1.0); corner[0][1] = pos(y, 0.0); corner[0][2] = pos(z, kOff);
        corner[1][0] = pos(x, 1.0); corner[1][1] = pos(y, 1.0); corner[1][2] = pos(z, kOff);
        corner[2][0] = pos(x, 0.0); corner[2][1] = pos(y, 1.0); corner[2][2] = pos(z, kOff);
        corner[3][0] = pos(x, 0.0); corner[3][1] = pos(y, 0.0); corner[3][2] = pos(z, kOff);
        uvIndex[0] = 2; uvIndex[1] = 3; uvIndex[2] = 0; uvIndex[3] = 1;
        break;
    case 4:   // +X wall, sheet at x = 1 - 0.05
        corner[0][0] = pos(x, 1.0 - kOff); corner[0][1] = pos(y, 0.0); corner[0][2] = pos(z, 1.0);
        corner[1][0] = pos(x, 1.0 - kOff); corner[1][1] = pos(y, 1.0); corner[1][2] = pos(z, 1.0);
        corner[2][0] = pos(x, 1.0 - kOff); corner[2][1] = pos(y, 1.0); corner[2][2] = pos(z, 0.0);
        corner[3][0] = pos(x, 1.0 - kOff); corner[3][1] = pos(y, 0.0); corner[3][2] = pos(z, 0.0);
        uvIndex[0] = 2; uvIndex[1] = 3; uvIndex[2] = 0; uvIndex[3] = 1;
        break;
    default:  // 5: -X wall, sheet at x = 0.05
        corner[0][0] = pos(x, kOff); corner[0][1] = pos(y, 1.0); corner[0][2] = pos(z, 1.0);
        corner[1][0] = pos(x, kOff); corner[1][1] = pos(y, 0.0); corner[1][2] = pos(z, 1.0);
        corner[2][0] = pos(x, kOff); corner[2][1] = pos(y, 0.0); corner[2][2] = pos(z, 0.0);
        corner[3][0] = pos(x, kOff); corner[3][1] = pos(y, 1.0); corner[3][2] = pos(z, 0.0);
        uvIndex[0] = 0; uvIndex[1] = 1; uvIndex[2] = 2; uvIndex[3] = 3;
        break;
    }

    const i16 sheetUv[4][2] = {
        {uv.corner[uvIndex[0]][0], uv.corner[uvIndex[0]][1]},
        {uv.corner[uvIndex[1]][0], uv.corner[uvIndex[1]][1]},
        {uv.corner[uvIndex[2]][0], uv.corner[uvIndex[2]][1]},
        {uv.corner[uvIndex[3]][0], uv.corner[uvIndex[3]][1]},
    };
    addSheet(corner, sheetUv, light, out);
}

// `bc.b(ly,IIIFFF)`, which `bc.l` hands to -- the top and bottom at the cell's
// full extent, and each side a **full-size face pushed in by a sixteenth**.
// That is the whole of what makes a cactus a cactus rather than a green block:
// the side sheets stand proud of nothing, so a column of them has a visible
// seam at every block.
//
// **The spikes are those sheets' outer texel columns.** The original does not
// narrow a side to the inset; it draws the standard face across the whole cell
// with `Tessellator.setTranslation(0, 0, 0.0625F)` around it, so the tile's
// edge columns -- transparent except for the spike pixels -- hang out past the
// neighbouring sides, and the alpha test cuts them out. Narrowing the side to
// the inset box, as this used to, cropped exactly those columns off and drew a
// smooth green post. Still six quads either way, so a spike costs nothing to
// draw at any distance: there is no geometry to fade out.
//
// **The boxes and their face masks are `block::renderBoxes`'s**, which is the
// same shape `bc.a(Lly;)V` gives a cactus in a slot, in the hand and on the
// ground: the two methods in the jar agree, so this port keeps one copy of
// them. This is the only render type that reads the mask.
void addCactus(const MeshScratch& scratch, int x, int y, int z, BlockId id, const BlockDef& def,
               MeshBuilder& out)
{
    AABB boxes[block::kMaxRenderBoxes];
    int faceMask[block::kMaxRenderBoxes];
    const int count = block::renderBoxes(id, 0, 0, boxes, block::kMaxRenderBoxes, faceMask);
    if (count == 0) {
        return;
    }
    const u8 light = lightAt(scratch, x, y, z, def);
    for (int box = 0; box < count; ++box) {
        addBox(x, y, z, boxes[box], def.faces, light, kFlat, faceMask[box], out);
    }
}

// ---------------------------------------------------------------------------
// The ones that carry their own constants
// ---------------------------------------------------------------------------

// `bc.m` -- a post and up to four half-rails. The boxes themselves are
// `block::renderBoxes`, shared with the inventory icon so a fence in the hand
// and a fence in the world are the same shape rather than two sets of
// constants; this decides only which sides it reaches towards.
//
// A fence reaches towards a neighbour that is another fence or an opaque cube,
// which is the test `renderBlockFence` makes.
void addFence(const MeshScratch& scratch, int x, int y, int z, BlockId id, const BlockDef& def,
              MeshBuilder& out)
{
    const int dx[4] = {-1, 1, 0, 0};
    const int dz[4] = {0, 0, -1, 1};
    const int bits[4] = {block::kConnectNegX, block::kConnectPosX, block::kConnectNegZ,
                         block::kConnectPosZ};

    int connections = 0;
    for (int side = 0; side < 4; ++side) {
        const BlockDef& other = block::def(scratch.block(x + dx[side], y, z + dz[side]));
        if (other.render == RenderType::Fence || other.opaque) {
            connections |= bits[side];
        }
    }

    AABB boxes[block::kMaxRenderBoxes];
    const int count = block::renderBoxes(id, 0, connections, boxes, block::kMaxRenderBoxes);
    const u8 light = lightAt(scratch, x, y, z, def);
    for (int i = 0; i < count; ++i) {
        addBox(x, y, z, boxes[i], def.faces, light, kFlat, kAllBoxFaces, out);
    }
}

// `bc.i` -- four vertical sheets, two across each axis a quarter of a block
// either side of the middle, and the whole thing dropped by a sixteenth. The
// 0.0625 is the only constant in the method and it is that drop.
//
// **The tile is the growth stage.** `BlockCrops.getBlockTextureFromSideAndMetadata`
// returns `blockIndexInTexture + metadata`, so the eight stages are eight
// consecutive tiles starting at the table's own texture column.
void addCrops(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def, u8 metadata,
              MeshBuilder& out)
{
    constexpr double kOffset = 0.25;
    constexpr double kDrop = 0.0625;

    const TileUv uv = wholeTile(int(def.texture) + int(metadata & 7));
    const u8 light = lightAt(scratch, x, y, z, def);

    const i16 y0 = pos(y, -kDrop);
    const i16 y1 = pos(y, 1.0 - kDrop);
    const i16 lo = pos(0, 0.0);
    const i16 hi = pos(1, 0.0);

    for (int pair = 0; pair < 2; ++pair) {
        for (int side = 0; side < 2; ++side) {
            const double at = side == 0 ? 0.5 - kOffset : 0.5 + kOffset;
            i16 corner[4][3];
            if (pair == 0) {
                // A sheet at a fixed x, spanning z.
                const i16 px = pos(x, at);
                const i16 z0 = i16(z * kDetailUnitsPerBlock + lo);
                const i16 z1 = i16(z * kDetailUnitsPerBlock + hi);
                corner[0][0] = px; corner[0][1] = y1; corner[0][2] = z0;
                corner[1][0] = px; corner[1][1] = y0; corner[1][2] = z0;
                corner[2][0] = px; corner[2][1] = y0; corner[2][2] = z1;
                corner[3][0] = px; corner[3][1] = y1; corner[3][2] = z1;
            } else {
                const i16 pz = pos(z, at);
                const i16 x0 = i16(x * kDetailUnitsPerBlock + lo);
                const i16 x1 = i16(x * kDetailUnitsPerBlock + hi);
                corner[0][0] = x0; corner[0][1] = y1; corner[0][2] = pz;
                corner[1][0] = x0; corner[1][1] = y0; corner[1][2] = pz;
                corner[2][0] = x1; corner[2][1] = y0; corner[2][2] = pz;
                corner[3][0] = x1; corner[3][1] = y1; corner[3][2] = pz;
            }
            addSheet(corner, uv.corner, light, out);
        }
    }
}

// `bc.f` -- one sheet a sixteenth off the floor, **turned a quarter, a half or
// three quarters** for the other nine shapes, and tilted for the four that
// ascend.
//
// A curve shows the tile one row up, the same relationship the door's two
// halves have: `BlockRail.getBlockTextureFromSideAndMetadata` answers with
// `blockIndexInTexture - 16` for metadata 6 and above, and terrain.png puts the
// curved rail directly above the straight one.
//
// **The turn is in the geometry, not in the UVs.** The class file keeps one
// fixed set of texture corners and permutes which cell corner each one lands
// on, which is what makes an east-west rail read as sleepers across the track
// rather than along it, and what points each of the four curves the right way.
// Without it every rail in the world was drawn as the north-south one. The four
// permutations, in the class file's own grouping:
//
// | metadata      | corner 0 | corner 1 | corner 2 | corner 3 |
// |---------------|----------|----------|----------|----------|
// | 0, 4, 5, 6    | +X, -Z   | +X, +Z   | -X, +Z   | -X, -Z   |
// | 1, 2, 3, 7    | +X, +Z   | -X, +Z   | -X, -Z   | +X, -Z   |
// | 8             | -X, +Z   | -X, -Z   | +X, -Z   | +X, +Z   |
// | 9             | -X, -Z   | +X, -Z   | +X, +Z   | -X, +Z   |
//
// **And the slope is applied to corners, not to axes**, which is how the two
// ascending pairs come out the right way round: 2 and 4 raise corners 0 and 3,
// 3 and 5 raise corners 1 and 2. Read through the table above that makes
// **2 ascend towards +X and 3 towards -X** -- this file used to have those two
// the other way round, so every east-west rail staircase in a world climbed
// backwards and met its neighbour in mid air.
void addRail(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def, u8 metadata,
             MeshBuilder& out)
{
    constexpr double kHeight = 0.0625;

    const bool curved = metadata >= 6;
    const int tile = curved ? int(def.texture) - kTileRow : int(def.texture);
    const TileUv uv = wholeTile(tile);
    const u8 light = lightAt(scratch, x, y, z, def);

    // Which cell corner each of the quad's four vertices sits on, as (x, z) in
    // block-local units.
    static constexpr double kTurns[4][4][2] = {
        {{1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}},   // 0, 4, 5, 6
        {{1.0, 1.0}, {0.0, 1.0}, {0.0, 0.0}, {1.0, 0.0}},   // 1, 2, 3, 7
        {{0.0, 1.0}, {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}},   // 8
        {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}},   // 9
    };
    int turn = 0;
    if (metadata == 1 || metadata == 2 || metadata == 3 || metadata == 7) {
        turn = 1;
    } else if (metadata == 8) {
        turn = 2;
    } else if (metadata == 9) {
        turn = 3;
    }

    double height[4] = {kHeight, kHeight, kHeight, kHeight};
    if (metadata == 2 || metadata == 4) {
        height[0] = 1.0;
        height[3] = 1.0;
    } else if (metadata == 3 || metadata == 5) {
        height[1] = 1.0;
        height[2] = 1.0;
    }

    i16 corner[4][3];
    for (int i = 0; i < 4; ++i) {
        corner[i][0] = pos(x, kTurns[turn][i][0]);
        corner[i][1] = pos(y, height[i]);
        corner[i][2] = pos(z, kTurns[turn][i][1]);
    }

    // The tile's own corners, in the order `renderBlockRail` writes them:
    // (uMax, vMin), (uMax, vMax), (uMin, vMax), (uMin, vMin). `TileUv::corner`
    // holds them as (u0,v0), (u0,v1), (u1,v1), (u1,v0), so this is 3, 2, 1, 0.
    const i16 railUv[4][2] = {
        {uv.corner[3][0], uv.corner[3][1]},
        {uv.corner[2][0], uv.corner[2][1]},
        {uv.corner[1][0], uv.corner[1][1]},
        {uv.corner[0][0], uv.corner[0][1]},
    };
    addSheet(corner, railUv, light, out);
}

// `bc.e` -- the wire lying on the floor, and the sheets it climbs a neighbour
// with.
//
// **This used to draw the crossing tile always, unconnected, unlit.** Three
// separate things were missing and all three are in the class file:
//
//   * **Which neighbours it reaches towards.** `kf.b(Lnm;III)Z` --
//     isPowerProviderOrWire -- is asked of the four horizontal neighbours, one
//     block down where the neighbour is not a solid to climb and one block up
//     where it is. A wire with a run along one axis and nothing on the other
//     draws the *line* tile, which is the tile one column along; a wire that
//     reaches some but not all four ways draws the crossing tile with the
//     unused arms cut off, quad and UV together, five texels a side.
//   * **The glow**, and it is a texture and not a tint.
//     `kf.a(II)I` is `blockIndexInTexture + (metadata > 0 ? 16 : 0)`, so a
//     powered wire is drawn from the row below in terrain.png. The colour the
//     original passes is `setColorOpaque_F(f, f, f)` -- brightness on all three
//     channels and nothing else -- so there is no tint to reproduce here and
//     the earlier note claiming otherwise was describing a later version.
//   * **The climb.** Where a neighbour is a solid cube with wire on top of it
//     and this cell has nothing on top of *it*, the wire is drawn up the
//     neighbour's near face as a vertical sheet of the line tile.
//
// The height off the floor is the original's 1/32, not the 1/64 that was here.
void addRedstoneWire(const MeshScratch& scratch, int x, int y, int z, BlockId id,
                     const BlockDef& def, u8 metadata, MeshBuilder& out)
{
    // f19 and f24 in `bc.e`: how far off the floor the sheet sits, and how far
    // an arm that leads nowhere is pulled back.
    constexpr double kHeight = 0.03125;
    constexpr double kTrim = 0.3125;
    // The same trim expressed in texels, which is what the UV edge moves by.
    constexpr double kTrimTexels = kTrim * 16.0;

    const auto attaches = [&scratch](int ax, int ay, int az) {
        return tick::canProvidePower(scratch.block(ax, ay, az));
    };
    const auto opaque = [&scratch](int ax, int ay, int az) {
        return block::def(scratch.block(ax, ay, az)).opaque;
    };

    // The four horizontal connections, exactly as the original works them out:
    // the neighbour itself, or -- when the neighbour is not something to climb
    // -- the wire running under it.
    bool west = attaches(x - 1, y, z) || (!opaque(x - 1, y, z) && attaches(x - 1, y - 1, z));
    bool east = attaches(x + 1, y, z) || (!opaque(x + 1, y, z) && attaches(x + 1, y - 1, z));
    bool north = attaches(x, y, z - 1) || (!opaque(x, y, z - 1) && attaches(x, y - 1, z - 1));
    bool south = attaches(x, y, z + 1) || (!opaque(x, y, z + 1) && attaches(x, y - 1, z + 1));

    // And a wire climbing a solid neighbour counts too, but only while nothing
    // is sitting on this cell to stop it.
    const bool openAbove = !opaque(x, y + 1, z);
    if (openAbove) {
        west = west || (opaque(x - 1, y, z) && attaches(x - 1, y + 1, z));
        east = east || (opaque(x + 1, y, z) && attaches(x + 1, y + 1, z));
        north = north || (opaque(x, y, z - 1) && attaches(x, y + 1, z - 1));
        south = south || (opaque(x, y, z + 1) && attaches(x, y + 1, z + 1));
    }

    // `blockIndexInTexture + 16` when live: one row down in terrain.png, which
    // is the lit dust. The line tile is the next column along from whichever of
    // the two the wire is drawn from.
    const int base = int(def.texture) + (metadata > 0 ? kTileRow : 0);
    const int cross = base >= 0 && base < kAtlasTileCount ? base : int(def.texture);
    const int line = cross + 1 < kAtlasTileCount ? cross + 1 : cross;

    // 0 is the crossing, 1 a run along x, 2 a run along z. A wire that reaches
    // in one direction only is still a crossing with three arms cut off, which
    // is why this is not "how many connections".
    int mode = 0;
    if ((west || east) && !north && !south) {
        mode = 1;
    }
    if ((north || south) && !west && !east) {
        mode = 2;
    }

    const int tile = mode == 0 ? cross : line;
    const int col = tileColumn(tile);
    const int row = tileRow(tile);

    double x0 = 0.0;
    double x1 = 1.0;
    double z0 = 0.0;
    double z1 = 1.0;
    double u0 = 0.0;
    double u1 = 16.0;
    double v0 = 0.0;
    double v1 = 16.0;

    // **Only a crossing is trimmed, and only when it connects to something.**
    // A wire touching nothing is drawn as the whole crossing tile -- which is
    // the dot you get from a single dust on the ground.
    if (mode == 0 && (west || east || north || south)) {
        if (!west) {
            x0 += kTrim;
            u0 += kTrimTexels;
        }
        if (!east) {
            x1 -= kTrim;
            u1 -= kTrimTexels;
        }
        if (!north) {
            z0 += kTrim;
            v0 += kTrimTexels;
        }
        if (!south) {
            z1 -= kTrim;
            v1 -= kTrimTexels;
        }
    }

    const u8 light = lightAt(scratch, x, y, z, def);
    const i16 h = pos(y, kHeight);
    const i16 px0 = pos(x, x0);
    const i16 px1 = pos(x, x1);
    const i16 pz0 = pos(z, z0);
    const i16 pz1 = pos(z, z1);
    const i16 pu0 = tileUv(col, u0);
    const i16 pu1 = tileUv(col, u1);
    const i16 pv0 = tileUv(row, v0);
    const i16 pv1 = tileUv(row, v1);

    const i16 flat[4][3] = {{px1, h, pz1}, {px1, h, pz0}, {px0, h, pz0}, {px0, h, pz1}};
    // **A run along z turns the tile a quarter turn**, and that is the only
    // difference between the two straight cases in the class file: the line
    // texture runs along x as it sits in the atlas, so the crossing and the
    // x-run share a mapping and the z-run does not.
    const i16 alongX[4][2] = {{pu1, pv1}, {pu1, pv0}, {pu0, pv0}, {pu0, pv1}};
    const i16 alongZ[4][2] = {{pu1, pv1}, {pu0, pv1}, {pu0, pv0}, {pu1, pv0}};
    addSheet(flat, mode == 2 ? alongZ : alongX, light, out);

    // ---- the climb ----------------------------------------------------
    //
    // A vertical sheet a thirty-second off the neighbour's face, drawn from the
    // line tile, wherever the neighbour is a cube with wire standing on it.
    //
    // **`== redstoneWire`, not `isPowerProviderOrWire`.** The flat sheet's four
    // connections take anything that hands out power; the climb takes only more
    // of this same block, which is the class file's own distinction and is why
    // the id is passed in rather than a second behaviour test being written.
    if (!openAbove) {
        return;
    }
    const auto climbs = [&scratch, id](int ax, int ay, int az) {
        return BlockId(scratch.block(ax, ay, az)) == id;
    };

    const int lineCol = tileColumn(line);
    const int lineRow = tileRow(line);
    const i16 lu0 = tileUv(lineCol, 0.0);
    const i16 lu1 = tileUv(lineCol, 16.0);
    const i16 lv0 = tileUv(lineRow, 0.0);
    const i16 lv1 = tileUv(lineRow, 16.0);

    const i16 wy0 = pos(y, 0.0);
    const i16 wy1 = pos(y, 1.0);
    const i16 wx0 = pos(x, 0.0);
    const i16 wx1 = pos(x, 1.0);
    const i16 wz0 = pos(z, 0.0);
    const i16 wz1 = pos(z, 1.0);

    if (opaque(x - 1, y, z) && climbs(x - 1, y + 1, z)) {
        const i16 at = pos(x, kHeight);
        const i16 corner[4][3] = {{at, wy1, wz1}, {at, wy0, wz1}, {at, wy0, wz0}, {at, wy1, wz0}};
        const i16 uv[4][2] = {{lu1, lv0}, {lu0, lv0}, {lu0, lv1}, {lu1, lv1}};
        addSheet(corner, uv, light, out);
    }
    if (opaque(x + 1, y, z) && climbs(x + 1, y + 1, z)) {
        const i16 at = pos(x, 1.0 - kHeight);
        const i16 corner[4][3] = {{at, wy0, wz1}, {at, wy1, wz1}, {at, wy1, wz0}, {at, wy0, wz0}};
        const i16 uv[4][2] = {{lu0, lv1}, {lu1, lv1}, {lu1, lv0}, {lu0, lv0}};
        addSheet(corner, uv, light, out);
    }
    if (opaque(x, y, z - 1) && climbs(x, y + 1, z - 1)) {
        const i16 at = pos(z, kHeight);
        const i16 corner[4][3] = {{wx1, wy0, at}, {wx1, wy1, at}, {wx0, wy1, at}, {wx0, wy0, at}};
        const i16 uv[4][2] = {{lu0, lv1}, {lu1, lv1}, {lu1, lv0}, {lu0, lv0}};
        addSheet(corner, uv, light, out);
    }
    if (opaque(x, y, z + 1) && climbs(x, y + 1, z + 1)) {
        const i16 at = pos(z, 1.0 - kHeight);
        const i16 corner[4][3] = {{wx1, wy1, at}, {wx1, wy0, at}, {wx0, wy0, at}, {wx0, wy1, at}};
        const i16 uv[4][2] = {{lu1, lv0}, {lu0, lv0}, {lu0, lv1}, {lu1, lv1}};
        addSheet(corner, uv, light, out);
    }
}

// `bc.c` -- a cobblestone base plate with a handle standing out of it, and the
// handle is **swung** rather than upright.
//
// This was the file's first honest simplification and it no longer needs to be
// one. The original builds the handle out of eight corners it rotates -- 40
// degrees about x one way or the other depending on whether the lever is
// thrown, then a quarter turn for the second floor orientation, then a right
// angle and a quarter turn for each of the four walls -- and every one of those
// is expressible here, because `addDetailQuad` takes arbitrary corners. What it
// is not expressible through is `addBox`, which is why the upright stub was
// there.
//
// **The UVs are the reason the top looked broken.** `addBox` derives a face's
// UV from the box's own bounds, which for a handle 1/8 of a block across gave
// it a two-texel strip taken from wherever the box happened to sit in its cell.
// The original takes a fixed patch: texels 7..9 across and 6..16 down for the
// four sides, and 7..9 by 6..8 for the two ends. Those are the numbers below.
void addLever(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def, u8 metadata,
              MeshBuilder& out)
{
    // f, f1 and f2 in the class file: the plate is half a block across the wall
    // it sits on, three eighths the other way, and three sixteenths deep.
    constexpr double kWide = 0.25;
    constexpr double kNarrow = 0.1875;
    constexpr double kDepth = 0.1875;

    const int orientation = int(metadata) & 7;
    const bool thrown = (metadata & 8) != 0;
    const u8 light = lightAt(scratch, x, y, z, def);

    // The plate, and these six are `setBounds` calls transcribed one for one.
    AABB plate{0.5 - kNarrow, 0.0, 0.5 - kWide, 0.5 + kNarrow, kDepth, 0.5 + kWide};
    switch (orientation) {
    case 1:  // hanging on the -x wall
        plate = AABB{0.0, 0.5 - kWide, 0.5 - kNarrow, kDepth, 0.5 + kWide, 0.5 + kNarrow};
        break;
    case 2:  // +x
        plate = AABB{1.0 - kDepth, 0.5 - kWide, 0.5 - kNarrow, 1.0, 0.5 + kWide, 0.5 + kNarrow};
        break;
    case 3:  // -z
        plate = AABB{0.5 - kNarrow, 0.5 - kWide, 0.0, 0.5 + kNarrow, 0.5 + kWide, kDepth};
        break;
    case 4:  // +z
        plate = AABB{0.5 - kNarrow, 0.5 - kWide, 1.0 - kDepth, 0.5 + kNarrow, 0.5 + kWide, 1.0};
        break;
    case 6:  // on the floor, lying the other way round
        plate = AABB{0.5 - kWide, 0.0, 0.5 - kNarrow, 0.5 + kWide, kDepth, 0.5 + kNarrow};
        break;
    default:  // 5, and anything the game never writes
        break;
    }

    // **Cobblestone for the plate.** `renderBlockLever` sets
    // `overrideBlockTexture` to the cobblestone tile for the plate and puts it
    // back afterwards, so the plate borrows the block it is modelled on rather
    // than the lever carrying a second texture column.
    const Tiles base = uniform(mcver::kBlocks[int(mcver::Block::Cobblestone)].texture);
    addBox(x, y, z, plate, base.face, light, kFlat, kAllBoxFaces, out);

    // ---- the handle ---------------------------------------------------
    //
    // Eight corners of a 1/8 x 5/8 x 1/8 stick standing at the origin, then the
    // original's transform chain applied to each. Written out rather than
    // folded into a matrix because that is how the class file reads and because
    // there is no matrix type on this side of the mesher.
    constexpr double kHalf = 0.0625;
    constexpr double kLength = 0.625;
    constexpr float kSwing = 0.69813174f;  // 40 degrees, and the class file's own float
    constexpr float kQuarter = 1.5707964f;
    constexpr float kHalfTurn = 3.1415927f;

    double cx[8] = {-kHalf, kHalf, kHalf, -kHalf, -kHalf, kHalf, kHalf, -kHalf};
    double cy[8] = {0.0, 0.0, 0.0, 0.0, kLength, kLength, kLength, kLength};
    double cz[8] = {-kHalf, -kHalf, kHalf, kHalf, -kHalf, -kHalf, kHalf, kHalf};

    // `aj.a(F)V` and `aj.b(F)V` -- Vec3D.rotateAroundX and rotateAroundY. The
    // sine comes from `MathHelper`'s table rather than from `<cmath>` for the
    // same reason it does everywhere else in this port: the original's table is
    // what the original's geometry was built from, and the two disagree in the
    // fourth decimal place.
    const auto rotateX = [](double* py, double* pz, float angle) {
        const double c = double(MathHelper::cos(angle));
        const double s = double(MathHelper::sin(angle));
        const double ny = *py * c + *pz * s;
        const double nz = *pz * c - *py * s;
        *py = ny;
        *pz = nz;
    };
    const auto rotateY = [](double* px, double* pz, float angle) {
        const double c = double(MathHelper::cos(angle));
        const double s = double(MathHelper::sin(angle));
        const double nx = *px * c + *pz * s;
        const double nz = *pz * c - *px * s;
        *px = nx;
        *pz = nz;
    };

    for (int i = 0; i < 8; ++i) {
        // **The throw**: the handle leans one way or the other, and it leans
        // about a pivot a sixteenth off centre so the two positions meet at the
        // plate rather than crossing through it.
        cz[i] += thrown ? -kHalf : kHalf;
        rotateX(&cy[i], &cz[i], thrown ? kSwing : -kSwing);

        if (orientation == 6) {
            rotateY(&cx[i], &cz[i], kQuarter);
        }

        if (orientation < 5) {
            // A wall lever is the floor one tipped on its side, dropped by
            // three eighths first so it ends up centred on the wall.
            cy[i] -= 0.375;
            rotateX(&cy[i], &cz[i], kQuarter);
            if (orientation == 3) {
                rotateY(&cx[i], &cz[i], kHalfTurn);
            } else if (orientation == 2) {
                rotateY(&cx[i], &cz[i], kQuarter);
            } else if (orientation == 1) {
                rotateY(&cx[i], &cz[i], -kQuarter);
            }
            cx[i] += 0.5;
            cy[i] += 0.5;
            cz[i] += 0.5;
        } else {
            cx[i] += 0.5;
            cy[i] += 0.125;
            cz[i] += 0.5;
        }
    }

    // The six quads, by corner index, in the class file's own order: bottom
    // cap, top cap, then the four sides.
    constexpr int kQuad[6][4] = {
        {0, 1, 2, 3}, {7, 6, 5, 4}, {1, 0, 4, 5}, {2, 1, 5, 6}, {3, 2, 6, 7}, {0, 3, 7, 4},
    };

    const int col = tileColumn(int(def.texture));
    const int row = tileRow(int(def.texture));
    // Texels 7..9 across for everything; 6..8 down for the two caps and 6..16
    // for the four sides. The 0.01 the original subtracts from the far edge is
    // its way of staying inside the tile and is what `tileUv`'s clamp does here.
    const i16 hu0 = tileUv(col, 7.0);
    const i16 hu1 = tileUv(col, 9.0);
    const i16 hv0 = tileUv(row, 6.0);
    const i16 capV1 = tileUv(row, 8.0);
    const i16 sideV1 = tileUv(row, 16.0);

    for (int q = 0; q < 6; ++q) {
        const i16 v1 = q < 2 ? capV1 : sideV1;
        const i16 uv[4][2] = {{hu0, v1}, {hu1, v1}, {hu1, hv0}, {hu0, hv0}};
        i16 corner[4][3];
        for (int c = 0; c < 4; ++c) {
            const int idx = kQuad[q][c];
            corner[c][0] = pos(x, cx[idx]);
            corner[c][1] = pos(y, cy[idx]);
            corner[c][2] = pos(z, cz[idx]);
        }
        addSheet(corner, uv, light, out);
    }
}

// `bc.d` -- fire.
//
// **Two shapes, and the second was missing.** The one everybody pictures is
// fire on the ground: four sheets leaning in over the block below, 1.4 blocks
// tall, drawn twice each from opposite sides. The other is fire *on a wall* --
// a flame that has caught the side of a burning block with nothing under it to
// stand on -- and it is a different set of quads entirely, pinned to whichever
// of the five faces around the cell can burn. This used to draw the first
// shape's rough outline in both cases, so a fire climbing the side of a house
// hung in the air in the middle of its cell instead of clinging to the wall.
//
// The branch is `isBlockNormalCube(i, j - 1, k) || canBlockCatchFire(i, j - 1,
// k)`: something solid or something burnable directly underneath means floor
// fire, and anything else means wall fire. **Not "is there air below"** -- fire
// on a solid but unburnable floor still draws the floor shape, which is what
// makes a fire lit on cobble look the same as one on planks.
//
// **`canBlockCatchFire` is `chanceToEncourageFire[id] > 0`**, which is the
// `burnEncourage` column -- the same table core/tick/fire.cpp spreads by. So
// the shape a fire draws and the blocks it will spread to are one fact, and a
// wall fire is drawn against exactly the faces it is eating.
//
// **Two tiles, alternating.** `Block.fire.blockIndexInTexture` and the tile one
// atlas row below it; the original registers a `TextureFlamesFX` for each and
// draws different sheets of one fire from each. Which tile a wall flame starts
// on comes from `(i + j + k) & 1`, and whether its texture is mirrored from
// `(i/2 + j/2 + k/2) & 1` -- two cheap parities whose whole job is to stop a
// wall of fire from looking like wallpaper. Java's integer division truncates
// toward zero and so does C++'s, so the negative-coordinate case agrees without
// help.
//
// **Every sheet goes through `addSheet`**, which draws it and its mirror. The
// original writes each of its floor sheets once and relies on there being eight
// of them at four leans to cover both directions; this renderer culls back
// faces, so the pairing is done here instead. It costs quads on a block there
// are never many of.

// Where a fire quad's four UV corners come from. The original writes them in
// one of two orders and nothing else: `u1,u1,u0,u0` down the quad, or the same
// mirrored. `Right` is the first, `Left` the second.
struct FireTile {
    i16 u0, u1, v0, v1;
};

enum class FireUv { Right, Left };

FireTile fireTile(int texture)
{
    const int tile = texture >= 0 && texture < kAtlasTileCount ? texture : 0;
    return FireTile{tileUvMin(tileColumn(tile)), tileUvMax(tileColumn(tile)),
                    tileUvMin(tileRow(tile)), tileUvMax(tileRow(tile))};
}

// One fire sheet: four corners in the original's own order, and the UV pattern
// it wrote beside them.
void addFireSheet(const i16 corner[4][3], const FireTile& t, FireUv order, u8 light,
                  MeshBuilder& out)
{
    if (order == FireUv::Right) {
        const i16 uv[4][2] = {{t.u1, t.v0}, {t.u1, t.v1}, {t.u0, t.v1}, {t.u0, t.v0}};
        addSheet(corner, uv, light, out);
        return;
    }
    const i16 uv[4][2] = {{t.u0, t.v0}, {t.u0, t.v1}, {t.u1, t.v1}, {t.u1, t.v0}};
    addSheet(corner, uv, light, out);
}

// `og.b(nm,III)Z` -- BlockFire.canBlockCatchFire.
bool fireCanCatch(const MeshScratch& scratch, int x, int y, int z)
{
    return block::def(scratch.block(x, y, z)).burnEncourage > 0;
}

void addFire(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def,
             MeshBuilder& out)
{
    const u8 light = lightAt(scratch, x, y, z, def);

    const FireTile tile0 = fireTile(int(def.texture));
    const FireTile tile1 = fireTile(int(def.texture) + kTileRow);

    // `float f1 = 1.4F`. A flame is taller than its block, which is most of
    // why fire reads as fire and not as a coloured cube.
    constexpr double kTall = 1.4;

    const i16 z0 = pos(z, 0.0);
    const i16 z1 = pos(z, 1.0);
    const i16 x0 = pos(x, 0.0);
    const i16 x1 = pos(x, 1.0);

    if (block::def(scratch.block(x, y - 1, z)).opaque || fireCanCatch(scratch, x, y - 1, z)) {
        // ---- fire on the ground -------------------------------------------
        //
        // Eight sheets in two sets of four. The first set leans in from
        // +-0.2 at the top to +-0.3 at the bottom; the second from +-0.4 to
        // the cell wall. The pairs face opposite ways, which is what gives a
        // single fire block its depth.
        const i16 top = pos(y, kTall);
        const i16 bottom = pos(y, 0.0);

        const i16 a4 = pos(x, 0.7);   // i + 0.5 + 0.2
        const i16 a5 = pos(x, 0.3);   // i + 0.5 - 0.2
        const i16 a6 = pos(z, 0.7);
        const i16 a7 = pos(z, 0.3);
        const i16 a8 = pos(x, 0.2);   // i + 0.5 - 0.3
        const i16 a9 = pos(x, 0.8);
        const i16 a10 = pos(z, 0.2);
        const i16 a11 = pos(z, 0.8);

        const i16 q1[4][3] = {{a8, top, z1}, {a4, bottom, z1}, {a4, bottom, z0}, {a8, top, z0}};
        addFireSheet(q1, tile0, FireUv::Right, light, out);
        const i16 q2[4][3] = {{a9, top, z0}, {a5, bottom, z0}, {a5, bottom, z1}, {a9, top, z1}};
        addFireSheet(q2, tile0, FireUv::Right, light, out);

        const i16 q3[4][3] = {{x1, top, a11}, {x1, bottom, a7}, {x0, bottom, a7}, {x0, top, a11}};
        addFireSheet(q3, tile1, FireUv::Right, light, out);
        const i16 q4[4][3] = {{x0, top, a10}, {x0, bottom, a6}, {x1, bottom, a6}, {x1, top, a10}};
        addFireSheet(q4, tile1, FireUv::Right, light, out);

        // The second set: the cell wall at the bottom, +-0.4 at the top.
        const i16 b4 = pos(x, 0.0);
        const i16 b5 = pos(x, 1.0);
        const i16 b6 = pos(z, 0.0);
        const i16 b7 = pos(z, 1.0);
        const i16 b8 = pos(x, 0.1);   // i + 0.5 - 0.4
        const i16 b9 = pos(x, 0.9);
        const i16 b10 = pos(z, 0.1);
        const i16 b11 = pos(z, 0.9);

        const i16 q5[4][3] = {{b8, top, z0}, {b4, bottom, z0}, {b4, bottom, z1}, {b8, top, z1}};
        addFireSheet(q5, tile1, FireUv::Left, light, out);
        const i16 q6[4][3] = {{b9, top, z1}, {b5, bottom, z1}, {b5, bottom, z0}, {b9, top, z0}};
        addFireSheet(q6, tile1, FireUv::Left, light, out);

        const i16 q7[4][3] = {{x0, top, b11}, {x0, bottom, b7}, {x1, bottom, b7}, {x1, top, b11}};
        addFireSheet(q7, tile0, FireUv::Left, light, out);
        const i16 q8[4][3] = {{x1, top, b10}, {x1, bottom, b6}, {x0, bottom, b6}, {x0, top, b10}};
        addFireSheet(q8, tile0, FireUv::Left, light, out);
        return;
    }

    // ---- fire on a wall ---------------------------------------------------
    //
    // `float f2 = 0.2F, f3 = 0.0625F`: how far the top of a wall flame leans
    // out from its wall, and how far the whole sheet is lifted off the floor.
    // The lift is what stops a flame hanging on a wall from z-fighting with
    // the block below it.
    constexpr double kLean = 0.2;
    constexpr double kLift = 0.0625;

    // The two parities. Neither changes the geometry -- only which of the two
    // flame tiles a face starts on and whether its texture is mirrored -- and
    // between them they give a burning wall four distinct-looking cells before
    // it repeats.
    //
    // **These are section-local coordinates and the original's are world ones,
    // and the two agree.** A section origin is a multiple of sixteen on every
    // axis, so it is even and `origin + local` has the parity of `local`; and
    // `(origin + local) / 2` is `8 * n + local / 2`, which is even plus
    // `local / 2`. Both hold for negative chunks too, because sixteen times a
    // negative integer is still even. If they did not, the pattern would jump
    // at every chunk boundary -- visible on a long burning wall and on nothing
    // else, which is the sort of thing that survives for a year.
    FireTile wall = ((x + y + z) & 1) == 1 ? tile1 : tile0;
    if (((x / 2 + y / 2 + z / 2) & 1) == 1) {
        const i16 swap = wall.u0;
        wall.u0 = wall.u1;
        wall.u1 = swap;
    }

    const i16 top = pos(y, kTall + kLift);
    const i16 bottom = pos(y, kLift);

    if (fireCanCatch(scratch, x - 1, y, z)) {
        const i16 lean = pos(x, kLean);
        const i16 q[4][3] = {{lean, top, z1}, {x0, bottom, z1}, {x0, bottom, z0}, {lean, top, z0}};
        addFireSheet(q, wall, FireUv::Right, light, out);
    }
    if (fireCanCatch(scratch, x + 1, y, z)) {
        const i16 lean = pos(x, 1.0 - kLean);
        const i16 q[4][3] = {{lean, top, z0}, {x1, bottom, z0}, {x1, bottom, z1}, {lean, top, z1}};
        addFireSheet(q, wall, FireUv::Left, light, out);
    }
    if (fireCanCatch(scratch, x, y, z - 1)) {
        const i16 lean = pos(z, kLean);
        const i16 q[4][3] = {{x0, top, lean}, {x0, bottom, z0}, {x1, bottom, z0}, {x1, top, lean}};
        addFireSheet(q, wall, FireUv::Right, light, out);
    }
    if (fireCanCatch(scratch, x, y, z + 1)) {
        const i16 lean = pos(z, 1.0 - kLean);
        const i16 q[4][3] = {{x1, top, lean}, {x1, bottom, z1}, {x0, bottom, z1}, {x0, top, lean}};
        addFireSheet(q, wall, FireUv::Left, light, out);
    }

    // **Fire on a ceiling**, which is the case nobody remembers exists. Two
    // sheets hanging from the block above, leaning *down* by 0.2 -- `f1` is
    // reassigned to -0.2F here, and `j` is incremented, so every coordinate
    // below is relative to the top of the cell.
    //
    // The parity is re-read **after** the increment, and the UVs are reset to
    // the unmirrored first tile, so the wall's two variations do not carry
    // into it. Both are the class file's.
    if (!fireCanCatch(scratch, x, y + 1, z)) {
        return;
    }

    const i16 ceiling = pos(y + 1, 0.0);
    const i16 hang = pos(y + 1, -0.2);

    if (((x + (y + 1) + z) & 1) == 0) {
        const i16 q1[4][3] = {{x0, hang, z0}, {x1, ceiling, z0}, {x1, ceiling, z1}, {x0, hang, z1}};
        addFireSheet(q1, tile0, FireUv::Right, light, out);
        const i16 q2[4][3] = {{x1, hang, z1}, {x0, ceiling, z1}, {x0, ceiling, z0}, {x1, hang, z0}};
        addFireSheet(q2, tile1, FireUv::Right, light, out);
        return;
    }
    const i16 q3[4][3] = {{x0, hang, z1}, {x0, ceiling, z0}, {x1, ceiling, z0}, {x1, hang, z1}};
    addFireSheet(q3, tile0, FireUv::Right, light, out);
    const i16 q4[4][3] = {{x1, hang, z0}, {x1, ceiling, z1}, {x0, ceiling, z1}, {x0, hang, z0}};
    addFireSheet(q4, tile1, FireUv::Right, light, out);
}

}  // namespace

bool shapeHasEmitter(RenderType type)
{
    switch (type) {
        case RenderType::Stairs:
        case RenderType::Door:
        case RenderType::Ladder:
        case RenderType::Cactus:
        case RenderType::Fence:
        case RenderType::Crops:
        case RenderType::Rail:
        case RenderType::RedstoneWire:
        case RenderType::Lever:
        case RenderType::Fire:
            return true;
        default:
            return false;
    }
}

bool addShape(const MeshScratch& scratch, int x, int y, int z, BlockId id, u8 metadata,
              MeshBuilder& out)
{
    const BlockDef& def = block::def(id);
    switch (def.render) {
        case RenderType::Stairs:
            addStairs(scratch, x, y, z, id, def, metadata, out);
            return true;
        case RenderType::Door:
            addDoor(scratch, x, y, z, id, def, metadata, out);
            return true;
        case RenderType::Ladder:
            addLadder(scratch, x, y, z, def, metadata, out);
            return true;
        case RenderType::Cactus:
            addCactus(scratch, x, y, z, id, def, out);
            return true;
        case RenderType::Fence:
            addFence(scratch, x, y, z, id, def, out);
            return true;
        case RenderType::Crops:
            addCrops(scratch, x, y, z, def, metadata, out);
            return true;
        case RenderType::Rail:
            addRail(scratch, x, y, z, def, metadata, out);
            return true;
        case RenderType::RedstoneWire:
            addRedstoneWire(scratch, x, y, z, id, def, metadata, out);
            return true;
        case RenderType::Lever:
            addLever(scratch, x, y, z, def, metadata, out);
            return true;
        case RenderType::Fire:
            addFire(scratch, x, y, z, def, out);
            return true;
        default:
            return false;
    }
}

}  // namespace mc::mesh
