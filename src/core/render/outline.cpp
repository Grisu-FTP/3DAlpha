// The selection outline's geometry. See outline.hpp for why it is triangles.

#include "core/render/outline.hpp"

namespace mc::render {
namespace {

// One axis-aligned box, as six quads wound counter-clockwise when seen from
// outside -- the winding the renderer culls against.
int emitBox(double minX, double minY, double minZ, double maxX, double maxY, double maxZ,
            OutlineVertex* out)
{
    const float x0 = float(minX);
    const float y0 = float(minY);
    const float z0 = float(minZ);
    const float x1 = float(maxX);
    const float y1 = float(maxY);
    const float z1 = float(maxZ);

    // Each face as two triangles. Written out rather than indexed: 432
    // vertices a frame is nothing, and an index buffer would be a second
    // allocation and a second upload for no gain.
    const float faces[6][4][3] = {
        {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},  // -Y
        {{x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {x0, y1, z0}},  // +Y
        {{x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}},  // -Z
        {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}},  // +Z
        {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},  // -X
        {{x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}},  // +X
    };

    int n = 0;
    for (const auto& face : faces) {
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (const int i : order) {
            out[n].x = face[i][0];
            out[n].y = face[i][1];
            out[n].z = face[i][2];
            ++n;
        }
    }
    return n;
}

}  // namespace

int buildOutline(const AABB& box, double originX, double originY, double originZ,
                 OutlineVertex* out, int max)
{
    if (out == nullptr || max < kOutlineVertexCount) {
        return 0;
    }

    // The original's expansion, then everything relative to the origin. The
    // subtraction is the last thing done in double.
    const double x0 = box.minX - kOutlineExpand - originX;
    const double y0 = box.minY - kOutlineExpand - originY;
    const double z0 = box.minZ - kOutlineExpand - originZ;
    const double x1 = box.maxX + kOutlineExpand - originX;
    const double y1 = box.maxY + kOutlineExpand - originY;
    const double z1 = box.maxZ + kOutlineExpand - originZ;

    const double t = kOutlineHalfThickness;

    int n = 0;

    // The four edges running along X, at each combination of the y and z faces.
    const double ys[2] = {y0, y1};
    const double zs[2] = {z0, z1};
    for (const double y : ys) {
        for (const double z : zs) {
            n += emitBox(x0 - t, y - t, z - t, x1 + t, y + t, z + t, out + n);
        }
    }

    // The four along Y.
    const double xs[2] = {x0, x1};
    for (const double x : xs) {
        for (const double z : zs) {
            n += emitBox(x - t, y0 - t, z - t, x + t, y1 + t, z + t, out + n);
        }
    }

    // And the four along Z.
    for (const double x : xs) {
        for (const double y : ys) {
            n += emitBox(x - t, y - t, z0 - t, x + t, y + t, z1 + t, out + n);
        }
    }

    return n;
}

}  // namespace mc::render
