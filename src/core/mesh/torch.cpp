#include "core/mesh/torch.hpp"

#include "core/mesh/vertex.hpp"

namespace mc::mesh {

using block::BlockDef;

namespace {

// One atlas texel, the same conversion fluid.cpp makes: the original's UV
// arithmetic is denominated in texels of a 256-pixel atlas and ours in
// kUvUnitsPerTile per tile.
static_assert(kUvUnitsPerTile % 16 == 0, "a tile must be a whole number of texels");

// The torch is drawn with one colour for all five quads -- `bc.b` calls
// setColorOpaque_F once and never consults the per-face shade table, exactly
// as the crossed-square renderer does.
constexpr u8 kUnshaded = 255;

// Sky and block light both at 15. The lightmap is
// `lightBrightness(effectiveLightLevel(sky, block, subtracted))` and block
// light is never reduced by the day, so this is 1.0 at every hour -- which is
// what `brightness = 1.0f` means for a light-emitting block.
constexpr u8 kFullBright = (15 << 4) | 15;

struct TileOrigin {
    int u, v;  // in texels
};

TileOrigin tileOrigin(u16 texture)
{
    const int tile = texture < kAtlasTileCount ? texture : 0;
    return {(tile & 15) << 4, tile & 240};
}

i16 uvAt(int originTexels, int texels)
{
    return static_cast<i16>((originTexels + texels) * kUvUnitsPerTexel);
}

}  // namespace

// `bc.b(ly,III)`, whose whole body after the brightness setup is:
//
//     double d  = 0.4;              // kTorchTilt
//     double d1 = 0.5 - d;          // kTorchOffset
//     double d2 = 0.2;              // kTorchRise
//     if      (meta == 1) renderTorchAtAngle(block, x - d1, y + d2, z,  -d, 0.0);
//     else if (meta == 2) renderTorchAtAngle(block, x + d1, y + d2, z,   d, 0.0);
//     else if (meta == 3) renderTorchAtAngle(block, x, y + d2, z - d1, 0.0,  -d);
//     else if (meta == 4) renderTorchAtAngle(block, x, y + d2, z + d1, 0.0,   d);
//     else                renderTorchAtAngle(block, x, y, z, 0.0, 0.0);
//
// Note which way the pair goes: metadata 1 shifts the base toward -X *and*
// leans it further -X, which reads backwards until you see that the tilt moves
// the bottom. The base ends up flat against the -X wall and the flame leans
// out into the block. Getting the sign wrong buries the torch in the wall.
TorchMount torchMount(int metadata)
{
    switch (metadata) {
        case 1:
            return {-kTorchOffset, kTorchRise, 0.0f, -kTorchTilt, 0.0f};
        case 2:
            return {+kTorchOffset, kTorchRise, 0.0f, +kTorchTilt, 0.0f};
        case 3:
            return {0.0f, kTorchRise, -kTorchOffset, 0.0f, -kTorchTilt};
        case 4:
            return {0.0f, kTorchRise, +kTorchOffset, 0.0f, +kTorchTilt};
        default:
            return {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }
}

u8 torchLight(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def)
{
    if (def.light > 0) {
        return kFullBright;
    }
    return scratch.light(x, y, z);
}

// `bc.a(ly,DDDDD)` -- renderTorchAtAngle. Five quads, and the two things about
// it that are not obvious:
//
// **The four side quads are full-block, not stick-sized.** Each spans the whole
// block in its horizontal axis and the whole block in height, textured with the
// entire torch tile. What makes a torch look like a stick is that the tile is
// transparent everywhere else, so this render type depends on the alpha test in
// the opaque detail pass exactly as the crossed squares do. A placeholder atlas
// that fills the tile solid draws a torch as a slab, which is a fault in the
// atlas and not here -- textures.cpp carves the stick out for that reason.
//
// **The lean is a shear.** The bottom edge of every side quad is displaced by
// (dx, dz) and the top edge is not, so a point at height t sits at dx*(1-t).
// The cap is placed independently at dx*(1 - 0.625), which is the same line
// evaluated at the stick's top -- that consistency is the check that the shear
// was transcribed the right way up.
//
// There is no bottom face. A torch is never seen from below in the original and
// the sixth quad was simply not written.
void addTorch(const MeshScratch& scratch, int x, int y, int z, const BlockDef& def,
              MeshBuilder& out)
{
    const TorchMount mount = torchMount(scratch.metadata(x, y, z));
    const u8 light = torchLight(scratch, x, y, z, def);

    // `getBlockTextureFromSide(0)`, which is the table's face 0 and not the
    // block's bare `texture`: the two redstone torches carry a different tile
    // on their top face and the torch renderer must not pick it up.
    const TileOrigin tile = tileOrigin(def.faces[0]);

    // The full tile, for the side quads -- inset off the tile boundary, which
    // is a hardware fix and not a change of shape; see vertex.hpp's kUvInset.
    // The cap below is not inset because texels 7..9 and 6..8 are interior and
    // never touch a boundary.
    const i16 u0 = i16(uvAt(tile.u, 0) + kUvInset);
    const i16 u1 = i16(uvAt(tile.u, 16) - kUvInset);
    const i16 v0 = i16(uvAt(tile.v, 0) + kUvInset);
    const i16 v1 = i16(uvAt(tile.v, 16) - kUvInset);

    // The cap samples texels 7..9 across and 6..8 down -- the two-texel square
    // the flame sits on. The original writes these as 0.02734375 and friends,
    // which are those texel counts over 256.
    const i16 capU0 = uvAt(tile.u, 7);
    const i16 capU1 = uvAt(tile.u, 9);
    const i16 capV0 = uvAt(tile.v, 6);
    const i16 capV1 = uvAt(tile.v, 8);

    const float d9 = kTorchHalfWidth;
    const float top = kTorchTopHeight;

    // `x += 0.5` inside the original, applied to a base the caller had already
    // shifted, so the centre and the two edges all carry the mount offset.
    const float cx = 0.5f + mount.ox;
    const float cz = 0.5f + mount.oz;
    const float loX = mount.ox;
    const float hiX = 1.0f + mount.ox;
    const float loZ = mount.oz;
    const float hiZ = 1.0f + mount.oz;

    const float yTop = mount.oy + 1.0f;
    const float yBot = mount.oy;

    auto px = [x](float o) { return detailPos(x, o); };
    auto py = [y](float o) { return detailPos(y, o); };
    auto pz = [z](float o) { return detailPos(z, o); };

    // The cap, at the height the stick ends rather than at the block's top.
    const float capX = cx + mount.dx * (1.0f - top);
    const float capZ = cz + mount.dz * (1.0f - top);
    const i16 cap[4][3] = {
        {px(capX - d9), py(mount.oy + top), pz(capZ - d9)},
        {px(capX - d9), py(mount.oy + top), pz(capZ + d9)},
        {px(capX + d9), py(mount.oy + top), pz(capZ + d9)},
        {px(capX + d9), py(mount.oy + top), pz(capZ - d9)},
    };
    const i16 capUv[4][2] = {{capU0, capV0}, {capU0, capV1}, {capU1, capV1}, {capU1, capV0}};
    out.addDetailQuad(cap, capUv, kUnshaded, light, kFacePosY, DetailPass::Opaque);

    // The four sides. Each is wound counter-clockwise seen from outside the
    // stick, so the two facing away are culled and a torch costs two drawn
    // quads from any viewpoint. No back copies, unlike the crossed squares,
    // which are single planes and have to survive being seen from either side.
    const i16 sideUv[4][2] = {{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}};

    const i16 negX[4][3] = {
        {px(cx - d9), py(yTop), pz(loZ)},
        {px(cx - d9 + mount.dx), py(yBot), pz(loZ + mount.dz)},
        {px(cx - d9 + mount.dx), py(yBot), pz(hiZ + mount.dz)},
        {px(cx - d9), py(yTop), pz(hiZ)},
    };
    out.addDetailQuad(negX, sideUv, kUnshaded, light, kFaceNegX, DetailPass::Opaque);

    const i16 posX[4][3] = {
        {px(cx + d9), py(yTop), pz(hiZ)},
        {px(cx + d9 + mount.dx), py(yBot), pz(hiZ + mount.dz)},
        {px(cx + d9 + mount.dx), py(yBot), pz(loZ + mount.dz)},
        {px(cx + d9), py(yTop), pz(loZ)},
    };
    out.addDetailQuad(posX, sideUv, kUnshaded, light, kFacePosX, DetailPass::Opaque);

    const i16 posZ[4][3] = {
        {px(loX), py(yTop), pz(cz + d9)},
        {px(loX + mount.dx), py(yBot), pz(cz + d9 + mount.dz)},
        {px(hiX + mount.dx), py(yBot), pz(cz + d9 + mount.dz)},
        {px(hiX), py(yTop), pz(cz + d9)},
    };
    out.addDetailQuad(posZ, sideUv, kUnshaded, light, kFacePosZ, DetailPass::Opaque);

    const i16 negZ[4][3] = {
        {px(hiX), py(yTop), pz(cz - d9)},
        {px(hiX + mount.dx), py(yBot), pz(cz - d9 + mount.dz)},
        {px(loX + mount.dx), py(yBot), pz(cz - d9 + mount.dz)},
        {px(loX), py(yTop), pz(cz - d9)},
    };
    out.addDetailQuad(negZ, sideUv, kUnshaded, light, kFaceNegZ, DetailPass::Opaque);
}

}  // namespace mc::mesh
