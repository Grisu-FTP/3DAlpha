// See falling_block_mesh.hpp.

#include "core/render/falling_block_mesh.hpp"

#include "core/block/registry.hpp"
#include "core/render/draw_budget.hpp"

namespace mc::render {
namespace {

// The detail position is a signed short of 1/1024 blocks, so a little under 32
// blocks either way of the origin. The same bound the items and the particles
// use.
constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

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

// Corner indices per face, in `mc::mesh::Face` order -- `mesh::kFaceCorner`
// rewritten as indices into the eight, where an index is `x + 2y + 4z` with
// each bit meaning "the high edge on that axis". The same table
// `item_entity_mesh.cpp` carries, and for the same reason: the winding is the
// one that argument already settled.
constexpr int kFace[6][4] = {
    {0, 1, 5, 4},  // -Y
    {6, 7, 3, 2},  // +Y
    {1, 0, 2, 3},  // -Z
    {4, 5, 7, 6},  // +Z
    {0, 4, 6, 2},  // -X
    {5, 1, 3, 7},  // +X
};

}  // namespace

int buildFallingBlocks(const entity::FallingBlockSystem& system, double originX,
                       double originY, double originZ, float partial,
                       mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kFallingBlockVerticesEach) {
        return 0;
    }

    // Where an entity is drawn relative to the origin, and whether it is drawn
    // at all. Shared by the nearest-first count and the build, which must agree.
    const auto place = [&](int i, double* cx, double* cy, double* cz) {
        const entity::FallingBlock& e = system[i];
        if (!e.alive()) {
            return false;
        }
        *cx = e.prevX + (e.x - e.prevX) * double(partial) - originX;
        *cy = e.prevY + (e.y - e.prevY) * double(partial) - originY;
        *cz = e.prevZ + (e.z - e.prevZ) * double(partial) - originZ;
        return *cx >= -kLimit && *cx <= kLimit && *cy >= -kLimit && *cy <= kLimit
               && *cz >= -kLimit && *cz <= kLimit;
    };
    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    DrawCutoff cutoff;
    if (system.count() * kFallingBlockVerticesEach > max) {
        cutoff.compute(system.count(), max, place,
                       [](int) { return kFallingBlockVerticesEach; });
    }

    int written = 0;
    for (int i = 0; i < system.count(); ++i) {
        const entity::FallingBlock& e = system[i];
        double cx = 0.0;
        double cy = 0.0;
        double cz = 0.0;
        if (!place(i, &cx, &cy, &cz) || written + kFallingBlockVerticesEach > max
            || !cutoff.admit(cx, cy, cz, kFallingBlockVerticesEach)) {
            continue;
        }

        const block::BlockDef& def = block::def(e.block);

        // **A full cube, not the 0.98 collision box.** `renderBlockFallingSand`
        // draws the block's own bounds, which for sand and gravel are 0..1; the
        // 0.98 is the entity's box and is what it collides with. Drawing the
        // smaller one would leave a visible seam where the falling block met
        // the pile it is landing on.
        const double lo[3] = {cx - 0.5, cy - 0.5, cz - 0.5};
        const double hi[3] = {cx + 0.5, cy + 0.5, cz + 0.5};

        double px[8], py[8], pz[8];
        for (int c = 0; c < 8; ++c) {
            px[c] = (c & 1) != 0 ? hi[0] : lo[0];
            py[c] = (c & 2) != 0 ? hi[1] : lo[1];
            pz[c] = (c & 4) != 0 ? hi[2] : lo[2];
        }

        for (int face = 0; face < 6; ++face) {
            const TileUv uv = tileUv(int(def.faces[face]));
            const u8 shade = mesh::kFaceShade[face];
            for (int c = 0; c < 4; ++c) {
                const int idx = kFace[face][c];
                mesh::DetailVertex& v = out[written];
                v.x = toUnits(px[idx]);
                v.y = toUnits(py[idx]);
                v.z = toUnits(pz[idx]);
                v.face = 0;
                // `mesh::kFaceCornerUV`, which is the same four pairs for every
                // face **except the bottom** -- and that exception is the reason
                // this reads the table rather than carrying one pattern.
                v.u = mesh::kFaceCornerUV[face][c][0] != 0 ? uv.u1 : uv.u0;
                v.v = mesh::kFaceCornerUV[face][c][1] != 0 ? uv.v1 : uv.v0;
                v.r = shade;
                v.g = shade;
                v.b = shade;
                v.light = e.light;
                ++written;
            }
        }
    }
    return written;
}

}  // namespace mc::render
