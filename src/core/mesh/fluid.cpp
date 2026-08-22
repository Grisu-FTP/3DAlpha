#include "core/mesh/fluid.hpp"

#include "core/block/registry.hpp"

#include <cassert>
#include <cmath>

namespace mc::mesh {

using block::BlockDef;

namespace {

// One atlas texel. Tiles are 16x16 and a tile is kUvUnitsPerTile, so the
// original's pixel-denominated UV arithmetic converts by a constant.
static_assert(kUvUnitsPerTile % 16 == 0, "a tile must be a whole number of texels");
constexpr int kUvUnitsPerTexel = kUvUnitsPerTile / 16;

// The original insets the far edge of a fluid's side texture by 0.01 of a
// texel, to keep a filtered sample off the next tile. That is 0.64 of our UV
// units, and 1 is the nearest value the fixed-point format can express -- a
// 64th of a texel, which does the same job.
constexpr int kEdgeLip = 1;

const BlockDef& at(const MeshScratch& scratch, int x, int y, int z)
{
    return block::def(scratch.block(x, y, z));
}

i16 uvTexels(int texels) { return static_cast<i16>(texels * kUvUnitsPerTexel); }

// The tile's origin in texels, which is how the original addresses the atlas:
// `(tile & 15) << 4` across and `tile & 240` down.
struct TileOrigin {
    int u, v;
};

TileOrigin tileOrigin(u16 texture)
{
    const int tile = texture < kAtlasTileCount ? texture : 0;
    return {(tile & 15) << 4, tile & 240};
}

// The four cells meeting at a corner, in the order `bc.a(int,int,int,gb)`
// walks them: px = x - (i & 1), pz = z - ((i >> 1) & 1).
float cornerHeight(const MeshScratch& scratch, int x, int y, int z, u8 material)
{
    int count = 0;
    float total = 0.0f;

    for (int i = 0; i < 4; ++i) {
        const int px = x - (i & 1);
        const int pz = z - ((i >> 1) & 1);

        // Fluid directly above this cell: the corner is pinned to the top of
        // the block, which is what keeps a waterfall's column square instead of
        // pinching in at every block boundary.
        if (at(scratch, px, y + 1, pz).material == material) {
            return 1.0f;
        }

        if (at(scratch, px, y, pz).material == material) {
            const int level = scratch.metadata(px, y, pz);

            // Sources and falling columns count eleven times over. That x10 is
            // what keeps a lake's surface flat rather than sagging toward its
            // edges, where the flowing cells with their lower heights are.
            if (level >= 8 || level == 0) {
                total += fluidHeightPercent(level) * 10.0f;
                count += 10;
            }
            total += fluidHeightPercent(level);
            count += 1;
        } else if (!at(scratch, px, y, pz).solid) {
            // Empty: pulls the corner down. A solid neighbour contributes to
            // neither sum, so water against a wall keeps its height.
            total += 1.0f;
            count += 1;
        }
    }

    // Every corner of a fluid block samples that block itself, so `count` is at
    // least the eleven or one it contributes. The original would divide by zero
    // here; we cannot reach it.
    assert(count > 0);
    return 1.0f - total / static_cast<float>(count);
}

// `jp.b(nm,III)`: the cell's flow level, or -1 when it is not this fluid at
// all. Levels 8..15 are the falling flag and read as a source.
int flowDecay(const MeshScratch& scratch, int x, int y, int z, u8 material)
{
    if (at(scratch, x, y, z).material != material) {
        return -1;
    }
    const int level = scratch.metadata(x, y, z);
    return level >= 8 ? 0 : level;
}

// `aj.b()`: normalise, with the original's own 1e-4 guard and its float sqrt of
// a double length.
void normalise(double& x, double& y, double& z)
{
    const double length = static_cast<float>(std::sqrt(x * x + y * y + z * z));
    if (length < 1.0e-4) {
        x = y = z = 0.0;
        return;
    }
    x /= length;
    y /= length;
    z /= length;
}

}  // namespace

float fluidHeightPercent(int level)
{
    // `jp.b(I)F`. Ninths, not eighths: a source's surface sits at 1 - 1/9.
    if (level >= 8) {
        level = 0;
    }
    return static_cast<float>(level + 1) / 9.0f;
}

FluidCorners fluidCorners(const MeshScratch& scratch, int x, int y, int z, u8 material)
{
    return {{
        cornerHeight(scratch, x, y, z, material),
        cornerHeight(scratch, x, y, z + 1, material),
        cornerHeight(scratch, x + 1, y, z + 1, material),
        cornerHeight(scratch, x + 1, y, z, material),
    }};
}

bool fluidFaceVisible(const MeshScratch& scratch, int x, int y, int z, int face, u8 material)
{
    // `jp.c(nm,IIII)`, which takes the *neighbour's* coordinates.
    const BlockDef& def = at(scratch, x, y, z);

    if (def.material == material) {
        return false;
    }

    // Ice, by singleton identity in the jar -- there is no property behind it,
    // which is why the generator has to name it. The comparison is compiled out
    // entirely for a version with no ice, where the constant is 0 and no
    // constructed block takes material 0.
    if (mcver::kIceMaterial != 0 && def.material == mcver::kIceMaterial) {
        return false;
    }

    // A fluid's top face is drawn unconditionally, even buried under stone.
    // Wasteful and deliberate: it is what the original does, and the faces are
    // rare -- only fluid with a solid lid directly on it qualifies, since fluid
    // under fluid was already rejected above.
    if (face == kFacePosY) {
        return true;
    }

    // `ly.c(nm,IIII)` otherwise, which for a full-cube bounding box -- and a
    // fluid block's box is 0..1 on every axis while it renders -- reduces to
    // "is the neighbour an opaque cube".
    return !def.opaque;
}

u8 fluidLight(const MeshScratch& scratch, int x, int y, int z)
{
    const u8 here = scratch.light(x, y, z);
    const u8 above = scratch.light(x, y + 1, z);

    const u8 sky = (here >> 4) > (above >> 4) ? (here >> 4) : (above >> 4);
    const u8 block = (here & 15) > (above & 15) ? (here & 15) : (above & 15);
    return static_cast<u8>((sky << 4) | block);
}

float fluidFlowAngle(const MeshScratch& scratch, int x, int y, int z, u8 material)
{
    // `jp.e(nm,III)`, the flow vector, then `jp.a(nm,IIILgb;)D` on top of it.
    double vx = 0.0;
    double vy = 0.0;
    double vz = 0.0;

    const int mine = flowDecay(scratch, x, y, z, material);

    // Note this walks -X, -Z, +X, +Z, which is not the face order the emitter
    // below uses. Kept as the original has it, because the sign of the vector
    // depends on it.
    for (int i = 0; i < 4; ++i) {
        int px = x;
        int pz = z;
        if (i == 0) px -= 1;
        if (i == 1) pz -= 1;
        if (i == 2) px += 1;
        if (i == 3) pz += 1;

        int decay = flowDecay(scratch, px, y, pz, material);

        if (decay < 0) {
            // Not this fluid. If it is something you can walk through, the
            // fluid one step down still pulls -- this is what makes water
            // aim at the lip of a drop rather than at the drop's far wall.
            //
            // The predicate is Material.blocksMovement, `gb.c()`, and it is
            // identical to Material.isSolid, `gb.a()`, for every one of
            // a1.1.2's four material classes: air, liquid and the
            // no-collision material override both to false, the base
            // overrides neither. So the `solid` column answers both.
            if (!at(scratch, px, y, pz).solid) {
                decay = flowDecay(scratch, px, y - 1, pz, material);
                if (decay >= 0) {
                    const int weight = decay - (mine - 8);
                    vx += static_cast<double>((px - x) * weight);
                    vz += static_cast<double>((pz - z) * weight);
                }
            }
        } else {
            const int weight = decay - mine;
            vx += static_cast<double>((px - x) * weight);
            vz += static_cast<double>((pz - z) * weight);
        }
    }

    // A falling column with anything open around it -- at its own level or the
    // one above -- points almost straight down, so its texture stops spinning
    // with whatever horizontal imbalance it happened to have.
    if (scratch.metadata(x, y, z) >= 8) {
        const int open[8][4] = {
            {x, y, z - 1, kFaceNegZ},     {x, y, z + 1, kFacePosZ},
            {x - 1, y, z, kFaceNegX},     {x + 1, y, z, kFacePosX},
            {x, y + 1, z - 1, kFaceNegZ}, {x, y + 1, z + 1, kFacePosZ},
            {x - 1, y + 1, z, kFaceNegX}, {x + 1, y + 1, z, kFacePosX},
        };

        bool falling = false;
        for (const auto& cell : open) {
            if (fluidFaceVisible(scratch, cell[0], cell[1], cell[2], cell[3], material)) {
                falling = true;
                break;
            }
        }

        if (falling) {
            normalise(vx, vy, vz);
            vy += -6.0;
        }
    }

    normalise(vx, vy, vz);

    if (vx == 0.0 && vz == 0.0) {
        return kNoFlow;
    }

    // The original reads sine and cosine out of a 65,536-entry table, so this
    // angle is quantised there and is not here. The difference is under 1e-4 of
    // a radian on a texture rotation; the table is 256 KB and would be the
    // single largest thing in .rodata.
    constexpr double kHalfPi = 1.5707963267948966;
    return static_cast<float>(std::atan2(vz, vx) - kHalfPi);
}

void addFluid(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def,
              MeshBuilder& out)
{
    const u8 material = def.material;

    const bool drawTop = fluidFaceVisible(scratch, x, y + 1, z, kFacePosY, material);
    const bool drawBottom = fluidFaceVisible(scratch, x, y - 1, z, kFaceNegY, material);

    // Side order is the original's loop, which is -Z, +Z, -X, +X -- our face
    // numbering 2..5, so `side + 2` is the face and indexes both the per-face
    // texture table and the shade table.
    const bool drawSide[4] = {
        fluidFaceVisible(scratch, x, y, z - 1, kFaceNegZ, material),
        fluidFaceVisible(scratch, x, y, z + 1, kFacePosZ, material),
        fluidFaceVisible(scratch, x - 1, y, z, kFaceNegX, material),
        fluidFaceVisible(scratch, x + 1, y, z, kFacePosX, material),
    };

    if (!drawTop && !drawBottom && !drawSide[0] && !drawSide[1] && !drawSide[2] && !drawSide[3]) {
        return;
    }

    const FluidCorners corners = fluidCorners(scratch, x, y, z, material);

    // Water goes in the sorted, blended pass and lava does not. That is
    // a1.1.2's own getRenderBlockPass, in the block table -- see
    // BlockDef::translucent -- and not a judgement made here.
    const DetailPass pass = def.translucent ? DetailPass::Translucent : DetailPass::Opaque;

    const i16 x0 = static_cast<i16>(x * kDetailUnitsPerBlock);
    const i16 x1 = static_cast<i16>((x + 1) * kDetailUnitsPerBlock);
    const i16 z0 = static_cast<i16>(z * kDetailUnitsPerBlock);
    const i16 z1 = static_cast<i16>((z + 1) * kDetailUnitsPerBlock);
    const i16 y0 = static_cast<i16>(y * kDetailUnitsPerBlock);

    const auto surface = [&](int corner) { return detailPos(y, corners.h[corner]); };

    if (drawTop) {
        // Still fluid keeps the still tile and reads the whole of it. Flowing
        // fluid takes the flowing tile and spins the sample square about the
        // flow direction -- and about the tile's *far corner*, not its centre,
        // so the square straddles four tiles.
        //
        // That is not a transcription error. a1.1.2's terrain.png reserves the
        // 2x2 below-right of each flowing tile for the same fluid: water's
        // flowing tile is 206 and 207, 222 and 223 are all water, and lava's is
        // 238 with 239, 254 and 255. The layout is built for this.
        const float angle = fluidFlowAngle(scratch, x, y, z, material);
        const bool flowing = angle != kNoFlow;

        // `block.getBlockTexture(1)` for still, `(2)` for flowing -- the
        // original asks the top face for one and a *side* face for the other,
        // and for a fluid every side answers the same flowing tile.
        const TileOrigin tile = tileOrigin(flowing ? def.faces[kFaceNegZ] : def.faces[kFacePosY]);
        const int centre = flowing ? 16 : 8;

        const float radius = 8.0f * kUvUnitsPerTexel;
        const float du = flowing ? std::sin(angle) * radius : 0.0f;
        const float dv = flowing ? std::cos(angle) * radius : radius;

        const i16 uc = uvTexels(tile.u + centre);
        const i16 vc = uvTexels(tile.v + centre);

        const auto uv = [&](float su, float sv) {
            return static_cast<i16>(std::lround(su * dv + sv * du));
        };

        const i16 corner[4][3] = {
            {x0, surface(0), z0},
            {x0, surface(1), z1},
            {x1, surface(2), z1},
            {x1, surface(3), z0},
        };
        const i16 texture[4][2] = {
            {static_cast<i16>(uc + uv(-1, -1)), static_cast<i16>(vc + uv(-1, +1))},
            {static_cast<i16>(uc + uv(-1, +1)), static_cast<i16>(vc + uv(+1, +1))},
            {static_cast<i16>(uc + uv(+1, +1)), static_cast<i16>(vc + uv(+1, -1))},
            {static_cast<i16>(uc + uv(+1, -1)), static_cast<i16>(vc + uv(-1, -1))},
        };

        // The fluid's own cell, through the max-with-above override. A fluid is
        // not an opaque cube, so its cell carries real light in the first place.
        out.addDetailQuad(corner, texture, kFaceShade[kFacePosY], fluidLight(scratch, x, y, z),
                          kFacePosY, pass);
    }

    if (drawBottom) {
        // An ordinary cube face, so it borrows the cube tables wholesale. It
        // still belongs in the detail stream: the whole block has to be drawn
        // in one pass once that pass is sorted and blended.
        const TileOrigin tile = tileOrigin(def.faces[kFaceNegY]);
        const i16 u0 = uvTexels(tile.u);
        const i16 v0 = uvTexels(tile.v);

        i16 corner[4][3];
        i16 texture[4][2];
        for (int c = 0; c < 4; ++c) {
            const Corner& unit = kFaceCorner[kFaceNegY][c];
            corner[c][0] = static_cast<i16>((x + unit.x) * kDetailUnitsPerBlock);
            corner[c][1] = static_cast<i16>((y + unit.y) * kDetailUnitsPerBlock);
            corner[c][2] = static_cast<i16>((z + unit.z) * kDetailUnitsPerBlock);
            texture[c][0] = static_cast<i16>(u0 + kFaceCornerUV[kFaceNegY][c][0] * kUvUnitsPerTile);
            texture[c][1] = static_cast<i16>(v0 + kFaceCornerUV[kFaceNegY][c][1] * kUvUnitsPerTile);
        }

        out.addDetailQuad(corner, texture, kFaceShade[kFaceNegY], fluidLight(scratch, x, y - 1, z),
                          kFaceNegY, pass);
    }

    for (int side = 0; side < 4; ++side) {
        if (!drawSide[side]) {
            continue;
        }

        const int face = side + 2;

        // Which two of the four corner heights this face spans, and which way
        // round, so the quad stays wound counter-clockwise from outside.
        static constexpr int kNear[4] = {0, 2, 1, 3};
        static constexpr int kFar[4] = {3, 1, 0, 2};
        static constexpr i16 kNearX[4] = {0, 1, 0, 1};
        static constexpr i16 kFarX[4] = {1, 0, 0, 1};
        static constexpr i16 kNearZ[4] = {0, 1, 1, 0};
        static constexpr i16 kFarZ[4] = {0, 1, 0, 1};

        const i16 nx = kNearX[side] ? x1 : x0;
        const i16 fx = kFarX[side] ? x1 : x0;
        const i16 nz = kNearZ[side] ? z1 : z0;
        const i16 fz = kFarZ[side] ? z1 : z0;

        const float hNear = corners.h[kNear[side]];
        const float hFar = corners.h[kFar[side]];

        const TileOrigin tile = tileOrigin(def.faces[face]);

        // u spans the tile; v is anchored to the tile's *top* and starts where
        // the surface does, so the texture is cropped by the fluid's height
        // rather than squashed into it.
        const i16 uNear = uvTexels(tile.u);
        const i16 uFar = static_cast<i16>(uvTexels(tile.u + 16) - kEdgeLip);
        const i16 vNear =
            static_cast<i16>(std::lround((tile.v + (1.0f - hNear) * 16.0f) * kUvUnitsPerTexel));
        const i16 vFar =
            static_cast<i16>(std::lround((tile.v + (1.0f - hFar) * 16.0f) * kUvUnitsPerTexel));
        const i16 vBottom = static_cast<i16>(uvTexels(tile.v + 16) - kEdgeLip);

        const i16 corner[4][3] = {
            {nx, detailPos(y, hNear), nz},
            {fx, detailPos(y, hFar), fz},
            {fx, y0, fz},
            {nx, y0, nz},
        };
        const i16 texture[4][2] = {
            {uNear, vNear},
            {uFar, vFar},
            {uFar, vBottom},
            {uNear, vBottom},
        };

        // The neighbour's cell, as for a cube face: the side of a block is lit
        // by what it faces, not by what is inside it. Through the same override,
        // so a side face against air one block under the surface is lit by the
        // surface rather than by the shadow it sits in.
        const int lx = side == 2 ? x - 1 : (side == 3 ? x + 1 : x);
        const int lz = side == 0 ? z - 1 : (side == 1 ? z + 1 : z);

        out.addDetailQuad(corner, texture, kFaceShade[face], fluidLight(scratch, lx, y, lz), face,
                          pass);
    }
}

}  // namespace mc::mesh
