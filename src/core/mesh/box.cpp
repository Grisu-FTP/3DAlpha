#include "core/mesh/box.hpp"

namespace mc::mesh {

namespace {

// Which of the box's six bounds each face's u and v run between. Read straight
// off `bc.a` through `bc.f` -- the two pairs of bound fields each method loads
// are the whole answer.
//
// **Only the axes, and no flip.** Which *end* of the range each corner takes is
// already `kFaceCornerUV`, whose pairing with `kFaceCorner` is what puts u
// backwards on -Z and +X and reads v downward on all four sides. That pairing
// is the one the cube path has been drawing with on hardware since M0, so
// re-deriving a flip here would be a second answer to a settled question and a
// chance to get it wrong. A full cube through this table and a full cube
// through `addQuad` come out identical, which a test asserts.
enum Axis : u8 { AxisX = 0, AxisY = 1, AxisZ = 2 };

struct FaceUv {
    Axis u;
    Axis v;
};

// Note the sides take v from **y**, low end first, so the box's *top* edge
// samples tile row `minY * 16`. That is what makes a slab show the top half of
// its tile rather than the bottom. See the header.
constexpr FaceUv kFaceUv[kFaceCount] = {
    {AxisX, AxisZ},  // kFaceNegY -- bc.a
    {AxisX, AxisZ},  // kFacePosY -- bc.b
    {AxisX, AxisY},  // kFaceNegZ -- bc.c
    {AxisX, AxisY},  // kFacePosZ -- bc.d
    {AxisZ, AxisY},  // kFaceNegX -- bc.e
    {AxisZ, AxisY},  // kFacePosX -- bc.f
};

double boundLow(const AABB& b, Axis axis)
{
    return axis == AxisX ? b.minX : (axis == AxisY ? b.minY : b.minZ);
}

double boundHigh(const AABB& b, Axis axis)
{
    return axis == AxisX ? b.maxX : (axis == AxisY ? b.maxY : b.maxZ);
}

// A tile-local offset in blocks (0..1) to an absolute atlas coordinate.
i16 uvAt(int tileAxis, double offsetInBlock)
{
    const double units = double(tileAxis) * double(kUvUnitsPerTile)
                         + offsetInBlock * double(kUvUnitsPerTile);
    const int rounded = int(units >= 0.0 ? units + 0.5 : units - 0.5);
    return static_cast<i16>(rounded);
}

// Both ends pulled in by the same eighth of a texel every other emitter uses.
// The original insets only its high end and only by a hundredth of a texel;
// ours is the PICA's tile-boundary margin and the reason is written up in
// vertex.hpp. At an eighth of a texel it is invisible on a partial range too.
i16 insetLow(i16 v) { return static_cast<i16>(v + kUvInset); }
i16 insetHigh(i16 v) { return static_cast<i16>(v - kUvInset); }

}  // namespace

void addBox(int x, int y, int z, const AABB& bounds, const u16 tiles[6], u8 light, bool shaded,
            int faceMask, MeshBuilder& out, DetailPass pass)
{
    for (int face = 0; face < kFaceCount; ++face) {
        if ((faceMask & (1 << face)) == 0) {
            continue;
        }

        const int tile = tiles[face] < kAtlasTileCount ? int(tiles[face]) : 0;
        const int tileU = tile % kAtlasTilesPerEdge;
        const int tileV = tile / kAtlasTilesPerEdge;
        const FaceUv& map = kFaceUv[face];

        // The two ends of each axis. `uLo` is what a corner whose
        // `kFaceCornerUV` entry is 0 takes, and that table is where the
        // per-face flips live.
        const double uMin = boundLow(bounds, map.u);
        const double uMax = boundHigh(bounds, map.u);
        const double vMin = boundLow(bounds, map.v);
        const double vMax = boundHigh(bounds, map.v);

        const i16 uLo = insetLow(uvAt(tileU, uMin));
        const i16 uHi = insetHigh(uvAt(tileU, uMax));
        const i16 vLo = insetLow(uvAt(tileV, vMin));
        const i16 vHi = insetHigh(uvAt(tileV, vMax));

        i16 corner[4][3];
        i16 uv[4][2];
        for (int c = 0; c < 4; ++c) {
            const Corner& unit = kFaceCorner[face][c];
            corner[c][0] = detailPos(x, float(unit.x != 0 ? bounds.maxX : bounds.minX));
            corner[c][1] = detailPos(y, float(unit.y != 0 ? bounds.maxY : bounds.minY));
            corner[c][2] = detailPos(z, float(unit.z != 0 ? bounds.maxZ : bounds.minZ));
            uv[c][0] = kFaceCornerUV[face][c][0] != 0 ? uHi : uLo;
            uv[c][1] = kFaceCornerUV[face][c][1] != 0 ? vHi : vLo;
        }

        out.addDetailQuad(corner, uv, shaded ? kFaceShade[face] : u8(255), light, face, pass);
    }
}

void addBox(int x, int y, int z, const AABB& bounds, u16 tile, u8 light, bool shaded, int faceMask,
            MeshBuilder& out, DetailPass pass)
{
    const u16 tiles[6] = {tile, tile, tile, tile, tile, tile};
    addBox(x, y, z, bounds, tiles, light, shaded, faceMask, out, pass);
}

}  // namespace mc::mesh
