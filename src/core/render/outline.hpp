#pragma once

// The box drawn around whatever the crosshair is on, as triangles.
//
// **Triangles, because the PICA200 has no line primitive.** a1.1.2 draws this
// with `GL_LINE_STRIP` at one pixel wide; the 3DS GPU offers triangles, strips,
// fans and the geometry primitive and nothing else, so each of the twelve edges
// becomes a long thin box. That is a deviation and it is the only one available:
// the alternative is not drawing the selection at all.
//
// The consequence is that our edges have a **thickness in world units** rather
// than in pixels, so they thin out with distance where the original's would not.
// At a four-block reach that is the difference between two pixels and one.
//
// **The 0.002 expansion is the original's**, and it is a float widened to a
// double -- `RenderGlobal` calls `getSelectedBoundingBoxFromPool(...).expand(f,
// f, f)` with `f = 0.002f`. It is what stops the outline z-fighting with the
// face it is drawn against.
//
// Positions come out relative to a caller-supplied origin, because the renderer
// works in chunk-relative coordinates -- a float world coordinate snaps to the
// block grid out at the Far Lands. The subtraction happens in double and only
// the small result becomes a float.
//
// Generates nothing dynamic: the caller owns the storage and this fills it.

#include "core/util/aabb.hpp"
#include "core/util/types.hpp"

namespace mc::render {

struct OutlineVertex {
    float x, y, z;
};

inline constexpr int kOutlineEdges = 12;

// A cuboid per edge: six faces, two triangles each, three vertices each.
inline constexpr int kOutlineVerticesPerEdge = 36;
inline constexpr int kOutlineVertexCount = kOutlineEdges * kOutlineVerticesPerEdge;

// `RenderGlobal`'s own 0.002f, widened exactly as the original widens it.
inline constexpr double kOutlineExpand = 0.0020000000949949026;

// Half-thickness of an edge, in blocks. **Ours, not the game's** -- a line has
// no thickness in world units, so there is nothing to copy. 1/128 of a block
// puts an edge at roughly two pixels at arm's length on a 400-pixel-wide top
// screen, and one pixel at the far end of a four-block reach.
inline constexpr double kOutlineHalfThickness = 0.0078125;

// Fills `out` with exactly kOutlineVertexCount vertices and returns how many it
// wrote, or 0 if `max` is too small. `box` is in world coordinates -- add the
// block position to what block::selectionBox returns before calling.
int buildOutline(const AABB& box, double originX, double originY, double originZ,
                 OutlineVertex* out, int max);

}  // namespace mc::render
