// See primed_tnt_mesh.hpp.

#include "core/render/primed_tnt_mesh.hpp"

#include "core/block/registry.hpp"
#include "core/render/draw_budget.hpp"

namespace mc::render {
namespace {

// The detail position is a signed short of 1/1024 blocks, so a little under 32
// blocks either way of the origin. The same bound the items and the falling
// blocks use.
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

// The same eight-corner indexing and the same winding `falling_block_mesh.cpp`
// uses; see the note there for why this table is written as indices.
constexpr int kFace[6][4] = {
    {0, 1, 5, 4},  // -Y
    {6, 7, 3, 2},  // +Y
    {1, 0, 2, 3},  // -Z
    {4, 5, 7, 6},  // +Z
    {0, 4, 6, 2},  // -X
    {5, 1, 3, 7},  // +X
};

float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

}  // namespace

float primedTntScale(int fuse, float partial)
{
    // `(float)fuse - partialTick + 1.0F`, which is the ticks left including the
    // one being drawn. `hw` tests it against 10 and skips the scale entirely
    // when it is larger -- the same answer as scaling by one.
    const float left = float(fuse) - partial + 1.0f;
    if (left >= kPrimedTntSwellTicks) {
        return 1.0f;
    }
    float f = clamp01(1.0f - left / kPrimedTntSwellTicks);
    // **Squared twice, not cubed and not squared once.** The class file does
    // `f *= f;` on two consecutive lines, which is the fourth power and is why
    // TNT stays its own size until the last half second.
    f *= f;
    f *= f;
    return 1.0f + f * kPrimedTntSwell;
}

float primedTntFlashAlpha(int fuse, float partial)
{
    // `1.0F - ((float)fuse - partialTick + 1.0F) / 100.0F` times 0.8. Not
    // clamped in the original because a fuse never exceeds 80; clamped here
    // because a restored save is not a guarantee.
    const float left = float(fuse) - partial + 1.0f;
    return clamp01(1.0f - left / kPrimedTntFlashFade) * kPrimedTntFlashPeak;
}

int buildPrimedTnt(const entity::PrimedTntSystem& system, double originX, double originY,
                   double originZ, float partial, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kPrimedTntVerticesEach) {
        return 0;
    }

    const auto place = [&](int i, double* cx, double* cy, double* cz) {
        const entity::PrimedTnt& e = system[i];
        if (!e.alive()) {
            return false;
        }
        *cx = e.prevX + (e.x - e.prevX) * double(partial) - originX;
        *cy = e.prevY + (e.y - e.prevY) * double(partial) - originY;
        *cz = e.prevZ + (e.z - e.prevZ) * double(partial) - originZ;
        return *cx >= -kLimit && *cx <= kLimit && *cy >= -kLimit && *cy <= kLimit
               && *cz >= -kLimit && *cz <= kLimit;
    };
    DrawCutoff cutoff;
    if (system.count() * kPrimedTntVerticesEach > max) {
        cutoff.compute(system.count(), max, place, [](int) { return kPrimedTntVerticesEach; });
    }

    // Every one of these is the same block, so the faces are read once rather
    // than per entity.
    const block::BlockDef& def = block::def(block::BlockId(mcver::Block::Tnt));

    int written = 0;
    for (int i = 0; i < system.count(); ++i) {
        const entity::PrimedTnt& e = system[i];
        double cx = 0.0;
        double cy = 0.0;
        double cz = 0.0;
        if (!place(i, &cx, &cy, &cz) || written + kPrimedTntVerticesEach > max
            || !cutoff.admit(cx, cy, cz, kPrimedTntVerticesEach)) {
            continue;
        }

        // **A full cube and not the 0.98 collision box**, for the same reason a
        // falling block is one: `renderBlockAsItem` draws the block's own 0..1
        // bounds. The swell is applied about the entity's centre, which is
        // where `hw`'s `glScalef` acts -- it follows the `glTranslatef`.
        const double half = 0.5 * double(primedTntScale(e.fuse, partial));
        const double lo[3] = {cx - half, cy - half, cz - half};
        const double hi[3] = {cx + half, cy + half, cz + half};

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

int buildPrimedTntFlash(const entity::PrimedTnt& e, double originX, double originY,
                        double originZ, float partial, OutlineVertex* out, int max)
{
    if (out == nullptr || max < kPrimedTntFlashVerticesEach || !e.alive()
        || !primedTntFlashing(e.fuse)) {
        return 0;
    }

    const double cx = e.prevX + (e.x - e.prevX) * double(partial) - originX;
    const double cy = e.prevY + (e.y - e.prevY) * double(partial) - originY;
    const double cz = e.prevZ + (e.z - e.prevZ) * double(partial) - originZ;

    // **The same cube the textured pass drew, to the bit.** `hw` reuses one
    // `glScalef` for both draws, so any disagreement here would show as a white
    // rind round the block.
    const double half = 0.5 * double(primedTntScale(e.fuse, partial));
    const double lo[3] = {cx - half, cy - half, cz - half};
    const double hi[3] = {cx + half, cy + half, cz + half};

    float px[8], py[8], pz[8];
    for (int c = 0; c < 8; ++c) {
        px[c] = float((c & 1) != 0 ? hi[0] : lo[0]);
        py[c] = float((c & 2) != 0 ? hi[1] : lo[1]);
        pz[c] = float((c & 4) != 0 ? hi[2] : lo[2]);
    }

    int written = 0;
    for (int face = 0; face < 6; ++face) {
        // Two triangles from the quad's four corners: 0-1-2 and 0-2-3, the
        // winding the shared index buffer would have given them.
        const int quad[4] = {kFace[face][0], kFace[face][1], kFace[face][2], kFace[face][3]};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int t = 0; t < 6; ++t) {
            const int idx = quad[order[t]];
            out[written++] = OutlineVertex{px[idx], py[idx], pz[idx]};
        }
    }
    return written;
}

}  // namespace mc::render
