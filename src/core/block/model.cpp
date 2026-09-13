#include "core/block/model.hpp"

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"

namespace mc::block {

namespace {

// `bc.m`'s own `setBounds` calls: the post is 6/16 to 10/16 across and full
// height, and each rail is 7/16 to 9/16 thick at 12/16..15/16 and 6/16..9/16.
constexpr double kPostLo = 0.375;
constexpr double kPostHi = 0.625;
constexpr double kRailLo = 0.4375;
constexpr double kRailHi = 0.5625;
constexpr double kUpperBottom = 0.75;
constexpr double kUpperTop = 0.9375;
constexpr double kLowerBottom = 0.375;
constexpr double kLowerTop = 0.5625;

int fenceBoxes(int connections, AABB* out, int max)
{
    int count = 0;
    if (count < max) {
        out[count++] = AABB{kPostLo, 0.0, kPostLo, kPostHi, 1.0, kPostHi};
    }

    const int bits[4] = {kConnectNegX, kConnectPosX, kConnectNegZ, kConnectPosZ};
    for (int side = 0; side < 4; ++side) {
        if ((connections & bits[side]) == 0) {
            continue;
        }
        for (int level = 0; level < 2 && count < max; ++level) {
            const double lo = level == 0 ? kLowerBottom : kUpperBottom;
            const double hi = level == 0 ? kLowerTop : kUpperTop;
            AABB rail{kRailLo, lo, kRailLo, kRailHi, hi, kRailHi};
            switch (bits[side]) {
                case kConnectNegX:
                    rail.minX = 0.0;
                    rail.maxX = kPostLo;
                    break;
                case kConnectPosX:
                    rail.minX = kPostHi;
                    rail.maxX = 1.0;
                    break;
                case kConnectNegZ:
                    rail.minZ = 0.0;
                    rail.maxZ = kPostLo;
                    break;
                default:
                    rail.minZ = kPostHi;
                    rail.maxZ = 1.0;
                    break;
            }
            out[count++] = rail;
        }
    }
    return count;
}

// `bc.a(Lly;)V`'s render-type-13 branch -- renderBlockAsItem, which is what
// draws a cactus in a slot, in the hand and lying on the ground, and it is
// **not** the inset box this used to hand back.
//
// The method resets the bounds to the whole cell (`ly.e()`, setBlockBounds-
// ForItemRender), draws the top and the bottom there, and then draws each of
// the four sides as a full-cell face under `Tessellator.addTranslation(±f)`
// with `f = 0.0625F`:
//
//     float f = 0.0625F;
//     renderBottomFace(block, 0, 0, 0, tile(0));   // no translation
//     renderTopFace   (block, 0, 0, 0, tile(1));   // no translation
//     addTranslation(0, 0,  f); renderEastFace (...); addTranslation(0, 0, -f);
//     ...
//
// So a cactus item is a **full-size cube whose four sides are pushed in a
// sixteenth**, exactly the shape `mesh::addCactus` draws in the world -- and
// the spikes are the same thing in both places: the side tile's outer texel
// columns, which hang past the cell and survive because the alpha test cuts
// the transparent rest of them away.
//
// **Narrowing the box instead crops those columns off**, because a face is
// textured over its own extent everywhere in this project. That is what put a
// visible gap round a dropped cactus: the sides were drawn at 1/16..15/16 with
// the tile's transparent edges mapped onto them, so the block showed daylight
// between its own faces. The world mesher had this corrected already; the
// three item paths were still reading the collision box.
//
// The sixteenth comes from the collision box rather than from a literal of its
// own, which is where the world's copy of this shape gets it too: it is the
// one number the two share, and it is generated per version.
int cactusBoxes(BlockId id, u8 metadata, AABB* out, int* faceMask, int max)
{
    AABB collision[kMaxCollisionBoxes];
    if (collisionBoxes(id, metadata, collision, kMaxCollisionBoxes) == 0) {
        return 0;
    }
    const AABB& inset = collision[0];

    int count = 0;
    const auto push = [&](const AABB& box, int faces) {
        if (count >= max) {
            return;
        }
        out[count] = box;
        if (faceMask != nullptr) {
            faceMask[count] = faces;
        }
        ++count;
    };

    // The cell itself, for the two faces the original does not translate.
    push(AABB{0.0, 0.0, 0.0, 1.0, 1.0, 1.0}, kRenderFaceNegY | kRenderFacePosY);
    // Each pair of sides inset along its own axis only, so the face sits a
    // sixteenth in and still spans the cell the other way -- which is what the
    // translation does to a full-cell face.
    push(AABB{inset.minX, 0.0, 0.0, inset.maxX, 1.0, 1.0},
         kRenderFaceNegX | kRenderFacePosX);
    push(AABB{0.0, 0.0, inset.minZ, 1.0, 1.0, inset.maxZ},
         kRenderFaceNegZ | kRenderFacePosZ);
    return count;
}

}  // namespace

int renderBoxes(BlockId id, u8 metadata, int connections, AABB* out, int max,
                int* faceMask)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }

    // Every shape but the cactus draws whole boxes, so the mask is filled in
    // once here and the branches below only have to disagree with it.
    if (faceMask != nullptr) {
        for (int i = 0; i < max; ++i) {
            faceMask[i] = kAllRenderFaces;
        }
    }

    switch (def(id).render) {
        case RenderType::Cube:
            // The bounds the standard renderer draws, which the selection table
            // already holds for every metadata value -- a full cube for most, a
            // half for a slab, a plate for a pressure plate.
            out[0] = selectionBox(id, metadata);
            return 1;

        case RenderType::Stairs:
            // The only shape in a1.1.2 with two boxes, and they are the two you
            // walk into as well as the two that are drawn.
            return collisionBoxes(id, metadata, out, max);

        case RenderType::Cactus:
            // Three boxes and two faces each: the cell for the top and the
            // bottom, and a pair of inset sides per horizontal axis. See
            // `cactusBoxes` -- this is the one shape whose boxes are not solid
            // and whose caller has to honour the mask.
            return cactusBoxes(id, metadata, out, faceMask, max);

        case RenderType::Fence:
            return fenceBoxes(connections, out, max);

        default:
            // Sheets, crosses, torches, fluids and fire. Not boxes, and the
            // original does not draw them in three dimensions in a slot either.
            return 0;
    }
}

int itemRenderBoxes(BlockId id, AABB* out, int max, int* faceMask)
{
    if (out == nullptr || max <= 0) {
        return 0;
    }

    // Only the standard renderer reads the bounds the item-render call leaves;
    // the stairs, the fence and the cactus each draw from their own constants
    // and ignore it, which is why they go on sharing the world's shapes.
    if (def(id).render == RenderType::Cube) {
        if (faceMask != nullptr) {
            faceMask[0] = kAllRenderFaces;
        }
        out[0] = itemRenderBox(id);
        return 1;
    }
    return renderBoxes(id, 0, 0, out, max, faceMask);
}

}  // namespace mc::block
