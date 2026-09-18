// See held_item.hpp.

#include "core/render/held_item.hpp"

#include "core/block/block_def.hpp"
#include "core/block/model.hpp"
#include "core/block/registry.hpp"
#include "core/item/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/render/box_model.hpp"
#include "core/util/math_helper.hpp"

#include "version_config.hpp"

#include <cmath>

namespace mc::render {
namespace {

constexpr float kPi = 3.1415927f;
constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);

// **Full brightness on the flat branch.** `renderItem`'s sprite half never
// touches the colour, exactly as `RenderItem` does not -- the light byte is
// what darkens it in a cave. The block half is the one that shades; see
// `addBox`.
constexpr u8 kFullBright = 255;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

// One tile of a 16 x 16 sheet, inset an eighth of a texel the way every other
// whole-tile quad in this project is. Both sheets are 16 x 16, so this is the
// same arithmetic for terrain.png and gui/items.png.
struct TileUv {
    i16 u0, v0, u1, v1;
};

TileUv tileUv(int tile)
{
    const int clamped = tile >= 0 && tile < mesh::kAtlasTileCount ? tile : 0;
    const int col = clamped % mesh::kAtlasTilesPerEdge;
    const int row = clamped / mesh::kAtlasTilesPerEdge;
    return TileUv{mesh::tileUvMin(col), mesh::tileUvMin(row), mesh::tileUvMax(col),
                  mesh::tileUvMax(row)};
}

// The block an item puts into the world, or the block it *is* -- the same rule
// `item_entity_mesh.cpp` and `gui/item_icon.cpp` follow, and for the same
// reason: item 324 is a door and block 64 is the door it leaves.
u16 blockOf(const item::ItemDef& def, item::ItemId id)
{
    if (def.places != 0) {
        return def.places;
    }
    return def.sheet == item::IconSheet::Terrain ? u16(id) : 0;
}

// **An affine transform composed the way OpenGL's matrix stack composes.**
//
// Every call here right-multiplies, which is what `glTranslatef`, `glRotatef`
// and `glScalef` do to the current matrix -- so the sequence below reads in the
// same order the class file makes the calls, and the *last* one applied is the
// one nearest the geometry. Getting that backwards is the difference between
// an item in the corner of the screen and an item behind the camera.
//
// Three rows of four: the fourth column is the translation, and the transform
// is `p' = M * (p, 1)`.
struct Transform {
    float m[3][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f}};

    void translate(float x, float y, float z)
    {
        for (int i = 0; i < 3; ++i) {
            m[i][3] += m[i][0] * x + m[i][1] * y + m[i][2] * z;
        }
    }

    void scale(float s)
    {
        for (int i = 0; i < 3; ++i) {
            for (int c = 0; c < 3; ++c) {
                m[i][c] *= s;
            }
        }
    }

    // `glRotatef` goes through the C library's sine, not through a1.1.2's
    // 65,536-entry table -- it is GL doing the arithmetic and not the game --
    // so these three use std::sin and the animation arithmetic in
    // `buildHeldItem` uses MathHelper. The difference is about a thousandth of
    // a degree, and keeping the two apart is what makes either of them
    // checkable against the original.
    void rotateX(float degrees)
    {
        const float a = degrees * kPi / 180.0f;
        const float c = std::cos(a);
        const float s = std::sin(a);
        for (int i = 0; i < 3; ++i) {
            const float y = m[i][1];
            const float z = m[i][2];
            m[i][1] = y * c + z * s;
            m[i][2] = -y * s + z * c;
        }
    }

    void rotateY(float degrees)
    {
        const float a = degrees * kPi / 180.0f;
        const float c = std::cos(a);
        const float s = std::sin(a);
        for (int i = 0; i < 3; ++i) {
            const float x = m[i][0];
            const float z = m[i][2];
            m[i][0] = x * c - z * s;
            m[i][2] = x * s + z * c;
        }
    }

    void rotateZ(float degrees)
    {
        const float a = degrees * kPi / 180.0f;
        const float c = std::cos(a);
        const float s = std::sin(a);
        for (int i = 0; i < 3; ++i) {
            const float x = m[i][0];
            const float y = m[i][1];
            m[i][0] = x * c + y * s;
            m[i][1] = -x * s + y * c;
        }
    }

    void apply(float x, float y, float z, float* ox, float* oy, float* oz) const
    {
        *ox = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        *oy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        *oz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    }
};

// Writes one vertex through the transform. Every branch below goes through
// this, so there is one place that can put a field in the wrong slot.
void writeVertex(mesh::DetailVertex& v, const Transform& t, float x, float y, float z, i16 u,
                 i16 vv, u8 shade, u8 light)
{
    float wx, wy, wz;
    t.apply(x, y, z, &wx, &wy, &wz);
    v.x = toUnits(double(wx));
    v.y = toUnits(double(wy));
    v.z = toUnits(double(wz));
    v.face = 0;
    v.u = u;
    v.v = vv;
    v.r = shade;
    v.g = shade;
    v.b = shade;
    v.light = light;
}

// One box of `renderBlockOnInventory`, in block-local 0..1 coordinates.
//
// **The shade is this project's face table and not the original's lighting.**
// `bc.a(Lly;)V` sets a normal per face and leaves the colour alone, because
// `RenderHelper.enableStandardItemLighting` has two directional lights turned
// on behind it. The PICA has no fixed-function lighting and this renderer has
// no light of its own, so the six faces are darkened by
// `mesh::kFaceShade` -- which is what a slot on the bottom screen already does
// (`gui/item_icon.cpp`) and what the same block in the world is drawn with, so
// all three agree.
// Four vertices per face the mask keeps.
int boxVertices(int faceMask)
{
    int quads = 0;
    for (int face = 0; face < mesh::kFaceCount; ++face) {
        if ((faceMask & (1 << face)) != 0) {
            ++quads;
        }
    }
    return quads * 4;
}

// `faceMask` is which of the six to emit, as `block::renderBoxes` fills it in.
// Only the cactus asks for fewer than all of them, and it asks three times:
// its item shape is one cell drawn as a cap box and two pairs of inset sides.
//
// **The tile is sampled over the box's own extent**, through the same
// `mesh::boxTileUv` the world mesher draws a slab and a fence post with. It is
// the original's rule -- `renderBlockOnInventory` sets the block's bounds and
// then calls the very `bc.a`..`bc.f` the world uses, which clip their UVs to
// those bounds -- and it is why a fence in the hand is the narrow strip of the
// plank tile the post covers rather than the whole tile stretched across it.
int addBox(const AABB& box, const Transform& t, const u16 tiles[6], int faceMask, u8 light,
           mesh::DetailVertex* out, int max)
{
    if (max < boxVertices(faceMask)) {
        return 0;
    }
    const double lo[3] = {box.minX, box.minY, box.minZ};
    const double hi[3] = {box.maxX, box.maxY, box.maxZ};

    int written = 0;
    for (int face = 0; face < mesh::kFaceCount; ++face) {
        if ((faceMask & (1 << face)) == 0) {
            continue;
        }
        const mesh::BoxTileUv uv = mesh::boxTileUv(int(tiles[face]), box, face);
        for (int c = 0; c < 4; ++c) {
            const mesh::Corner& corner = mesh::kFaceCorner[face][c];
            const float x = float(corner.x != 0 ? hi[0] : lo[0]);
            const float y = float(corner.y != 0 ? hi[1] : lo[1]);
            const float z = float(corner.z != 0 ? hi[2] : lo[2]);
            const i16 u = mesh::kFaceCornerUV[face][c][0] != 0 ? uv.u1 : uv.u0;
            const i16 v = mesh::kFaceCornerUV[face][c][1] != 0 ? uv.v1 : uv.v0;
            writeVertex(out[written], t, x, y, z, u, v, mesh::kFaceShade[face], light);
            ++written;
        }
    }
    return written;
}

// **The flat icon given thickness** -- the `else` half of `jh.a(Lev;)V`.
//
// A front face and a back face a sixteenth of a unit apart, plus four runs of
// sixteen one-texel strips joining their edges. The strips are what make a
// sword in the hand look like a sword and not like a decal: each one samples a
// single column (or row) of the icon, and the alpha test carves the silhouette
// out of all 66 quads at once.
//
// The half-texel the original subtracts from every strip's coordinate --
// `0.001953125F`, which is 0.5/256 -- is why the strips need no inset of ours:
// it already lands them in the middle of a texel rather than on a boundary.
int addSprite(int tile, const Transform& outer, u8 light, mesh::DetailVertex* out, int max)
{
    if (max < kHeldSpriteQuads * 4) {
        return 0;
    }

    // `glTranslatef(-f8, -f9, 0)` with f8 = 0 and f9 = 0.3, then the scale, the
    // two turns and the last translate. All five are literals in the method.
    Transform t = outer;
    t.translate(0.0f, -0.3f, 0.0f);
    t.scale(1.5f);
    t.rotateY(50.0f);
    t.rotateZ(335.0f);
    t.translate(-0.9375f, -0.0625f, 0.0f);

    // The tile's four edges, uninset: the strips below interpolate between them
    // and land on texel centres by themselves, and the two full-tile faces take
    // the project's eighth-of-a-texel inset instead.
    const int clamped = tile >= 0 && tile < mesh::kAtlasTileCount ? tile : 0;
    const int col = clamped % mesh::kAtlasTilesPerEdge;
    const int row = clamped / mesh::kAtlasTilesPerEdge;
    const float rawU0 = float(col * mesh::kUvUnitsPerTile);
    const float rawU1 = float((col + 1) * mesh::kUvUnitsPerTile);
    const float rawV0 = float(row * mesh::kUvUnitsPerTile);
    const float rawV1 = float((row + 1) * mesh::kUvUnitsPerTile);
    // `0.001953125F` of the whole 256-texel sheet, which is half a texel.
    const float halfTexel = float(mesh::kUvUnitsPerTexel) * 0.5f;
    const TileUv inset = tileUv(clamped);

    // `f11`, the depth of the extrusion: a sixteenth of the icon's own width.
    constexpr float kDepth = 0.0625f;

    int written = 0;
    const auto vertex = [&](float x, float y, float z, float u, float v) {
        writeVertex(out[written], t, x, y, z, i16(u + (u >= 0.0f ? 0.5f : -0.5f)),
                    i16(v + (v >= 0.0f ? 0.5f : -0.5f)), kFullBright, light);
        ++written;
    };

    // The front face, at z = 0. u runs backwards across x -- the original's own
    // winding, and it is why a held sword points the way it does.
    vertex(0.0f, 0.0f, 0.0f, float(inset.u1), float(inset.v1));
    vertex(1.0f, 0.0f, 0.0f, float(inset.u0), float(inset.v1));
    vertex(1.0f, 1.0f, 0.0f, float(inset.u0), float(inset.v0));
    vertex(0.0f, 1.0f, 0.0f, float(inset.u1), float(inset.v0));

    // The back face, at z = -kDepth.
    vertex(0.0f, 1.0f, -kDepth, float(inset.u1), float(inset.v0));
    vertex(1.0f, 1.0f, -kDepth, float(inset.u0), float(inset.v0));
    vertex(1.0f, 0.0f, -kDepth, float(inset.u0), float(inset.v1));
    vertex(0.0f, 0.0f, -kDepth, float(inset.u1), float(inset.v1));

    // The two vertical runs. `f13` walks the sixteen columns; the -X run sits
    // on the column's near edge and the +X run a sixteenth further along, which
    // is what closes the box on both sides of every texel.
    for (int i = 0; i < 16; ++i) {
        const float f13 = float(i) / 16.0f;
        const float u = rawU1 + (rawU0 - rawU1) * f13 - halfTexel;
        const float x = f13;
        vertex(x, 0.0f, -kDepth, u, float(rawV1));
        vertex(x, 0.0f, 0.0f, u, float(rawV1));
        vertex(x, 1.0f, 0.0f, u, float(rawV0));
        vertex(x, 1.0f, -kDepth, u, float(rawV0));
    }
    for (int i = 0; i < 16; ++i) {
        const float f13 = float(i) / 16.0f;
        const float u = rawU1 + (rawU0 - rawU1) * f13 - halfTexel;
        const float x = f13 + kDepth;
        vertex(x, 1.0f, -kDepth, u, float(rawV0));
        vertex(x, 1.0f, 0.0f, u, float(rawV0));
        vertex(x, 0.0f, 0.0f, u, float(rawV1));
        vertex(x, 0.0f, -kDepth, u, float(rawV1));
    }

    // And the two horizontal ones, the same sixteen rows seen from above and
    // from below.
    for (int i = 0; i < 16; ++i) {
        const float f13 = float(i) / 16.0f;
        const float v = rawV1 + (rawV0 - rawV1) * f13 - halfTexel;
        const float y = f13 + kDepth;
        vertex(0.0f, y, 0.0f, float(rawU1), v);
        vertex(1.0f, y, 0.0f, float(rawU0), v);
        vertex(1.0f, y, -kDepth, float(rawU0), v);
        vertex(0.0f, y, -kDepth, float(rawU1), v);
    }
    for (int i = 0; i < 16; ++i) {
        const float f13 = float(i) / 16.0f;
        const float v = rawV1 + (rawV0 - rawV1) * f13 - halfTexel;
        const float y = f13;
        vertex(1.0f, y, 0.0f, float(rawU0), v);
        vertex(0.0f, y, 0.0f, float(rawU1), v);
        vertex(0.0f, y, -kDepth, float(rawU1), v);
        vertex(1.0f, y, -kDepth, float(rawU0), v);
    }

    return written;
}

// **The player's own right arm** -- `bu.b()`, which is
// `RenderPlayer.drawFirstPersonHand`, and is three lines:
//
//     modelBipedMain.onGround = 0.0F;
//     modelBipedMain.setRotationAngles(0, 0, 0, 0, 0, 0.0625F);
//     modelBipedMain.bipedRightArm.render(0.0625F);
//
// So the only thing worth deriving is what `setRotationAngles` leaves the arm
// at when every one of its arguments is zero, and the answer came out of `cr`
// rather than out of memory. **Two of the three angles are zero and the third
// is not**: the method's last four statements add an idle sway to both arms,
// and the right arm's Z term is `cos(age * 0.09) * 0.05 + 0.05` -- which at age
// zero is `0.1` and not `0`. The X term beside it is `sin(age * 0.067) * 0.05`,
// which *is* zero. The swing block above them runs (`onGround` is 0, and the
// gate is `> -9990`) and every term in it is a sine or a square of zero, so it
// leaves the rotation point exactly where the constructor put it.
//
// The box itself is `new ip(40, 16)` with `addBox(-3, -2, -2, 4, 12, 4, 0)` and
// `setRotationPoint(-5, 2, 0)` -- `bu` builds its model with an expand of 0 and
// a yOffset of 0, so neither adds anything.
int addArm(const Transform& outer, u8 light, mesh::DetailVertex* out, int max)
{
    if (max < kBoxVertices) {
        return 0;
    }

    // The five transforms between the swing rotations and the model, and
    // **they are in blocks rather than in model units** -- `render(0.0625F)`
    // scales what is inside it and nothing outside. -1, 3.6 and 3.5 blocks
    // sounds like it would put the arm behind the player; the three rotations
    // after it turn that back to roughly (0.35, -1.1, -0.74), which is the
    // bottom right corner. Two `glScalef(1, 1, 1)` calls sit in this run in the
    // class file and are left out here.
    Transform t = outer;
    t.translate(-1.0f, 3.6f, 3.5f);
    t.rotateZ(120.0f);
    t.rotateX(200.0f);
    t.rotateY(-135.0f);
    t.translate(5.6f, 0.0f, 0.0f);

    // `Placement` is an origin and three axes, which is what this transform is
    // -- so the arm goes through the same `buildBox` a boat does, including its
    // page clamp and the tenth-of-a-texel inset `ll` applies. The axes carry
    // `render`'s own 0.0625, because a `Placement`'s axes are blocks per model
    // unit.
    Placement place;
    float ox, oy, oz;
    t.apply(0.0f, 0.0f, 0.0f, &ox, &oy, &oz);
    place.x = double(ox);
    place.y = double(oy);
    place.z = double(oz);
    const float unit[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    float* axis[3] = {place.ax, place.ay, place.az};
    for (int a = 0; a < 3; ++a) {
        float px, py, pz;
        t.apply(unit[a][0], unit[a][1], unit[a][2], &px, &py, &pz);
        axis[a][0] = (px - ox) * kModelUnit;
        axis[a][1] = (py - oy) * kModelUnit;
        axis[a][2] = (pz - oz) * kModelUnit;
    }

    // **The wide arm, and this version has no other.**
    //
    // The narrow body arrived with the 64 x 64 skin format, and both are 1.8's.
    // `core/texture/skin_list.hpp` still *detects* a narrow skin, because a
    // player who drops one in `skins/` deserves to be told which it is rather
    // than left wondering why the sleeve looks a texel wide -- but detecting is
    // as far as it goes here, and the flag below is the gate.
    //
    // **The assert is deliberate and it is not a placeholder.** A manifest that
    // turns `hasSlimSkins` on has to arrive with the narrow arm's box derived
    // from *that* version's `ModelBiped`, the way every other number in this
    // file was derived from a1.1.2's. Failing the build is the honest outcome
    // of flipping the flag without doing that; shipping a remembered
    // `addBox(-2, -2, -2, 3, 12, 4)` would not be.
    static_assert(!mcver::kHasSlimSkins,
                  "the narrow arm's ModelBiped box has not been derived -- open the jar of "
                  "the version that added it before turning hasSlimSkins on");

    ModelPart arm;
    arm.x = -3.0f;
    arm.y = -2.0f;
    arm.z = -2.0f;
    arm.w = 4;
    arm.h = 12;
    arm.d = 4;
    arm.texU = 40;
    arm.texV = 16;
    arm.pivotX = -5.0f;
    arm.pivotY = 2.0f;
    arm.pivotZ = 0.0f;
    // The idle sway, and the only non-zero angle at rest. See the note above.
    arm.angleZ = 0.1f;
    return buildBox(arm, place, texture::EntitySkin::Player, light, out, max);
}

}  // namespace

void HeldItemState::tick(item::ItemId inHand)
{
    // **`EntityLiving.onUpdate`'s first line and `EntityPlayer.b_` together**:
    // the previous value is kept before the counter moves, so the frame between
    // two ticks has something to interpolate across.
    prevSwing_ = swing_;
    if (swinging_) {
        ++swingTicks_;
        if (swingTicks_ == 8) {
            swingTicks_ = 0;
            swinging_ = false;
        }
    } else {
        swingTicks_ = 0;
    }
    swing_ = float(swingTicks_) / 8.0f;

    // `ItemRenderer.updateEquippedItem`. **The comparison is what makes this an
    // animation**: while the selected item differs from the one being drawn the
    // goal is zero, so the old item sinks; once it is nearly down the new one
    // is adopted and the goal flips to one and it rises. 0.4 a tick means five
    // ticks -- a quarter of a second -- for the whole swap.
    prevEquipped_ = equipped_;
    constexpr float kSpeed = 0.4f;
    const float goal = inHand == item_ ? 1.0f : 0.0f;
    float delta = goal - equipped_;
    if (delta < -kSpeed) {
        delta = -kSpeed;
    }
    if (delta > kSpeed) {
        delta = kSpeed;
    }
    equipped_ += delta;
    if (equipped_ < 0.1f) {
        item_ = inHand;
    }
}

void HeldItemState::swing()
{
    // `swingProgressInt = -1`, so the first tick of the swing lands on zero.
    swingTicks_ = -1;
    swinging_ = true;
}

float HeldItemState::equippedProgress(float partial) const
{
    return prevEquipped_ + (equipped_ - prevEquipped_) * partial;
}

float HeldItemState::swingProgress(float partial) const
{
    // **The wrap is the point.** The counter runs 0, 1/8 .. 7/8 and then back to
    // 0, so the last eighth of a swing interpolates 7/8 -> 1 rather than
    // 7/8 -> 0; without the `+ 1` the arm snaps back through the whole swing in
    // one frame.
    float f = swing_ - prevSwing_;
    if (f < 0.0f) {
        f += 1.0f;
    }
    return prevSwing_ + f * partial;
}

HeldItemMesh buildHeldItem(item::ItemId item, float equipped, float swing, float aspect,
                           u8 light, mesh::DetailVertex* out, int max)
{
    HeldItemMesh result;
    if (out == nullptr || max < 4) {
        return result;
    }
    const bool empty = item == 0;
    if (!empty && !item::def(item).known) {
        // The same call `drawItemIcon` makes: a stone block standing in for a
        // stranger is worse than an empty hand. **Nothing, and not the arm** --
        // the hand is not empty, this build just does not know what is in it,
        // and drawing a bare arm would say the opposite.
        return result;
    }

    // **What is being drawn, decided once.** An empty hand is the arm; an item
    // whose block `RenderBlocks.renderItemIn3d` accepts -- render types 0, 13,
    // 10 and 11 -- is that block, asked through the shape table rather than
    // through a second copy of the list, exactly as `item_entity_mesh.cpp` asks
    // it; everything else is the flat icon.
    AABB boxes[block::kMaxRenderBoxes];
    int faceMask[block::kMaxRenderBoxes];
    u16 block = 0;
    int boxCount = 0;
    int icon = 0;
    if (!empty) {
        const item::ItemDef& def = item::def(item);
        icon = int(def.icon);
        block = blockOf(def, item);
        if (def.sheet == item::IconSheet::Terrain && block != 0) {
            boxCount = block::itemRenderBoxes(block::BlockId(block), boxes,
                                              block::kMaxRenderBoxes, faceMask);
        }
    }
    const bool asBlock = boxCount > 0;
    result.sheet = empty        ? HeldSheet::PlayerSkin
                   : asBlock    ? HeldSheet::Terrain
                   : item < 256 ? HeldSheet::Terrain
                                : HeldSheet::Items;

    Transform t;

    // **Ours, and the only thing here that is not in the class file.** See the
    // header: the original's constants frame the hand for 4:3, and the top
    // screen is 5:3. `kHandRight` below is the x the item is placed at, so
    // moving it out by the same proportion the wider half-extent divides by
    // puts it back where a1.1.2 draws it.
    constexpr float kEquipScale = 0.8f;   // `f5`, and the same in both branches
    // `0.7F * f5` with an item and `0.8F * f5` without one.
    const float handRight = (empty ? 0.8f : 0.7f) * kEquipScale;
    if (aspect > 0.0f) {
        t.translate(handRight * (aspect / kOriginalAspect - 1.0f), 0.0f, 0.0f);
    }

    // `renderItemInFirstPerson`. The two `glRotatef` calls above these in the
    // method are inside their own push/pop and only orient the lighting, so
    // they are not part of either branch.
    //
    // **The two branches share their shape and none of their numbers.** The
    // swing offset is 0.3/0.4/0.4 for the arm against 0.4/0.2/0.2 for an item,
    // and the rest position is -0.75 against -0.65. Written out rather than
    // parameterised down to one line, because that is how the class file has
    // them and a reader checking one against the bytecode should not have to
    // unpick the other.
    const float bobSin = MathHelper::sin(swing * kPi);
    const float bobRoot = MathHelper::sin(std::sqrt(swing) * kPi);
    const float sway = MathHelper::sin(std::sqrt(swing) * kPi * 2.0f);
    if (empty) {
        t.translate(-bobRoot * 0.3f, sway * 0.4f, -bobSin * 0.4f);
        t.translate(handRight, -0.75f * kEquipScale - (1.0f - equipped) * 0.6f,
                    -0.9f * kEquipScale);
    } else {
        t.translate(-bobRoot * 0.4f, sway * 0.2f, -bobSin * 0.2f);
        t.translate(handRight, -0.65f * kEquipScale - (1.0f - equipped) * 0.6f,
                    -0.9f * kEquipScale);
    }
    t.rotateY(45.0f);

    // **The swing is read twice and the second reading is not the first.**
    // `f7` is `sin(swing * swing * PI)` here and `sin(swing * PI)` above, which
    // is what makes the hand dip and turn on different curves.
    const float turn = MathHelper::sin(swing * swing * kPi);
    const float lift = MathHelper::sin(std::sqrt(swing) * kPi);
    if (empty) {
        // **Plus seventy about Y, and no X rotation at all.** The arm swings
        // across the view where the item swings down through it.
        t.rotateY(lift * 70.0f);
        t.rotateZ(-turn * 20.0f);
        return HeldItemMesh{addArm(t, light, out, max), HeldSheet::PlayerSkin};
    }
    t.rotateY(-turn * 20.0f);
    t.rotateZ(-lift * 20.0f);
    t.rotateX(-lift * 80.0f);
    t.scale(0.4f);

    if (asBlock) {
        // `renderBlockOnInventory`'s own recentring: the faces are emitted in
        // block-local 0..1 and the whole block is moved back by half on each
        // axis, so it turns about its middle rather than about its corner.
        t.translate(-0.5f, -0.5f, -0.5f);
        const block::BlockDef& bd = block::def(block::BlockId(block));
        int written = 0;
        for (int b = 0; b < boxCount; ++b) {
            const int room = max - written;
            if (room < boxVertices(faceMask[b])) {
                break;
            }
            written += addBox(boxes[b], t, bd.faces, faceMask[b], light, out + written, room);
        }
        result.vertices = written;
        return result;
    }

    result.vertices = addSprite(icon, t, light, out, max);
    return result;
}

}  // namespace mc::render
