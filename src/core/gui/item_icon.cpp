#include "core/gui/item_icon.hpp"

#include "core/block/model.hpp"
#include "core/block/registry.hpp"
#include "core/item/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/texture/atlas_image.hpp"

namespace mc::gui {

namespace {

constexpr int kTiles = texture::kAtlasTilesPerEdge;
constexpr int kTilePixels = texture::kAtlasTilePixels;
constexpr int kSheetEdge = texture::kAtlasEdge;

// Below this the texel is a hole rather than a dark pixel. The same cutoff the
// world's detail pass uses: these sheets carry cutouts, not blends, and the
// bottom screen has no alpha to blend into.
constexpr u8 kAlphaCutoff = 128;

// One texel of a tile, in tile-local coordinates. Answers false for a texel
// that is not there to draw -- outside the sheet, or transparent.
bool sampleTile(const u8* sheet, int tile, int tu, int tv, u8* r, u8* g, u8* b)
{
    if (sheet == nullptr || tile < 0 || tile >= kTiles * kTiles) {
        return false;
    }
    if (tu < 0 || tv < 0 || tu >= kTilePixels || tv >= kTilePixels) {
        return false;
    }
    const int sx = (tile % kTiles) * kTilePixels + tu;
    const int sy = (tile / kTiles) * kTilePixels + tv;
    const u8* texel = sheet + (usize(sy) * kSheetEdge + usize(sx)) * 4;
    if (texel[3] < kAlphaCutoff) {
        return false;
    }
    *r = texel[0];
    *g = texel[1];
    *b = texel[2];
    return true;
}

u8 shade(u8 value, float factor)
{
    const int scaled = int(float(value) * factor + 0.5f);
    return u8(scaled > 255 ? 255 : scaled);
}

// **The 2:1 projection, as the affine map it is.** A block-local point goes to
// icon-local screen coordinates, both in units of the icon's size:
//
//     sx = 0.5 + (x + z - 1) * 0.5
//     sy = 0.25 + (x - z) * 0.25 + (1 - y) * 0.5
//
// Every coefficient is a quarter or a half, which is what keeps a unit cube's
// silhouette on pixel boundaries at any even size. It puts the corner nearest
// the viewer -- (1, 1, 0) -- at the middle of the icon, and the whole unit cube
// inside the box exactly.
//
// **It replaces a table of three fixed parallelograms**, which could only ever
// draw a full cube. A fence is a post and a cactus is inset, and both were
// drawn as a solid block because the table had nowhere to put their bounds.
struct Screen {
    float x, y;
};

Screen project(float bx, float by, float bz)
{
    return Screen{0.5f + (bx + bz - 1.0f) * 0.5f,
                  0.25f + (bx - bz) * 0.25f + (1.0f - by) * 0.5f};
}

// Which of a box's six faces are towards the viewer, and which of the block's
// tiles and shades each takes. Only three can be seen at once, and they are
// always the same three: the viewer is above, to the +X side and on the -Z
// side, so the light face is the north one at 0.8 and the dark face is the east
// one at 0.6 -- exactly what the world mesher gives them.
struct IconFace {
    int meshFace;
    // The box corner the tile's (0, 0) sits on, and the two box-space edges its
    // u and v run along, each as a mask over (minX/maxX, minY/maxY, minZ/maxZ).
    int originX, originY, originZ;  // 0 = min, 1 = max
    int uAxis, uTowards;            // axis 0/1/2, and which end it runs to
    int vAxis, vTowards;
};

// Written to match `core/mesh/box.hpp`'s UV rule, which is the original's: the
// top face takes u from x and v from z, and a side face takes v from y with the
// box's *top* edge on the low end of the tile.
constexpr IconFace kVisibleFaces[3] = {
    // Top: origin at (minX, maxY, minZ), u to maxX, v to maxZ.
    {mesh::kFacePosY, 0, 1, 0, 0, 1, 2, 1},
    // North (-Z): origin at (maxX, maxY, minZ), u to minX, v down to minY.
    {mesh::kFaceNegZ, 1, 1, 0, 0, 0, 1, 0},
    // East (+X): origin at (maxX, maxY, maxZ), u to minZ, v down to minY.
    {mesh::kFacePosX, 1, 1, 1, 2, 0, 1, 0},
};

float boxAxis(const AABB& box, int axis, int towards)
{
    switch (axis) {
        case 0: return float(towards != 0 ? box.maxX : box.minX);
        case 1: return float(towards != 0 ? box.maxY : box.minY);
        default: return float(towards != 0 ? box.maxZ : box.minZ);
    }
}

Screen projectCorner(const AABB& box, int px, int py, int pz)
{
    return project(boxAxis(box, 0, px), boxAxis(box, 1, py), boxAxis(box, 2, pz));
}

// The block an item puts down, which for an ItemBlock is the item's own id.
// Zero for an item that places nothing.
u16 blockOf(const item::ItemDef& def, item::ItemId id)
{
    if (def.places != 0) {
        return def.places;
    }
    return def.sheet == item::IconSheet::Terrain ? u16(id) : 0;
}

// One texel of a standalone 16 x 16 tile -- the same rule as `sampleTile`, over
// a buffer that is one tile rather than a 16 x 16 grid of them.
bool sampleLooseTile(const u8* tile, int tu, int tv, u8* r, u8* g, u8* b)
{
    if (tile == nullptr || tu < 0 || tv < 0 || tu >= kTilePixels || tv >= kTilePixels) {
        return false;
    }
    const u8* texel = tile + (usize(tv) * kTilePixels + usize(tu)) * 4;
    if (texel[3] < kAlphaCutoff) {
        return false;
    }
    *r = texel[0];
    *g = texel[1];
    *b = texel[2];
    return true;
}

// `liveTile` stands in for the sheet's own `tile` when it is not null -- see
// `IconSheets::animatedItems`. It changes where a texel is read from and
// nothing else: the same nearest-neighbour scale, the same alpha cutoff.
void drawFlat(const Surface& surface, int x, int y, int size, const u8* sheet, int tile,
              const u8* liveTile = nullptr)
{
    if (sheet == nullptr && liveTile == nullptr) {
        return;
    }
    for (int py = 0; py < size; ++py) {
        // Nearest neighbour, because a slot is sixteen or twenty pixels and an
        // averaged one at that size is mush. The world's atlas has already been
        // filtered down from whatever the pack shipped.
        const int tv = (py * kTilePixels) / size;
        for (int px = 0; px < size; ++px) {
            const int tu = (px * kTilePixels) / size;
            u8 r, g, b;
            const bool got = liveTile != nullptr
                                 ? sampleLooseTile(liveTile, tu, tv, &r, &g, &b)
                                 : sampleTile(sheet, tile, tu, tv, &r, &g, &b);
            if (!got) {
                continue;
            }
            Pixel* out = surface.at(x + px, y + py);
            if (out != nullptr) {
                *out = rgb565(int(r), int(g), int(b));
            }
        }
    }
}

// One box of a block's shape, as three parallelograms.
//
// Each face is inverse-mapped: a destination pixel is asked which point of the
// face it is, and a point outside 0..1 in either direction is not on it. That
// is one divide per face set up once and two multiplies per pixel, which at
// sixteen pixels square is nothing and needs no clipping code.
void drawBox(const Surface& surface, int x, int y, int size, const u8* sheet,
             const block::BlockDef& def, const AABB& box)
{
    const float edge = float(size);
    for (const IconFace& face : kVisibleFaces) {
        const Screen origin = projectCorner(box, face.originX, face.originY, face.originZ);

        // The two screen-space edges, found by moving the origin corner along
        // each texture axis to the other end of the box.
        int px[3] = {face.originX, face.originY, face.originZ};
        px[face.uAxis] = face.uTowards;
        const Screen alongU = projectCorner(box, px[0], px[1], px[2]);
        int qx[3] = {face.originX, face.originY, face.originZ};
        qx[face.vAxis] = face.vTowards;
        const Screen alongV = projectCorner(box, qx[0], qx[1], qx[2]);

        const float ux = alongU.x - origin.x;
        const float uy = alongU.y - origin.y;
        const float vx = alongV.x - origin.x;
        const float vy = alongV.y - origin.y;

        const float determinant = ux * vy - uy * vx;
        if (determinant == 0.0f) {
            // A box with no thickness along one of this face's axes -- a flat
            // plate seen edge on. Nothing to draw and nothing to divide by.
            continue;
        }
        const float inv = 1.0f / determinant;

        // **The tile is sampled over the box's own extent**, which is the
        // same rule core/mesh/box.hpp follows in the world: a slab's side shows
        // half its tile and a fence post shows the quarter of the plank texture
        // it covers.
        //
        // It comes out as one line per axis, and pleasantly so. Each face's
        // parameter runs from the origin corner towards the other end, and the
        // faces whose origin is at the *high* end are exactly the faces whose
        // texture axis runs backwards -- the two cancel, so the texel is just
        // the box's low bound plus the parameter across its extent, whichever
        // face it is.
        const float uMin = boxAxis(box, face.uAxis, 0);
        const float uSpan = boxAxis(box, face.uAxis, 1) - uMin;
        const float vMin = boxAxis(box, face.vAxis, 0);
        const float vSpan = boxAxis(box, face.vAxis, 1) - vMin;

        const float shadeFactor = mesh::kFaceShadeFloat[face.meshFace];
        const int tileIndex = int(def.faces[face.meshFace]);
        const float tile = float(kTilePixels);

        for (int py = 0; py < size; ++py) {
            for (int pxi = 0; pxi < size; ++pxi) {
                // Pixel centres, so a face's edge falls between two pixels
                // rather than on one and the three faces tile without a seam.
                const float dx = (float(pxi) + 0.5f) / edge - origin.x;
                const float dy = (float(py) + 0.5f) / edge - origin.y;
                const float a = (dx * vy - dy * vx) * inv;
                const float b = (dy * ux - dx * uy) * inv;
                if (a < 0.0f || a >= 1.0f || b < 0.0f || b >= 1.0f) {
                    continue;
                }

                const float uTexel = uMin + a * uSpan;
                const float vTexel = vMin + b * vSpan;

                u8 r, g, bl;
                if (!sampleTile(sheet, tileIndex, int(uTexel * tile), int(vTexel * tile), &r, &g,
                                &bl)) {
                    continue;
                }
                Pixel* out = surface.at(x + pxi, y + py);
                if (out != nullptr) {
                    *out = rgb565(int(shade(r, shadeFactor)), int(shade(g, shadeFactor)),
                                  int(shade(bl, shadeFactor)));
                }
            }
        }
    }
}

// Every box of the block, drawn back to front.
//
// **Painter's order, and it is one comparison.** The viewer is towards +X and
// -Z, so a box is nearer the eye the larger its x and the smaller its z; drawing
// in increasing `x - z` puts the far ones down first and lets the near ones
// cover them. A fence's post and its rails are the case that needs it.
void drawShape(const Surface& surface, int x, int y, int size, const u8* sheet,
               const block::BlockDef& def, block::BlockId id)
{
    AABB boxes[block::kMaxRenderBoxes];
    // No neighbours: an icon is a block on its own, so a fence is a bare post.
    const int count = block::renderBoxes(id, 0, 0, boxes, block::kMaxRenderBoxes);

    int order[block::kMaxRenderBoxes];
    for (int i = 0; i < count; ++i) {
        order[i] = i;
    }
    for (int i = 1; i < count; ++i) {
        const int held = order[i];
        const double key = boxes[held].minX - boxes[held].maxZ;
        int j = i - 1;
        while (j >= 0 && (boxes[order[j]].minX - boxes[order[j]].maxZ) > key) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = held;
    }

    for (int i = 0; i < count; ++i) {
        drawBox(surface, x, y, size, sheet, def, boxes[order[i]]);
    }
}

}  // namespace

bool itemDrawsAsCube(item::ItemId id)
{
    if (id == 0) {
        return false;
    }
    const item::ItemDef& def = item::def(id);
    if (!def.known || def.sheet != item::IconSheet::Terrain) {
        return false;
    }
    const u16 block = blockOf(def, id);
    if (block == 0) {
        return false;
    }
    // **The original's own list, read out of the jar rather than guessed.**
    // `RenderBlocks.renderItemIn3d` is a static `(I)Z` and answers true for
    // render types 0, 10, 11 and 13 -- the standard block, stairs, the fence
    // and the cactus. Everything else, items included, is drawn as a flat
    // sprite.
    //
    // The fence being in that list is why a fence in the hand used to be a
    // plain square of planks: it was taking the flat path and showing its
    // `texture` column, which for a fence is the plank tile.
    switch (block::def(block::BlockId(block)).render) {
        case block::RenderType::Cube:
        case block::RenderType::Stairs:
        case block::RenderType::Fence:
        case block::RenderType::Cactus:
            return true;
        default:
            return false;
    }
}

void drawItemIcon(const Surface& surface, int x, int y, int size, const IconSheets& sheets,
                  item::ItemId id)
{
    if (id == 0 || size <= 0) {
        return;
    }
    const item::ItemDef& def = item::def(id);
    if (!def.known) {
        // A music disc, or an id from a world this build does not know. Drawing
        // nothing leaves an empty slot, which is readable; drawing tile 0 would
        // put a stone block where a stranger is.
        return;
    }

    if (itemDrawsAsCube(id)) {
        drawShape(surface, x, y, size, sheets.terrain,
                  block::def(block::BlockId(blockOf(def, id))),
                  block::BlockId(blockOf(def, id)));
        return;
    }

    if (def.sheet == item::IconSheet::Terrain) {
        drawFlat(surface, x, y, size, sheets.terrain, int(def.icon));
        return;
    }

    if (sheets.items != nullptr) {
        // The live face when this is the tile something is animating, and the
        // pack's own otherwise. `animatedItemsTile` is -1 when nothing is.
        const u8* live = (sheets.animatedItems != nullptr
                          && sheets.animatedItemsTile == int(def.icon))
                             ? sheets.animatedItems
                             : nullptr;
        drawFlat(surface, x, y, size, sheets.items, int(def.icon), live);
        return;
    }

    // **No items.png, so fall back to the block it places.** A pack that
    // carries only a terrain.png is a pack, not a broken one -- and this is
    // what every slot in this project drew before there was a second sheet.
    const u16 block = blockOf(def, id);
    if (block != 0) {
        drawFlat(surface, x, y, size, sheets.terrain, int(block::def(block::BlockId(block)).texture));
    }
}

}  // namespace mc::gui
