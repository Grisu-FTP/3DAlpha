// The selection outline's geometry.
//
// The part of drawing a selection box that can be wrong in an interesting way,
// separated from the part that cannot be tested without a console. What the
// console does with these vertices is a pipeline binding and a draw call; what
// is checked here is that the vertices describe the right box.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/render/outline.hpp"
#include "framework.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::render::buildOutline;
using mc::render::kOutlineExpand;
using mc::render::kOutlineHalfThickness;
using mc::render::kOutlineVertexCount;
using mc::render::OutlineVertex;

namespace {

struct Bounds {
    float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
    float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;

    void add(const OutlineVertex& v)
    {
        minX = v.x < minX ? v.x : minX;
        minY = v.y < minY ? v.y : minY;
        minZ = v.z < minZ ? v.z : minZ;
        maxX = v.x > maxX ? v.x : maxX;
        maxY = v.y > maxY ? v.y : maxY;
        maxZ = v.z > maxZ ? v.z : maxZ;
    }
};

Bounds boundsOf(const OutlineVertex* v, int n)
{
    Bounds b;
    for (int i = 0; i < n; ++i) {
        b.add(v[i]);
    }
    return b;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-5; }

}  // namespace

TEST(the_outline_surrounds_the_expanded_box_and_no_more)
{
    OutlineVertex verts[kOutlineVertexCount];
    const AABB box{10.0, 64.0, -20.0, 11.0, 65.0, -19.0};
    CHECK_EQ(buildOutline(box, 0.0, 0.0, 0.0, verts, kOutlineVertexCount), kOutlineVertexCount);

    const Bounds b = boundsOf(verts, kOutlineVertexCount);
    const double grow = kOutlineExpand + kOutlineHalfThickness;
    CHECK(near(b.minX, box.minX - grow));
    CHECK(near(b.minY, box.minY - grow));
    CHECK(near(b.minZ, box.minZ - grow));
    CHECK(near(b.maxX, box.maxX + grow));
    CHECK(near(b.maxY, box.maxY + grow));
    CHECK(near(b.maxZ, box.maxZ + grow));
}

TEST(the_origin_is_subtracted_so_the_renderer_gets_small_numbers)
{
    // The Far Lands case: a world coordinate of twelve million has no business
    // reaching a float. Relative to its own chunk it is under sixteen.
    OutlineVertex verts[kOutlineVertexCount];
    const double far = 12550821.0;
    const AABB box{far, 64.0, far, far + 1.0, 65.0, far + 1.0};
    CHECK_EQ(buildOutline(box, far, 64.0, far, verts, kOutlineVertexCount), kOutlineVertexCount);

    const Bounds b = boundsOf(verts, kOutlineVertexCount);
    CHECK(b.minX > -1.0f && b.maxX < 2.0f);
    CHECK(b.minZ > -1.0f && b.maxZ < 2.0f);
}

TEST(every_one_of_the_twelve_edges_gets_geometry)
{
    // A box whose edges are all different lengths, so an edge emitted along the
    // wrong axis lands somewhere the check below will notice.
    OutlineVertex verts[kOutlineVertexCount];
    const AABB box{0.0, 0.0, 0.0, 4.0, 2.0, 8.0};
    CHECK_EQ(buildOutline(box, 0.0, 0.0, 0.0, verts, kOutlineVertexCount), kOutlineVertexCount);

    // The midpoint of each of the twelve edges must have geometry around it.
    const double xs[2] = {0.0, 4.0};
    const double ys[2] = {0.0, 2.0};
    const double zs[2] = {0.0, 8.0};
    int covered = 0;
    for (int axis = 0; axis < 3; ++axis) {
        for (int a = 0; a < 2; ++a) {
            for (int b = 0; b < 2; ++b) {
                double p[3];
                if (axis == 0) { p[0] = 2.0; p[1] = ys[a]; p[2] = zs[b]; }
                else if (axis == 1) { p[0] = xs[a]; p[1] = 1.0; p[2] = zs[b]; }
                else { p[0] = xs[a]; p[1] = ys[b]; p[2] = 4.0; }

                bool found = false;
                for (int i = 0; i < kOutlineVertexCount && !found; ++i) {
                    const double dx = double(verts[i].x) - p[0];
                    const double dy = double(verts[i].y) - p[1];
                    const double dz = double(verts[i].z) - p[2];
                    // Within an edge's own half-length plus its thickness.
                    if (std::fabs(dx) <= 2.1 && std::fabs(dy) <= 1.1 && std::fabs(dz) <= 4.1) {
                        found = true;
                    }
                }
                if (found) {
                    ++covered;
                }
            }
        }
    }
    CHECK_EQ(covered, 12);
}

TEST(a_buffer_that_is_too_small_is_refused_rather_than_overrun)
{
    OutlineVertex verts[kOutlineVertexCount];
    CHECK_EQ(buildOutline(AABB{0, 0, 0, 1, 1, 1}, 0, 0, 0, verts, kOutlineVertexCount - 1), 0);
    CHECK_EQ(buildOutline(AABB{0, 0, 0, 1, 1, 1}, 0, 0, 0, nullptr, kOutlineVertexCount), 0);
}

TEST(a_flat_shape_still_gets_a_box_worth_drawing)
{
    // A snow layer is an eighth of a block tall and a pressure plate is a
    // sixteenth; neither should collapse to nothing.
    OutlineVertex verts[kOutlineVertexCount];
    const AABB plate = block::selectionBox(BlockId(mcver::Block::SnowLayer), 0);
    CHECK_EQ(buildOutline(plate, 0.0, 0.0, 0.0, verts, kOutlineVertexCount), kOutlineVertexCount);

    const Bounds b = boundsOf(verts, kOutlineVertexCount);
    CHECK(b.maxY - b.minY > 0.0f);
    CHECK(b.maxX - b.minX > 0.9f);
}
