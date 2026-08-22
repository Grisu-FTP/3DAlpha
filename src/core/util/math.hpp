#pragma once

// The small amount of linear algebra core needs.
//
// Deliberately not a maths library. Core has to build without citro3d, so it
// cannot use C3D_Mtx -- and it should not want to: citro3d stores an FVec as
// (w, z, y, x), which is a trap for anyone who reaches into it expecting the
// obvious order. The platform layer converts at the boundary and core stays
// readable.
//
// Row-major, m[row][col], and vectors are columns: clip = M * v.

#include "core/util/types.hpp"

namespace mc {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity()
    {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

// n.p + d >= 0 is inside. Kept normalised so `d` is a real distance, which
// makes the AABB test a comparison rather than a division.
struct Plane {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f, d = 0.0f;

    float distanceTo(float x, float y, float z) const { return nx * x + ny * y + nz * z + d; }
};

}  // namespace mc
