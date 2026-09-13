// `drawBlockBreaking`'s geometry. See break_overlay.hpp.

#include "core/render/break_overlay.hpp"

#include "core/block/model.hpp"
#include "core/block/registry.hpp"
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
    const int col = tile % mesh::kAtlasTilesPerEdge;
    const int row = tile / mesh::kAtlasTilesPerEdge;
    const i16 u0 = i16(col * mesh::kUvUnitsPerTile);
    const i16 u1 = i16((col + 1) * mesh::kUvUnitsPerTile);
    const i16 v0 = i16(row * mesh::kUvUnitsPerTile);
    const i16 v1 = i16((row + 1) * mesh::kUvUnitsPerTile);

    int written = 0;
    for (int b = 0; b < count; ++b) {
        const AABB grown = boxes[b].expand(kBreakOverlayExpand, kBreakOverlayExpand,
                                           kBreakOverlayExpand);
        const double lo[3] = {grown.minX, grown.minY, grown.minZ};
        const double hi[3] = {grown.maxX, grown.maxY, grown.maxZ};
        for (int face = 0; face < mesh::kFaceCount; ++face) {
            if ((masks[b] & (1 << face)) == 0) {
                continue;
            }
            if (written + 4 > maxVertices) {
                return written;
            }
            for (int c = 0; c < 4; ++c) {
                const mesh::Corner& corner = mesh::kFaceCorner[face][c];
                mesh::DetailVertex& v = out[written++];
                v.x = toUnits(corner.x != 0 ? hi[0] : lo[0]);
                v.y = toUnits(corner.y != 0 ? hi[1] : lo[1]);
                v.z = toUnits(corner.z != 0 ? hi[2] : lo[2]);
                v.face = i16(face);
                v.u = mesh::kFaceCornerUV[face][c][0] != 0 ? u1 : u0;
                v.v = mesh::kFaceCornerUV[face][c][1] != 0 ? v1 : v0;
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
