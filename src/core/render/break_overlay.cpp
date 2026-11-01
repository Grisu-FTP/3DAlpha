// `drawBlockBreaking`'s geometry. See break_overlay.hpp.

#include "core/render/break_overlay.hpp"

#include "core/block/model.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/util/aabb.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

namespace mc::render {

namespace {

i16 toUnits(double blocks)
{
    const double units = blocks * double(mesh::kDetailUnitsPerBlock);
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

}  // namespace

int buildBreakOverlay(block::BlockId id, u8 metadata, int stage, mesh::DetailVertex* out,
                      int maxVertices)
{
    if (stage < 0 || stage >= kBreakOverlayStages) {
        return 0;
    }
    // `if (block == null) block = Block.stone` -- a cell that has already gone.
    const block::BlockId shape =
        id == block::kAir ? block::BlockId(mcver::Block::Stone) : id;

    AABB boxes[block::kMaxRenderBoxes];
    int masks[block::kMaxRenderBoxes];
    const int count =
        block::renderBoxes(shape, metadata, 0, boxes, block::kMaxRenderBoxes, masks);

    const int tile = kBreakOverlayFirstTile + stage;

    int written = 0;
    for (int b = 0; b < count; ++b) {
        const AABB& box = boxes[b];
        const double lo[3] = {box.minX, box.minY, box.minZ};
        const double hi[3] = {box.maxX, box.maxY, box.maxZ};
        for (int face = 0; face < mesh::kFaceCount; ++face) {
            if ((masks[b] & (1 << face)) == 0) {
                continue;
            }
            if (written + 4 > maxVertices) {
                return written;
            }
            // The face's own slice of the crack tile, as `renderBlockUsingTexture`
            // takes it: a button's crack is the button's few texels at the
            // tile's pixel scale, not the whole tile squeezed onto it.
            const mesh::BoxTileUv slice = mesh::boxTileUv(tile, box, face);
            // Lifted off the block along the face's normal only. Growing every
            // axis would push the edges into the neighbours above, below and
            // beside, and the crack would be drawn over a sliver of them.
            const int normal = (face == mesh::kFaceNegY || face == mesh::kFacePosY)   ? 1
                               : (face == mesh::kFaceNegZ || face == mesh::kFacePosZ) ? 2
                                                                                      : 0;
            for (int c = 0; c < 4; ++c) {
                const mesh::Corner& corner = mesh::kFaceCorner[face][c];
                const int hiCorner[3] = {corner.x, corner.y, corner.z};
                double pos[3];
                for (int axis = 0; axis < 3; ++axis) {
                    pos[axis] = hiCorner[axis] != 0 ? hi[axis] : lo[axis];
                    if (axis == normal) {
                        pos[axis] += hiCorner[axis] != 0 ? kBreakOverlayExpand
                                                         : -kBreakOverlayExpand;
                    }
                }
                mesh::DetailVertex& v = out[written++];
                v.x = toUnits(pos[0]);
                v.y = toUnits(pos[1]);
                v.z = toUnits(pos[2]);
                v.face = i16(face);
                v.u = mesh::kFaceCornerUV[face][c][0] != 0 ? slice.u1 : slice.u0;
                v.v = mesh::kFaceCornerUV[face][c][1] != 0 ? slice.v1 : slice.v0;
                // `glColor4f(1, 1, 1, 0.5)`, unshaded and unlit: the blend does
                // the darkening, off whatever the block already drew.
                v.r = 255;
                v.g = 255;
                v.b = 255;
                v.light = 0xFF;
            }
        }
    }
    return written;
}

}  // namespace mc::render
