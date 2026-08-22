#include "framework.hpp"

#include "core/util/frustum.hpp"

#include <cmath>
#include <utility>

using namespace mc;

namespace {

// A perspective projection looking down -Z, in the OpenGL convention, built
// here rather than taken from a library so the test does not inherit whatever
// convention the library happens to use.
Mat4 perspective(float fovYRadians, float aspect, float near, float far)
{
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);

    Mat4 r;
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (far + near) / (near - far);
    r.m[2][3] = (2.0f * far * near) / (near - far);
    r.m[3][2] = -1.0f;
    return r;
}

Frustum lookingDownNegativeZ(float near = 0.1f, float far = 256.0f)
{
    Frustum frustum;
    frustum.setFromViewProjection(perspective(1.2f, 400.0f / 240.0f, near, far),
                                  ClipRange::NegativeOneToOne);
    return frustum;
}

bool pointVisible(const Frustum& f, float x, float y, float z)
{
    const Vec3 p{x, y, z};
    return f.testBox(p, p);
}

}  // namespace

TEST(a_point_in_front_is_visible_and_one_behind_is_not)
{
    const Frustum f = lookingDownNegativeZ();

    CHECK(pointVisible(f, 0.0f, 0.0f, -10.0f));

    // Directly behind the camera. This is the case the near plane exists for,
    // and the one a frustum built with the wrong clip convention gets wrong.
    CHECK(!pointVisible(f, 0.0f, 0.0f, 10.0f));

    // Beyond the far plane.
    CHECK(!pointVisible(f, 0.0f, 0.0f, -1000.0f));

    // Inside the near plane.
    CHECK(!pointVisible(f, 0.0f, 0.0f, -0.01f));
}

TEST(the_field_of_view_widens_with_distance)
{
    const Frustum f = lookingDownNegativeZ();

    // 10 units out, a point 30 units to the side is outside the cone; 100 units
    // out it is comfortably inside. If the side planes were parallel rather
    // than converging on the camera, both would answer the same way.
    CHECK(!pointVisible(f, 30.0f, 0.0f, -10.0f));
    CHECK(pointVisible(f, 30.0f, 0.0f, -100.0f));
}

TEST(a_box_straddling_a_plane_counts_as_visible)
{
    const Frustum f = lookingDownNegativeZ();

    // Culling has to be conservative: a section half in view is drawn whole,
    // because clipping is the GPU's job. A box spanning the camera plane
    // contains the camera and must survive.
    CHECK(f.testBox(Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}));

    // ...and one entirely behind still does not.
    CHECK(!f.testBox(Vec3{-1.0f, -1.0f, 10.0f}, Vec3{1.0f, 1.0f, 20.0f}));
}

TEST(the_positive_vertex_test_does_not_miss_a_corner)
{
    const Frustum f = lookingDownNegativeZ();

    // A wide, thin box whose centre is far off to the left but whose right edge
    // reaches into the view. Testing the centre alone would reject it.
    CHECK(f.testBox(Vec3{-400.0f, -1.0f, -100.0f}, Vec3{-10.0f, 1.0f, -90.0f}));

    // The same box moved so that even its nearest corner is outside.
    CHECK(!f.testBox(Vec3{-400.0f, -1.0f, -100.0f}, Vec3{-300.0f, 1.0f, -90.0f}));
}

TEST(sections_are_tested_in_world_coordinates)
{
    const Frustum f = lookingDownNegativeZ(0.1f, 512.0f);

    // The camera sits at the origin looking down -Z, so chunk (0, -1) is
    // straight ahead and chunk (0, 1) is behind.
    CHECK(f.testSection(0, 0, -1));
    CHECK(!f.testSection(0, 0, 4));

    // Far enough along -Z to leave a 512-unit far plane behind.
    CHECK(!f.testSection(0, 0, -60));
}

// The origin is what makes the culler usable far from spawn. a1.1.2's world
// runs to +-32,000,000 blocks and its Far Lands sit at 12,550,824, where a
// float's spacing is a whole block -- so a frustum built in world coordinates
// out there has plane distances around 1e7 and cannot resolve a 16-block
// section at all.
//
// The renderer's answer is to build the view-projection around the camera's own
// chunk. This checks the other half of that: a section named in absolute
// coordinates has to be tested in the same relative space, and the answer must
// not depend on how far from the origin the pair happens to sit.
TEST(a_section_is_culled_the_same_however_far_from_the_origin_it_is)
{
    // 784,427 is a chunk past the Far Lands boundary; 2,000,000 is the world's
    // horizontal limit in chunks, so it is as far out as the game can go.
    const i32 origins[] = {0, 1, -1, 784427, -784427, 2000000, -2000000};

    for (const i32 originChunk : origins) {
        Frustum f = lookingDownNegativeZ(0.1f, 512.0f);
        f.setOrigin(originChunk, originChunk);

        // Exactly the relative offsets the world-space case above uses, so the
        // expected answers are the ones already established there.
        CHECK(f.testSection(originChunk + 0, 0, originChunk - 1));
        CHECK(!f.testSection(originChunk + 0, 0, originChunk + 4));
        CHECK(!f.testSection(originChunk + 0, 0, originChunk - 60));
    }
}

// And the failure it prevents, stated directly. Without the origin -- that is,
// testing far-out sections against a frustum built at the world origin -- the
// section in front of the camera is rejected and the one behind it is
// accepted, because both are hundreds of thousands of chunks away from the
// planes. This is the case that would have emptied the screen at the Far Lands.
TEST(without_an_origin_far_out_culling_is_wrong_rather_than_merely_imprecise)
{
    const i32 far_ = 784427;
    const Frustum f = lookingDownNegativeZ(0.1f, 512.0f);

    // The frustum still has its default origin of (0, 0), so these are tested
    // 12.5 million blocks from where the camera actually is.
    CHECK(!f.testSection(far_, 0, far_ - 1));

    // The same query, with the origin set, is visible -- which is the whole
    // difference.
    Frustum g = lookingDownNegativeZ(0.1f, 512.0f);
    g.setOrigin(far_, far_);
    CHECK(g.testSection(far_, 0, far_ - 1));
}

// Section boxes are still exact out there, and this sweeps a band of them
// rather than checking three.
//
// **Doing the subtraction in float would also work, and it is worth writing
// down why rather than leaving the i32 version looking cleverer than it is.**
// A float holds integers exactly to 16,777,216 and the world stops at
// +-2,000,000 chunks, so a chunk coordinate always converts exactly and so
// does the difference; the products are multiples of 16 up to 32,000,000,
// where float spacing is 2, so those are exact too. Mutating this line to
// `float(chunkX) - float(originChunkX_)` does not fail any test here, because
// it genuinely is not wrong.
//
// The i32 form is kept because it is exact *by construction* instead of by an
// argument that depends on the world limit, the section size and the mantissa
// width all staying where they are. The precision that actually needs care is
// one level up, in block coordinates, and that is the renderer's job.
TEST(far_out_culling_agrees_with_the_origin_across_a_whole_band)
{
    Frustum near_ = lookingDownNegativeZ(0.1f, 512.0f);
    near_.setOrigin(0, 0);

    Frustum far_ = lookingDownNegativeZ(0.1f, 512.0f);
    far_.setOrigin(1999000, -1999000);

    // Sweep a band of sections and require the two to agree on every one. A
    // rounding error of even one block would flip at least one box that
    // straddles a plane.
    for (int dz = -40; dz <= 8; ++dz) {
        for (int dx = -8; dx <= 8; ++dx) {
            const bool a = near_.testSection(dx, 0, dz);
            const bool b = far_.testSection(1999000 + dx, 0, -1999000 + dz);
            CHECK_EQ(int(a), int(b));
        }
    }
}

TEST(the_two_clip_conventions_disagree_only_about_the_near_plane)
{
    const Mat4 vp = perspective(1.2f, 1.0f, 1.0f, 100.0f);

    Frustum gl;
    gl.setFromViewProjection(vp, ClipRange::NegativeOneToOne);
    Frustum d3d;
    d3d.setFromViewProjection(vp, ClipRange::ZeroToOne);

    for (int i = 0; i < Frustum::kPlaneCount; ++i) {
        if (i == Frustum::kNear) {
            continue;
        }
        CHECK_EQ(gl.plane(i).nx, d3d.plane(i).nx);
        CHECK_EQ(gl.plane(i).ny, d3d.plane(i).ny);
        CHECK_EQ(gl.plane(i).nz, d3d.plane(i).nz);
        CHECK_EQ(gl.plane(i).d, d3d.plane(i).d);
    }

    // Both near planes face the same way; they sit at different depths. Picking
    // the wrong one silently culls geometry just in front of the camera, which
    // is why the convention is a parameter and not an assumption.
    CHECK(gl.plane(Frustum::kNear).nz < 0.0f);
    CHECK(d3d.plane(Frustum::kNear).nz < 0.0f);
    CHECK(gl.plane(Frustum::kNear).d != d3d.plane(Frustum::kNear).d);
}

// The matrix citro3d's Mtx_Persp and Mtx_PerspTilt actually write, transcribed
// from their disassembly rather than from what the PICA is assumed to do:
//
//     0:  vldr s15, =0.5        s0 = fovy * 0.5
//     24: bl tanf               s19 = tan(fovy/2)
//     4c: vmul s17, s17, s16    far * near
//     60: vdiv s12, s17, s14    -> [32] = M[2][3] = far*near/(near-far)
//     68: vdiv s13, s16, s14    -> [36] = M[2][2] = near/(near-far)
//     7c: vstr s15, [r4, #52]   -> M[3][2] = -1     (right-handed)
//
// C3D_Mtx stores each row reversed -- an FVec is (w, z, y, x) -- so element
// (row, col) is m[row * 4 + (3 - col)], which is where those byte offsets come
// from. Only the Z row matters here; the tilt is an X/Y swap.
Mat4 citro3dPerspective(float fovY, float aspect, float near, float far)
{
    const float t = std::tan(fovY * 0.5f);
    Mat4 r;
    r.m[0][0] = -1.0f / (t * aspect);
    r.m[1][1] = 1.0f / t;
    r.m[2][2] = near / (near - far);
    r.m[2][3] = far * near / (near - far);
    r.m[3][2] = -1.0f;
    return r;
}

// The open question docs/status.md carried since M2: which depth range citro3d
// produces. It is neither of the two obvious answers, and only one of the three
// readings draws the world.
TEST(a_citro3d_projection_clips_between_minus_one_and_zero)
{
    const float near = 1.0f;
    const float far = 100.0f;
    const Mat4 vp = citro3dPerspective(1.2f, 400.0f / 240.0f, near, far);

    // The mapping the frustum code has to match: near -> -1, far -> 0.
    const auto depth = [&vp](float viewZ) {
        const float z = vp.m[2][2] * viewZ + vp.m[2][3];
        const float w = vp.m[3][2] * viewZ;
        return z / w;
    };
    CHECK(std::fabs(depth(-near) + 1.0f) < 1e-4f);
    CHECK(std::fabs(depth(-far)) < 1e-4f);

    Frustum pica;
    pica.setFromViewProjection(vp, ClipRange::NegativeOneToZero);

    // A box halfway down the view volume, on the axis.
    const auto boxAt = [](float z) {
        return std::pair<Vec3, Vec3>{Vec3{-1.0f, -1.0f, z - 1.0f}, Vec3{1.0f, 1.0f, z + 1.0f}};
    };
    const auto mid = boxAt(-50.0f);
    const auto behind = boxAt(20.0f);
    const auto beyond = boxAt(-200.0f);

    CHECK(pica.testBox(mid.first, mid.second));
    CHECK(!pica.testBox(behind.first, behind.second));
    CHECK(!pica.testBox(beyond.first, beyond.second));

    // Reading the same matrix as Direct3D's range takes M[2] alone for the near
    // plane, which on this matrix is the far plane negated: everything nearer
    // than the far clip is culled, i.e. the whole world.
    Frustum asD3d;
    asD3d.setFromViewProjection(vp, ClipRange::ZeroToOne);
    CHECK(!asD3d.testBox(mid.first, mid.second));

    // Reading it as OpenGL's gets the near plane right and leaves a far plane
    // that rejects nothing -- over-drawing, not blanking.
    Frustum asGl;
    asGl.setFromViewProjection(vp, ClipRange::NegativeOneToOne);
    CHECK(asGl.testBox(mid.first, mid.second));
    CHECK(asGl.testBox(beyond.first, beyond.second));
    CHECK(!asGl.testBox(behind.first, behind.second));
}

TEST(an_uninitialised_frustum_draws_everything_rather_than_nothing)
{
    // A zero matrix is a bug, but it should look like "culling is broken"
    // rather than "the world vanished" -- the first is diagnosable at a glance.
    Frustum f;
    f.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);
    CHECK(f.testBox(Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}));
    CHECK(f.testSection(100, 3, -40));
}

TEST(planes_come_back_normalised)
{
    const Frustum f = lookingDownNegativeZ();
    for (int i = 0; i < Frustum::kPlaneCount; ++i) {
        const Plane& p = f.plane(i);
        const float length = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
        CHECK(std::fabs(length - 1.0f) < 1e-5f);
    }
}
