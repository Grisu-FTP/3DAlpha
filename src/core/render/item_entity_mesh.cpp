// See item_entity_mesh.hpp.

#include "core/render/item_entity_mesh.hpp"

#include "core/block/collision.hpp"
#include "core/block/model.hpp"
#include "core/block/registry.hpp"
#include "core/item/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/render/draw_budget.hpp"
#include "core/render/entity_range.hpp"
#include "core/util/java_random.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

// The detail position is a signed short of 1/1024 blocks, so a little under 32
// blocks either way of the origin -- and a stack is never drawn that far:
// `kh.a(D)Z` of its quarter-block box is 16 blocks. See
// core/render/entity_range.hpp.
constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = kDetailPlacementLimit;

constexpr float kPi = 3.1415927f;
// `57.295776F` -- the class file's own literal for radians to degrees, and it
// is not `180 / M_PI` to the last bit.
constexpr float kRadiansToDegrees = 57.295776f;

// **Full brightness on all three channels.** `RenderItem` never calls
// `setColorOpaque_F`, so the item is drawn at the colour the vertex format
// carries by default -- unlike a digging particle, which is deliberately
// darkened. The light byte still does its work.
constexpr u8 kItemShade = 255;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

// One tile of whichever 16x16 sheet, with the same eighth-of-a-texel inset the
// world uses -- an item entity samples the same atlas as a block face, and the
// PICA's boundary problem does not care which quad it is.
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

// The block an item puts into the world, or the block it *is*. The same rule
// `gui::item_icon.cpp` follows, and for the same reason: item 324 is a door and
// block 64 is the door it leaves.
u16 blockOf(const item::ItemDef& def, item::ItemId id)
{
    if (def.places != 0) {
        return def.places;
    }
    return def.sheet == item::IconSheet::Terrain ? u16(id) : 0;
}

// `RenderBlocks.renderItemIn3d`, through the shape table rather than through a
// second copy of the render-type list: `block::itemRenderBoxes` answers with
// boxes for exactly the four types that method returns true for, and zero for
// the rest.
//
// **The item form and not the world form**, which is `renderBlockAsItem`'s own
// first line -- `setBlockBoundsForItemRender` -- and is the difference between a
// dropped button and a dropped stone cube. See core/block/model.hpp.
int itemBoxes(u16 block, AABB* out, int* faceMask, int max)
{
    if (block == 0) {
        return 0;
    }
    return block::itemRenderBoxes(block::BlockId(block), out, max, faceMask);
}

// Four vertices for every face the mask keeps. Only the cactus drops any --
// its three boxes contribute two faces each -- but the budget has to be the
// number actually written, not the number a solid box would write, or a heap
// of dropped cactus reserves three times the buffer it uses.
int boxVertices(const int* faceMask, int count)
{
    int quads = 0;
    for (int box = 0; box < count; ++box) {
        for (int face = 0; face < 6; ++face) {
            if ((faceMask[box] & (1 << face)) != 0) {
                ++quads;
            }
        }
    }
    return quads * 4;
}

// A vertex, written once. Kept as a function because both branches write the
// same seven fields and getting one of them wrong in only one place is the
// bug this shape avoids.
void writeVertex(mesh::DetailVertex& v, double x, double y, double z, i16 u, i16 vv, u8 light)
{
    v.x = toUnits(x);
    v.y = toUnits(y);
    v.z = toUnits(z);
    v.face = 0;
    v.u = u;
    v.v = vv;
    v.r = kItemShade;
    v.g = kItemShade;
    v.b = kItemShade;
    v.light = light;
}

// One axis-aligned box, scaled about its own centre and then spun about the
// vertical axis -- which is the whole of what `glRotatef(spin, 0, 1, 0)` and
// `glScalef(s, s, s)` do to `renderBlockAsItem`'s output.
//
// The box comes in block-local (0..1) and is re-centred on the origin first,
// because a1.1.2's item transform is applied about the entity's position and
// `renderBlockAsItem` draws the block centred on it.
int addSpunBox(const AABB& box, double cx, double cy, double cz, float scale, float sinSpin,
               float cosSpin, const u16 tiles[6], int faceMask, u8 light,
               mesh::DetailVertex* out, int max)
{
    if (max < boxVertices(&faceMask, 1)) {
        return 0;
    }

    // The eight corners, centred and scaled. The shift is by half a block on
    // each axis, so a full cube spans -0.5..0.5 before the scale.
    double px[8], py[8], pz[8];
    const double lo[3] = {box.minX - 0.5, box.minY - 0.5, box.minZ - 0.5};
    const double hi[3] = {box.maxX - 0.5, box.maxY - 0.5, box.maxZ - 0.5};
    for (int i = 0; i < 8; ++i) {
        const double bx = ((i & 1) != 0 ? hi[0] : lo[0]) * double(scale);
        const double by = ((i & 2) != 0 ? hi[1] : lo[1]) * double(scale);
        const double bz = ((i & 4) != 0 ? hi[2] : lo[2]) * double(scale);
        px[i] = cx + bx * double(cosSpin) + bz * double(sinSpin);
        py[i] = cy + by;
        pz[i] = cz + bz * double(cosSpin) - bx * double(sinSpin);
    }

    // Corner indices per face, in `mc::mesh::Face` order. **These are
    // `mesh::kFaceCorner` rewritten as indices into the eight** -- an index is
    // `x + 2y + 4z` with each bit meaning "the high edge on that axis" -- so the
    // winding is the one that table already argues for, and a rotation about Y
    // preserves it.
    static constexpr int kFace[6][4] = {
        {0, 1, 5, 4},  // -Y
        {6, 7, 3, 2},  // +Y
        {1, 0, 2, 3},  // -Z
        {4, 5, 7, 6},  // +Z
        {0, 4, 6, 2},  // -X
        {5, 1, 3, 7},  // +X
    };

    int written = 0;
    for (int face = 0; face < 6; ++face) {
        if ((faceMask & (1 << face)) == 0) {
            // A cactus, whose sides and caps are three boxes of the same cell.
            continue;
        }
        // The box's own slice of the tile, not the whole of it -- see
        // `mesh::boxTileUv`. A fence on the ground is a post, and a post shows
        // the strip of the plank tile it covers.
        const mesh::BoxTileUv uv = mesh::boxTileUv(int(tiles[face]), box, face);
        for (int c = 0; c < 4; ++c) {
            const int idx = kFace[face][c];
            // `mesh::kFaceCornerUV`, which is the same four pairs for every
            // face **except the bottom** -- and that exception is the reason
            // this reads the table rather than carrying one pattern.
            const i16 u = mesh::kFaceCornerUV[face][c][0] != 0 ? uv.u1 : uv.u0;
            const i16 v = mesh::kFaceCornerUV[face][c][1] != 0 ? uv.v1 : uv.v0;
            writeVertex(out[written], px[idx], py[idx], pz[idx], u, v, light);
            ++written;
        }
    }
    return written;
}

}  // namespace

int itemCopies(int stackSize)
{
    // The original's three thresholds, in its order.
    int copies = 1;
    if (stackSize > 1) copies = 2;
    if (stackSize > 5) copies = 3;
    if (stackSize > 20) copies = 4;
    return copies;
}

int buildItemEntities(const entity::ItemEntitySystem& system, float viewYawDegrees,
                      double originX, double originY, double originZ, float partial,
                      item::IconSheet sheet, mesh::DetailVertex* out, int max,
                      DrawCutoff* shared)
{
    if (out == nullptr || max < 4) {
        return 0;
    }

    // Whether an item is drawn at all -- by either sheet's pass -- and where,
    // before the bob. Shared by the nearest-first count and the build.
    const auto place = [&](int i, double* px, double* py, double* pz) {
        const entity::ItemEntity& e = system[i];
        if (e.item == 0 || !item::def(e.item).known) {
            return false;
        }
        *px = e.prevX + (e.x - e.prevX) * double(partial) - originX;
        *pz = e.prevZ + (e.z - e.prevZ) * double(partial) - originZ;
        *py = e.prevY + (e.y - e.prevY) * double(partial) - originY;
        return entityInDrawRange(*px, *py, *pz, entity::kItemSize, entity::kItemSize, kLimit);
    };
    // At most what the build below writes for it: every copy, and every box of
    // a block or one quad of a sprite.
    const auto cost = [&](int i) {
        const entity::ItemEntity& e = system[i];
        const item::ItemDef& def = item::def(e.item);
        AABB boxes[block::kMaxRenderBoxes];
        int faceMask[block::kMaxRenderBoxes];
        const int boxCount =
            def.sheet == item::IconSheet::Terrain
                ? itemBoxes(blockOf(def, e.item), boxes, faceMask, block::kMaxRenderBoxes)
                : 0;
        return itemCopies(e.count)
               * (boxCount > 0 ? boxVertices(faceMask, boxCount) : 4);
    };
    // **One cutoff for both sheets.** The first pass settles it over every
    // item against the whole buffer, and the second charges its own items to
    // what is left of the same edge -- the two sets are disjoint, so between
    // them they take exactly the budget. Worst case per item is four copies of
    // the most boxes a block has, which is the fast test. See draw_budget.hpp.
    DrawCutoff local;
    DrawCutoff& cutoff = shared != nullptr ? *shared : local;
    if (!cutoff.computed()) {
        if (system.count() * 4 * block::kMaxRenderBoxes * 6 * 4 > max) {
            cutoff.compute(system.count(), max, place, cost);
        } else {
            cutoff.drawAll();
        }
    }

    // The sprite's facing, worked out once: `glRotatef(180 - playerViewY, 0, 1, 0)`
    // is the same turn for every item on screen.
    const float faceYaw = (180.0f - viewYawDegrees) / 180.0f * kPi;
    const float faceSin = MathHelper::sin(faceYaw);
    const float faceCos = MathHelper::cos(faceYaw);

    int written = 0;
    for (int i = 0; i < system.count(); ++i) {
        const entity::ItemEntity& e = system[i];
        if (e.item == 0) {
            continue;
        }
        const item::ItemDef& def = item::def(e.item);
        if (!def.known) {
            // A music disc, or an id from a world this build does not know.
            // Nothing is drawn, for the reason `drawItemIcon` gives: a stone
            // block standing in for a stranger is worse than a gap.
            continue;
        }

        const u16 block = blockOf(def, e.item);
        AABB boxes[block::kMaxRenderBoxes];
        int faceMask[block::kMaxRenderBoxes];
        const int boxCount =
            def.sheet == item::IconSheet::Terrain
                ? itemBoxes(block, boxes, faceMask, block::kMaxRenderBoxes)
                : 0;
        const bool asBlock = boxCount > 0;
        const int blockVertices = asBlock ? boxVertices(faceMask, boxCount) : 0;

        // Which sheet this entity draws from, and therefore which pass owns it.
        const item::IconSheet mine =
            asBlock ? item::IconSheet::Terrain
                    : (e.item < 256 ? item::IconSheet::Terrain : item::IconSheet::Items);
        if (mine != sheet) {
            continue;
        }

        double px = 0.0;
        double py = 0.0;
        double pz = 0.0;
        if (!place(i, &px, &py, &pz)
            || !cutoff.admit(px, py, pz,
                             itemCopies(e.count) * (asBlock ? blockVertices : 4))) {
            continue;
        }

        // The bob and the spin, both off `age + partial` so they run smoothly
        // between ticks rather than stepping at 20 Hz.
        const float animation = float(e.age) + partial;
        py += double(MathHelper::sin(animation / 10.0f + e.hoverPhase) * 0.1f + 0.1f);
        const float spinDegrees = (animation / 20.0f + e.hoverPhase) * kRadiansToDegrees;
        const float spin = spinDegrees / 180.0f * kPi;
        const float spinSin = MathHelper::sin(spin);
        const float spinCos = MathHelper::cos(spin);

        // **Seeded per entity, and 187 is the original's literal.** It is what
        // makes a pile of four look the same every frame instead of shimmering.
        JavaRandom jitter(kItemJitterSeed);
        const int copies = itemCopies(e.count);

        for (int copy = 0; copy < copies; ++copy) {
            double ox = 0.0;
            double oy = 0.0;
            double oz = 0.0;
            if (copy > 0) {
                // **The offset in world units, which is not the literal in the
                // class file.** Both branches translate *inside* their scale, so
                // what reaches the world is the literal times the scale -- and
                // the block branch pre-divides by its own scale to cancel that,
                // which the sprite branch does not. A block copy therefore
                // moves `0.2 / 0.25 * 0.25` = 0.2 of a block and a sprite copy
                // `0.3 * 0.5` = 0.15.
                const float spread = asBlock ? 0.2f : 0.3f * kItemSpriteScale;
                ox = double((jitter.nextFloat() * 2.0f - 1.0f) * spread);
                oy = double((jitter.nextFloat() * 2.0f - 1.0f) * spread);
                oz = double((jitter.nextFloat() * 2.0f - 1.0f) * spread);
            }

            if (asBlock) {
                const block::BlockDef& bd = block::def(block::BlockId(block));
                for (int b = 0; b < boxCount; ++b) {
                    if (written + boxVertices(&faceMask[b], 1) > max) {
                        return written;
                    }
                    written += addSpunBox(boxes[b], px + ox, py + oy, pz + oz,
                                          kItemBlockScale, spinSin, spinCos, bd.faces,
                                          faceMask[b], e.light, out + written,
                                          max - written);
                }
                continue;
            }

            // **One quad, and it needs no mirror.** It is billboarded on yaw
            // only -- `glRotatef(180 - playerViewY, 0, 1, 0)`, no pitch term --
            // so it always faces the camera and can never present its back;
            // and the pass it is drawn in has culling off anyway, which is the
            // arrangement the particle billboards already rely on.
            if (written + 4 > max) {
                return written;
            }

            // **The quad is not centred on the entity.** `f20/f21/f22` are
            // 1.0, 0.5 and 0.25, so the unit quad spans x -0.5..0.5 and
            // y -0.25..0.75 before `glScalef(0.5)` -- an item sits with a
            // quarter of itself below its own position and three quarters
            // above. That is what makes a dropped sword look like it is lying
            // on the ground rather than buried to the hilt.
            const float low = -0.25f * kItemSpriteScale;
            const float high = 0.75f * kItemSpriteScale;
            const float half = 0.5f * kItemSpriteScale;
            const TileUv uv = tileUv(int(def.icon));
            const double rx = double(faceCos * half);
            const double rz = double(-faceSin * half);

            const double cornerX[4] = {px + ox - rx, px + ox + rx, px + ox + rx,
                                       px + ox - rx};
            const double cornerY[4] = {py + oy + double(low), py + oy + double(low),
                                       py + oy + double(high), py + oy + double(high)};
            const double cornerZ[4] = {pz + oz - rz, pz + oz + rz, pz + oz + rz,
                                       pz + oz - rz};
            const i16 cornerU[4] = {uv.u0, uv.u1, uv.u1, uv.u0};
            const i16 cornerV[4] = {uv.v1, uv.v1, uv.v0, uv.v0};

            for (int c = 0; c < 4; ++c) {
                writeVertex(out[written], cornerX[c], cornerY[c], cornerZ[c], cornerU[c],
                            cornerV[c], e.light);
                ++written;
            }
        }
    }
    return written;
}

}  // namespace mc::render
