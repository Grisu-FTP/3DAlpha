#include "core/util/frustum.hpp"

#include "core/world/section.hpp"

#include <cmath>

namespace mc {

namespace {

Plane normalised(float a, float b, float c, float d)
{
    const float length = std::sqrt(a * a + b * b + c * c);
    if (length <= 0.0f) {
        // A degenerate matrix -- an uninitialised one, most likely. Returning a
        // plane that accepts everything makes that look like "nothing is
        // culled" rather than "nothing is drawn", which is far easier to spot.
        return Plane{0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / length;
    return Plane{a * inv, b * inv, c * inv, d * inv};
}

}  // namespace

void Frustum::setFromViewProjection(const Mat4& vp, ClipRange range)
{
    const float(*m)[4] = vp.m;

    // Each side plane is the w row plus or minus one of the others: a point is
    // inside the left plane when x >= -w, which is (row3 + row0) . p >= 0.
    planes_[kLeft] = normalised(m[3][0] + m[0][0], m[3][1] + m[0][1],
                                m[3][2] + m[0][2], m[3][3] + m[0][3]);
    planes_[kRight] = normalised(m[3][0] - m[0][0], m[3][1] - m[0][1],
                                 m[3][2] - m[0][2], m[3][3] - m[0][3]);
    planes_[kBottom] = normalised(m[3][0] + m[1][0], m[3][1] + m[1][1],
                                  m[3][2] + m[1][2], m[3][3] + m[1][3]);
    planes_[kTop] = normalised(m[3][0] - m[1][0], m[3][1] - m[1][1],
                               m[3][2] - m[1][2], m[3][3] - m[1][3]);

    // The near plane is z >= -w in the two ranges that start at -1, and z >= 0
    // in Direct3D's.
    if (range == ClipRange::ZeroToOne) {
        planes_[kNear] = normalised(m[2][0], m[2][1], m[2][2], m[2][3]);
    } else {
        planes_[kNear] = normalised(m[3][0] + m[2][0], m[3][1] + m[2][1],
                                    m[3][2] + m[2][2], m[3][3] + m[2][3]);
    }

    // The far plane is z <= w in both the conventions that end at +1, and z <= 0
    // on the PICA -- which makes it -row2, not row3 - row2. Using the wrong one
    // here does not cull the world, it just stops culling anything: on a
    // citro3d matrix, row3 - row2 comes out as a plane roughly at the camera,
    // which every section in front of it passes.
    if (range == ClipRange::NegativeOneToZero) {
        planes_[kFar] = normalised(-m[2][0], -m[2][1], -m[2][2], -m[2][3]);
    } else {
        planes_[kFar] = normalised(m[3][0] - m[2][0], m[3][1] - m[2][1],
                                   m[3][2] - m[2][2], m[3][3] - m[2][3]);
    }
}

bool Frustum::testBox(const Vec3& min, const Vec3& max) const
{
    for (int i = 0; i < kPlaneCount; ++i) {
        const Plane& p = planes_[i];

        // The positive vertex: the corner of the box that reaches furthest
        // along this plane's normal. If even that one is outside, every corner
        // is, and the box is gone in one test instead of eight.
        const float x = p.nx >= 0.0f ? max.x : min.x;
        const float y = p.ny >= 0.0f ? max.y : min.y;
        const float z = p.nz >= 0.0f ? max.z : min.z;

        if (p.distanceTo(x, y, z) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool Frustum::testSection(i32 chunkX, int sectionY, i32 chunkZ) const
{
    constexpr float kSize = float(world::Section::kSize);

    const Vec3 min{float(chunkX - originChunkX_) * kSize, float(sectionY) * kSize,
                   float(chunkZ - originChunkZ_) * kSize};
    const Vec3 max{min.x + kSize, min.y + kSize, min.z + kSize};
    return testBox(min, max);
}

}  // namespace mc
