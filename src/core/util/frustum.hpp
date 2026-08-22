#pragma once

// Frustum culling for axis-aligned boxes, which on this project means sections.
//
// The PICA has no occlusion queries, so everything that is not drawn has to be
// rejected on the CPU. The frustum is the cheap half of that (the visibility
// graph in core/mesh/visibility.hpp is the half that handles caves), and it is
// computed once per frame and shared by both stereo eyes with a slightly
// widened field of view -- see docs/architecture.md.

#include "core/util/math.hpp"
#include "core/util/types.hpp"

namespace mc {

// Which depth range the projection matrix maps the view volume onto.
//
// This is not decoration. OpenGL clips against -w <= z <= w and Direct3D
// against 0 <= z <= w, and extracting the near plane with the wrong assumption
// gives a frustum that quietly culls geometry in front of the camera.
//
// The PICA is a third case, and it was settled by disassembling citro3d's
// Mtx_Persp and Mtx_PerspTilt rather than by reasoning about what the hardware
// "should" do. Both write, for a right-handed projection:
//
//     M[2][2] = near / (near - far)
//     M[2][3] = far * near / (near - far)
//     M[3][2] = -1
//
// which puts the near plane at z/w = -1 and the far plane at z/w = 0. So the
// visible range is -w <= z <= 0: the near plane is OpenGL's, and the far plane
// is not either convention's.
//
// Getting this wrong is not symmetric. Reading a citro3d matrix as ZeroToOne
// takes M[2] alone as the near plane, which is the *far* plane negated -- it
// culls everything nearer than the far clip, i.e. the entire world. Reading it
// as NegativeOneToOne gets the near plane right and produces a far plane that
// never rejects anything, so it over-draws instead. Neither is what we want,
// hence the third entry.
enum class ClipRange {
    NegativeOneToOne,   // OpenGL:    -w <= z <= w
    ZeroToOne,          // Direct3D:   0 <= z <= w
    NegativeOneToZero,  // PICA, i.e. everything citro3d's Mtx_Persp* family builds
};

class Frustum {
public:
    enum { kLeft, kRight, kBottom, kTop, kNear, kFar, kPlaneCount };

    // Gribb-Hartmann: the clip-space plane equations fall out of sums and
    // differences of the view-projection matrix's rows.
    void setFromViewProjection(const Mat4& viewProjection, ClipRange range);

    // The planes can also be handed over directly, which is what a platform
    // that already has them should do rather than round-tripping a matrix.
    void setPlane(int index, const Plane& plane) { planes_[index] = plane; }
    const Plane& plane(int index) const { return planes_[index]; }

    // **Which chunk the planes are measured from.** The renderer builds its
    // view-projection relative to the camera's own chunk rather than to the
    // world origin -- see docs/architecture.md, "Rendering far from the
    // origin" -- so the frustum that falls out of that matrix is in the same
    // relative space, and testSection has to subtract the same origin before
    // it converts a chunk coordinate to float.
    //
    // Defaulting to (0, 0) keeps the plain world-space use working unchanged,
    // which is what the frustum tests and the host harness rely on.
    void setOrigin(i32 chunkX, i32 chunkZ)
    {
        originChunkX_ = chunkX;
        originChunkZ_ = chunkZ;
    }

    // Conservative: a box that straddles a plane counts as visible. Uses the
    // positive vertex only -- the corner furthest along each normal -- so the
    // test is six dot products, not twenty-four.
    bool testBox(const Vec3& min, const Vec3& max) const;

    // The box of one 16^3 section, named in absolute chunk coordinates and
    // tested relative to the origin above. **The subtraction happens in i32,
    // before anything becomes a float, and that is the entire point**: at the
    // Far Lands a section origin is around 12,550,000, where a float's spacing
    // is a whole block, so a box built in world coordinates and tested against
    // planes built in world coordinates culls to the nearest block or two.
    // Both chunk coordinates are bounded by the world limit of +-2,000,000
    // chunks, so the difference cannot overflow.
    bool testSection(i32 chunkX, int sectionY, i32 chunkZ) const;

private:
    Plane planes_[kPlaneCount];
    i32 originChunkX_ = 0;
    i32 originChunkZ_ = 0;
};

}  // namespace mc
