#include "core/mesh/visibility.hpp"

#include "core/block/registry.hpp"

#include <cstring>

namespace mc::mesh {

using world::Section;

namespace {

constexpr int kEdge = Section::kSize;
constexpr int kLast = kEdge - 1;

// Which of the six faces this cell sits against. A cell in a corner touches
// three, and a section one block wide would touch opposite faces at once --
// both are ordinary, so this returns a set rather than a single face.
constexpr u8 touchedFaces(int x, int y, int z)
{
    u8 faces = 0;
    if (y == 0) faces |= 1u << kFaceNegY;
    if (y == kLast) faces |= 1u << kFacePosY;
    if (z == 0) faces |= 1u << kFaceNegZ;
    if (z == kLast) faces |= 1u << kFacePosZ;
    if (x == 0) faces |= 1u << kFaceNegX;
    if (x == kLast) faces |= 1u << kFacePosX;
    return faces;
}

}  // namespace

SectionVisibility computeVisibility(const Section& section, VisibilityScratch& scratch)
{
    // A uniform section answers without touching the scratch at all, and that
    // is the overwhelmingly common case: most of a loaded world is uniform air
    // above the terrain and uniform stone below it.
    if (section.isUniform()) {
        return SectionVisibility(block::isOpaque(section.uniformBlock()) ? 0 : kVisibilityAll);
    }

    std::memset(scratch.visited, 0, sizeof(scratch.visited));

    u16 mask = 0;

    for (int x = 0; x < kEdge; ++x) {
        for (int z = 0; z < kEdge; ++z) {
            for (int y = 0; y < kEdge; ++y) {
                const int start = Section::index(x, y, z);
                if (scratch.visited[start] || block::isOpaque(section.block(start))) {
                    continue;
                }

                // Flood this connected volume, accumulating which faces it
                // reaches. Depth-first with an explicit stack: the recursion
                // depth here can be 4096 and these threads have small stacks.
                u8 reached = 0;
                int top = 0;
                scratch.stack[top++] = static_cast<u16>(start);
                scratch.visited[start] = 1;

                while (top > 0) {
                    const int cell = scratch.stack[--top];

                    // Unpack the Y-fastest index rather than carrying x,y,z on
                    // the stack -- it keeps each entry to two bytes.
                    const int cy = cell % kEdge;
                    const int cz = (cell / kEdge) % kEdge;
                    const int cx = cell / (kEdge * kEdge);

                    reached |= touchedFaces(cx, cy, cz);

                    for (int face = 0; face < kFaceCount; ++face) {
                        const FaceOffset& offset = kFaceOffset[face];
                        const int nx = cx + offset.dx;
                        const int ny = cy + offset.dy;
                        const int nz = cz + offset.dz;

                        if (nx < 0 || nx >= kEdge || ny < 0 || ny >= kEdge
                            || nz < 0 || nz >= kEdge) {
                            continue;
                        }

                        const int next = Section::index(nx, ny, nz);
                        if (scratch.visited[next] || block::isOpaque(section.block(next))) {
                            continue;
                        }

                        scratch.visited[next] = 1;
                        scratch.stack[top++] = static_cast<u16>(next);
                    }
                }

                // Every pair of faces this one volume reaches is now connected.
                for (int a = 0; a < kFaceCount; ++a) {
                    if ((reached & (1u << a)) == 0) {
                        continue;
                    }
                    for (int b = a + 1; b < kFaceCount; ++b) {
                        if ((reached & (1u << b)) != 0) {
                            mask |= static_cast<u16>(1u << visibilityBit(a, b));
                        }
                    }
                }
            }
        }
    }

    return SectionVisibility(mask);
}

}  // namespace mc::mesh
