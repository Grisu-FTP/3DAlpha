#pragma once

// Deciding what to draw, and -- the part the measurement forced -- what to
// mesh.
//
// Meshing every section of every loaded column costs 91 KB per column on a real
// world, which is 25.7 MB at render distance 8 against a ~12 MB VBO budget. It
// misses even at distance 6. Seventy-three percent of that geometry is below
// y=64: cave walls that are never on screen. So the same breadth-first walk
// that picks the draw list also produces the mesh queue, nearest first, and a
// section the walk never reaches keeps its block data and never gets a VBO.
// See docs/3ds-performance.md section 4.
//
// Nothing here allocates during a frame: the field is sized once at the render
// distance, and both output lists keep their capacity between frames.

#include "core/mesh/visibility.hpp"
#include "core/util/frustum.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <vector>

namespace mc::render {

// Per-section state for every column within the render distance, addressed by
// absolute chunk coordinate.
//
// The grid wraps: a column lands at (x mod edge, z mod edge), and each cell
// remembers which column it actually holds. Moving the centre is then a single
// assignment with no eviction pass, because a cell whose stored coordinates no
// longer match what is asked for is stale by definition.
class SectionField {
public:
    static constexpr int kSectionsY = world::ChunkColumn::kSectionCount;

    // radius is in chunks; the grid is (2 * radius + 1) square.
    void reset(int radius);

    int radius() const { return radius_; }
    int edge() const { return edge_; }

    void setCentre(i32 chunkX, i32 chunkZ)
    {
        centreX_ = chunkX;
        centreZ_ = chunkZ;
    }

    i32 centreX() const { return centreX_; }
    i32 centreZ() const { return centreZ_; }

    // Inside the square of columns the field can hold. Chebyshev distance,
    // matching how render distance is counted in every Minecraft version.
    bool inRange(i32 chunkX, i32 chunkZ) const;

    // A column is present when it is in range and the cell it maps to is the
    // one holding it.
    bool isLoaded(i32 chunkX, i32 chunkZ) const;

    // Publishes a column's per-section visibility masks, claiming its cell.
    void setColumn(i32 chunkX, i32 chunkZ, const mesh::SectionVisibility* perSection);
    void clearColumn(i32 chunkX, i32 chunkZ);

    mesh::SectionVisibility visibility(i32 chunkX, int sectionY, i32 chunkZ) const;

    // A section is in one of three states, and the third is what stops the
    // renderer meshing the same nothing every frame: 36 % of sections are
    // uniform air and another 15 % of the ones worth meshing come out with no
    // geometry at all. A bool could not tell "not meshed yet" from "meshed, and
    // there was nothing there", so those sections went back into the mesh queue
    // on every single frame.
    static constexpr u16 kNoMesh = 0xFFFF;     // never meshed
    static constexpr u16 kEmptyMesh = 0xFFFE;  // meshed, produced no geometry

    // kNoMesh for a section of a column that is not loaded, so an absent column
    // reads the same as an unmeshed one.
    u16 meshSlot(i32 chunkX, int sectionY, i32 chunkZ) const;
    void setMeshSlot(i32 chunkX, int sectionY, i32 chunkZ, u16 slot);

    // Whether the section has been through the mesher at all, which is what the
    // walk needs -- an empty section is finished with, not pending.
    bool hasMesh(i32 chunkX, int sectionY, i32 chunkZ) const
    {
        return meshSlot(chunkX, sectionY, chunkZ) != kNoMesh;
    }

    // The column a cell is holding, which need not be the one asked for: the
    // grid wraps, so a column moving in evicts whatever shared its cell. The
    // renderer asks before publishing, because the outgoing column's meshes are
    // still holding VBO memory that only it can hand back.
    bool cellOccupant(i32 chunkX, i32 chunkZ, i32* outChunkX, i32* outChunkZ) const;

    // Which cell of the wrapped grid a column lands in, or -1 out of range.
    // Stable under a moving centre -- it is the column's coordinates modulo the
    // edge, nothing else -- which is what lets it name a section in a token the
    // VBO pool hands back an unknown number of frames later.
    int cellOf(i32 chunkX, i32 chunkZ) const
    {
        return inRange(chunkX, chunkZ) ? cellIndex(chunkX, chunkZ) : -1;
    }

    int cellCount() const { return edge_ * edge_; }

private:
    struct Cell {
        i32 chunkX = 0;
        i32 chunkZ = 0;
        bool occupied = false;
        u16 visibility[kSectionsY] = {};
        u16 mesh[kSectionsY];

        Cell()
        {
            for (u16& slot : mesh) {
                slot = kNoMesh;
            }
        }
    };

    int cellIndex(i32 chunkX, i32 chunkZ) const;
    const Cell* find(i32 chunkX, i32 chunkZ) const;
    Cell* find(i32 chunkX, i32 chunkZ);

    int radius_ = 0;
    int edge_ = 0;
    i32 centreX_ = 0;
    i32 centreZ_ = 0;
    std::vector<Cell> cells_;
};

struct VisibleSection {
    i32 chunkX;
    i32 chunkZ;
    i16 sectionY;

    // Only meaningful in `draw`, where it is the VBO slot the geometry is in.
    // Carrying it here saves the caller a second field lookup per drawn section
    // in the one loop that runs on every frame.
    u16 slot = SectionField::kNoMesh;
};

struct VisibleSet {
    // Reached, loaded and holding geometry. Breadth-first order, which is
    // near-enough front-to-back for the early-depth test to pay off.
    std::vector<VisibleSection> draw;

    // Reached but holding no mesh. Nearest first, so a caller that can afford
    // three meshes this frame builds the three that matter most.
    std::vector<VisibleSection> toMesh;

    int sectionsVisited = 0;
    int rejectedByFrustum = 0;

    void clear()
    {
        draw.clear();
        toMesh.clear();
        sectionsVisited = 0;
        rejectedByFrustum = 0;
    }
};

// Walks outward from the camera's section through the visibility graph,
// stepping into a neighbour only when the section's own mask says the face it
// entered by connects to the face it wants to leave by.
//
// The camera's section is always included even when it fails the frustum test
// or is not loaded: you are standing inside it.
void buildVisibleSet(const SectionField& field, const Frustum& frustum,
                     i32 cameraChunkX, int cameraSectionY, i32 cameraChunkZ,
                     VisibleSet* out);

}  // namespace mc::render
