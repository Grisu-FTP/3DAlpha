// See painting_mesh.hpp. `bw.a(Ljc;IIII)V` transcribed.

#include "core/render/painting_mesh.hpp"

#include "core/render/draw_budget.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

// `glScalef(0.0625F, ...)` -- model units to blocks.
constexpr double kModelToBlock = 1.0 / 16.0;

// **Full brightness on all three channels.** `RenderPainting` sets no vertex
// colour; the light byte is what darkens a picture in a cellar.
constexpr u8 kPaintingShade = 255;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

// A fraction of the art sheet into the vertex's 1/16384 units. The art sheet is
// the whole texture for this pass, so a fraction is exactly a UV.
i16 artUv(float fraction)
{
    const float units = fraction * float(mesh::kUvUnitsPerAtlas);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

// One corner, in the painting's own model space, transformed and written.
struct Frame {
    double x, y, z;   // the canvas centre, relative to the mesh origin
    double ax, az;    // model +x in world, as (x, z)
    double zx, zz;    // model +z in world, as (x, z)
};

void writeCorner(mesh::DetailVertex& v, const Frame& f, float mx, float my, float mz,
                 float u, float uv, u8 light)
{
    const double bx = double(mx) * kModelToBlock;
    const double by = double(my) * kModelToBlock;
    const double bz = double(mz) * kModelToBlock;
    v.x = toUnits(f.x + bx * f.ax + bz * f.zx);
    v.y = toUnits(f.y + by);
    v.z = toUnits(f.z + bx * f.az + bz * f.zz);
    v.face = 0;
    v.u = artUv(u);
    v.v = artUv(uv);
    v.r = kPaintingShade;
    v.g = kPaintingShade;
    v.b = kPaintingShade;
    v.light = light;
}

}  // namespace

int buildPaintings(const entity::PaintingSystem& system, double originX, double originY,
                   double originZ, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < 4) {
        return 0;
    }

    // Where a painting is relative to the origin, and whether it is drawn at
    // all. Shared by the nearest-first count and the build, which must agree.
    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::Painting& p = system[index];
        if (!p.alive) {
            return false;
        }
        *rx = p.x - originX;
        *ry = p.y - originY;
        *rz = p.z - originZ;
        return *rx >= -kLimit && *rx <= kLimit && *ry >= -kLimit && *ry <= kLimit
               && *rz >= -kLimit && *rz <= kLimit;
    };
    const auto cost = [&](int index) {
        const entity::PaintingArt& art = system[index].artwork();
        return art.blocksWide() * art.blocksTall() * 6 * 4;
    };
    // **Nearest first** when the buffer cannot take them all -- the far end of
    // a long gallery goes, not whichever pictures sit late in the pool. See
    // draw_budget.hpp. The fast test is the worst case, sixteen cells each.
    DrawCutoff cutoff;
    if (system.count() * kPaintingMaxVertices > max) {
        cutoff.compute(system.count(), max, place, cost);
    }

    int written = 0;
    for (int index = 0; index < system.count(); ++index) {
        const entity::Painting& p = system[index];
        double rx = 0.0;
        double ry = 0.0;
        double rz = 0.0;
        if (!place(index, &rx, &ry, &rz) || !cutoff.admit(rx, ry, rz, cost(index))) {
            continue;
        }

        const entity::PaintingArt& art = p.artwork();
        const int cellsX = art.blocksWide();
        const int cellsY = art.blocksTall();
        if (written + cost(index) > max) {
            break;
        }

        // **The facing, as two axes rather than an angle.** `doRender` turns by
        // `rotationYaw`, which for a painting is `direction * 90`; writing the
        // four cases out is the same rotation with no trigonometry and no
        // question about which way positive goes. Model -Z is the front, so
        // these are chosen to put model -Z on the wall's outward face:
        //
        //   dir 0 faces -Z, 1 faces -X, 2 faces +Z, 3 faces +X.
        Frame frame{rx, ry, rz, 1.0, 0.0, 0.0, 1.0};
        switch (p.direction) {
            case 1:
                frame.ax = 0.0; frame.az = -1.0; frame.zx = 1.0; frame.zz = 0.0;
                break;
            case 2:
                frame.ax = -1.0; frame.az = 0.0; frame.zx = 0.0; frame.zz = -1.0;
                break;
            case 3:
                frame.ax = 0.0; frame.az = 1.0; frame.zx = -1.0; frame.zz = 0.0;
                break;
            default:
                break;
        }

        // `-sizeX / 2.0F` and `-sizeY / 2.0F`, in model units.
        const float baseX = -float(art.width) / 2.0f;
        const float baseY = -float(art.height) / 2.0f;
        const float zFront = -0.5f;
        const float zBack = 0.5f;

        // The back-of-canvas tile, as fractions of the sheet.
        const float bu0 = float(kCanvasBackU) / float(kArtSheetEdge);
        const float bu1 = float(kCanvasBackU + kCanvasBackSize) / float(kArtSheetEdge);
        const float bv0 = float(kCanvasBackV) / float(kArtSheetEdge);
        const float bv1 = float(kCanvasBackV + kCanvasBackSize) / float(kArtSheetEdge);
        // The two degenerate lines the edges sample. `0.001953125F` is half a
        // texel of 256 and `0.7519531F` is `0.75` plus the same -- the original
        // takes the *middle* of the tile's first texel rather than its corner,
        // which is what stops an edge strip picking up the neighbouring tile.
        const float bvMid = bv0 + 0.5f / float(kArtSheetEdge);
        const float buMid = bu0 + 0.5f / float(kArtSheetEdge);

        for (int i = 0; i < cellsX; ++i) {
            for (int j = 0; j < cellsY; ++j) {
                const float x1 = baseX + float((i + 1) * 16);
                const float x0 = baseX + float(i * 16);
                const float y1 = baseY + float((j + 1) * 16);
                const float y0 = baseY + float(j * 16);

                const u8 light = p.cellLight[(j < 4 ? j : 3) * 4 + (i < 4 ? i : 3)];

                // The art's rectangle for this cell. **u runs backwards against
                // x**, which is what makes the picture read the right way round
                // to somebody standing in front of it -- looking along +Z, the
                // viewer's right is -X.
                const float u1 = float(int(art.u) + int(art.width) - i * 16)
                                 / float(kArtSheetEdge);
                const float u0 = float(int(art.u) + int(art.width) - (i + 1) * 16)
                                 / float(kArtSheetEdge);
                const float v1 = float(int(art.v) + int(art.height) - j * 16)
                                 / float(kArtSheetEdge);
                const float v0 = float(int(art.v) + int(art.height) - (j + 1) * 16)
                                 / float(kArtSheetEdge);

                // Front -- the picture.
                writeCorner(out[written++], frame, x1, y0, zFront, u0, v1, light);
                writeCorner(out[written++], frame, x0, y0, zFront, u1, v1, light);
                writeCorner(out[written++], frame, x0, y1, zFront, u1, v0, light);
                writeCorner(out[written++], frame, x1, y1, zFront, u0, v0, light);

                // Back.
                writeCorner(out[written++], frame, x1, y1, zBack, bu0, bv0, light);
                writeCorner(out[written++], frame, x0, y1, zBack, bu1, bv0, light);
                writeCorner(out[written++], frame, x0, y0, zBack, bu1, bv1, light);
                writeCorner(out[written++], frame, x1, y0, zBack, bu0, bv1, light);

                // Top edge.
                writeCorner(out[written++], frame, x1, y1, zFront, bu0, bvMid, light);
                writeCorner(out[written++], frame, x0, y1, zFront, bu1, bvMid, light);
                writeCorner(out[written++], frame, x0, y1, zBack, bu1, bvMid, light);
                writeCorner(out[written++], frame, x1, y1, zBack, bu0, bvMid, light);

                // Bottom edge.
                writeCorner(out[written++], frame, x1, y0, zBack, bu0, bvMid, light);
                writeCorner(out[written++], frame, x0, y0, zBack, bu1, bvMid, light);
                writeCorner(out[written++], frame, x0, y0, zFront, bu1, bvMid, light);
                writeCorner(out[written++], frame, x1, y0, zFront, bu0, bvMid, light);

                // The two side edges, which sample a vertical line instead.
                writeCorner(out[written++], frame, x1, y1, zBack, buMid, bv0, light);
                writeCorner(out[written++], frame, x1, y0, zBack, buMid, bv1, light);
                writeCorner(out[written++], frame, x1, y0, zFront, buMid, bv1, light);
                writeCorner(out[written++], frame, x1, y1, zFront, buMid, bv0, light);

                writeCorner(out[written++], frame, x0, y1, zFront, buMid, bv0, light);
                writeCorner(out[written++], frame, x0, y0, zFront, buMid, bv1, light);
                writeCorner(out[written++], frame, x0, y0, zBack, buMid, bv1, light);
                writeCorner(out[written++], frame, x0, y1, zBack, buMid, bv0, light);
            }
        }
    }
    return written;
}

}  // namespace mc::render
