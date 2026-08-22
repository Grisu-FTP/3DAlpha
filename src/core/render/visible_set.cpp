#include "core/render/visible_set.hpp"

#include <cstdlib>

namespace mc::render {

using mesh::SectionVisibility;

namespace {

// Faces are numbered so that opposites differ in the low bit: -Y is 0 and +Y
// is 1, -Z is 2 and +Z is 3, -X is 4 and +X is 5. Stepping through a face means
// entering the neighbour by its opposite, which is one xor.
constexpr int opposite(int face)
{
    return face ^ 1;
}

static_assert(opposite(mesh::kFaceNegY) == mesh::kFacePosY, "");
static_assert(opposite(mesh::kFaceNegZ) == mesh::kFacePosZ, "");
static_assert(opposite(mesh::kFaceNegX) == mesh::kFacePosX, "");

int floorMod(i32 value, int modulus)
{
    const int r = static_cast<int>(value % modulus);
    return r < 0 ? r + modulus : r;
}

struct Step {
    i32 chunkX;
    i32 chunkZ;
    i16 sectionY;
    i16 entryFace;  // -1 at the camera, which may be left by any face
};

}  // namespace

void SectionField::reset(int radius)
{
    radius_ = radius < 0 ? 0 : radius;
    edge_ = radius_ * 2 + 1;
    cells_.assign(static_cast<usize>(edge_) * edge_, Cell{});
}

int SectionField::cellIndex(i32 chunkX, i32 chunkZ) const
{
    return floorMod(chunkZ, edge_) * edge_ + floorMod(chunkX, edge_);
}

bool SectionField::inRange(i32 chunkX, i32 chunkZ) const
{
    if (edge_ == 0) {
        return false;
    }
    const i64 dx = i64(chunkX) - centreX_;
    const i64 dz = i64(chunkZ) - centreZ_;
    return std::llabs(dx) <= radius_ && std::llabs(dz) <= radius_;
}

const SectionField::Cell* SectionField::find(i32 chunkX, i32 chunkZ) const
{
    if (!inRange(chunkX, chunkZ)) {
        return nullptr;
    }
    const Cell& cell = cells_[cellIndex(chunkX, chunkZ)];
    if (!cell.occupied || cell.chunkX != chunkX || cell.chunkZ != chunkZ) {
        return nullptr;
    }
    return &cell;
}

SectionField::Cell* SectionField::find(i32 chunkX, i32 chunkZ)
{
    const Cell* cell = static_cast<const SectionField*>(this)->find(chunkX, chunkZ);
    return const_cast<Cell*>(cell);
}

bool SectionField::isLoaded(i32 chunkX, i32 chunkZ) const
{
    return find(chunkX, chunkZ) != nullptr;
}

void SectionField::setColumn(i32 chunkX, i32 chunkZ, const SectionVisibility* perSection)
{
    if (!inRange(chunkX, chunkZ)) {
        return;
    }

    Cell& cell = cells_[cellIndex(chunkX, chunkZ)];
    const bool sameColumn = cell.occupied && cell.chunkX == chunkX && cell.chunkZ == chunkZ;

    cell.chunkX = chunkX;
    cell.chunkZ = chunkZ;
    cell.occupied = true;

    for (int sy = 0; sy < kSectionsY; ++sy) {
        cell.visibility[sy] = perSection[sy].mask();
        // A different column moving into this cell brings no meshes with it.
        // Re-publishing the same column keeps them, which is what a lighting
        // update or a neighbour arriving should do.
        //
        // The slots the outgoing column held are the caller's to hand back
        // first -- see cellOccupant(). Forgetting them here without releasing
        // them would leak the VBO memory for as long as the pool ran.
        if (!sameColumn) {
            cell.mesh[sy] = kNoMesh;
        }
    }
}

bool SectionField::cellOccupant(i32 chunkX, i32 chunkZ, i32* outChunkX, i32* outChunkZ) const
{
    if (!inRange(chunkX, chunkZ)) {
        return false;
    }
    const Cell& cell = cells_[cellIndex(chunkX, chunkZ)];
    if (!cell.occupied) {
        return false;
    }
    *outChunkX = cell.chunkX;
    *outChunkZ = cell.chunkZ;
    return true;
}

void SectionField::clearColumn(i32 chunkX, i32 chunkZ)
{
    if (Cell* cell = find(chunkX, chunkZ)) {
        *cell = Cell{};
    }
}

SectionVisibility SectionField::visibility(i32 chunkX, int sectionY, i32 chunkZ) const
{
    const Cell* cell = find(chunkX, chunkZ);
    if (cell == nullptr || sectionY < 0 || sectionY >= kSectionsY) {
        return SectionVisibility(0);
    }
    return SectionVisibility(cell->visibility[sectionY]);
}

u16 SectionField::meshSlot(i32 chunkX, int sectionY, i32 chunkZ) const
{
    const Cell* cell = find(chunkX, chunkZ);
    if (cell == nullptr || sectionY < 0 || sectionY >= kSectionsY) {
        return kNoMesh;
    }
    return cell->mesh[sectionY];
}

void SectionField::setMeshSlot(i32 chunkX, int sectionY, i32 chunkZ, u16 slot)
{
    if (sectionY < 0 || sectionY >= kSectionsY) {
        return;
    }
    if (Cell* cell = find(chunkX, chunkZ)) {
        cell->mesh[sectionY] = slot;
    }
}

void buildVisibleSet(const SectionField& field, const Frustum& frustum,
                     i32 cameraChunkX, int cameraSectionY, i32 cameraChunkZ,
                     VisibleSet* out)
{
    out->clear();

    const int edge = field.edge();
    if (edge == 0) {
        return;
    }

    // Clamp rather than reject: a camera above the build limit or below bedrock
    // still has to see the world, and the section it is nearest to is the right
    // place to start the walk from.
    const int startY = cameraSectionY < 0 ? 0
                       : cameraSectionY >= SectionField::kSectionsY
                           ? SectionField::kSectionsY - 1
                           : cameraSectionY;

    const usize cellCount = static_cast<usize>(edge) * edge * SectionField::kSectionsY;

    // Both of these keep their capacity across frames, so a steady render
    // distance means no allocation in the render loop at all.
    static thread_local std::vector<u8> visited;
    static thread_local std::vector<Step> queue;

    visited.assign(cellCount, 0);
    queue.clear();

    const auto visitedIndex = [edge](i32 chunkX, int sectionY, i32 chunkZ) {
        return (static_cast<usize>(floorMod(chunkZ, edge)) * edge + floorMod(chunkX, edge))
                   * SectionField::kSectionsY
               + static_cast<usize>(sectionY);
    };

    visited[visitedIndex(cameraChunkX, startY, cameraChunkZ)] = 1;
    queue.push_back(Step{cameraChunkX, cameraChunkZ, static_cast<i16>(startY), -1});

    for (usize head = 0; head < queue.size(); ++head) {
        const Step step = queue[head];
        const bool isCamera = head == 0;

        ++out->sectionsVisited;

        // The camera's own section is exempt from the frustum: the near plane
        // cuts through it, and dropping it would blank the block you are
        // standing in.
        if (!isCamera && !frustum.testSection(step.chunkX, step.sectionY, step.chunkZ)) {
            ++out->rejectedByFrustum;
            continue;
        }

        if (field.isLoaded(step.chunkX, step.chunkZ)) {
            const u16 slot = field.meshSlot(step.chunkX, step.sectionY, step.chunkZ);
            if (slot == SectionField::kNoMesh) {
                // This is the whole point: reached, therefore worth meshing.
                // Anything the walk never gets to never enters this list.
                out->toMesh.push_back({step.chunkX, step.chunkZ, step.sectionY, slot});
            } else if (slot != SectionField::kEmptyMesh) {
                out->draw.push_back({step.chunkX, step.chunkZ, step.sectionY, slot});
            }
            // A section that meshed to nothing is in neither list. It is still
            // walked through: it is air, so it connects everything it touches.
        } else if (!isCamera) {
            // An unloaded section is not see-through -- we have no idea what is
            // in it. Traversing anyway would let the search escape the loaded
            // area and queue meshes for columns that do not exist.
            continue;
        }

        const SectionVisibility vis =
            field.visibility(step.chunkX, step.sectionY, step.chunkZ);

        for (int face = 0; face < mesh::kFaceCount; ++face) {
            // entryFace < 0 is the camera's own section, and it may be left by
            // any face. The mask cannot help there: it says which faces are
            // mutually reachable, not which of the section's air volumes the
            // camera happens to be standing in. Consulting it would blank the
            // world whenever the player stood in a pocket that reaches only one
            // face -- a covered doorway, the bottom of a shaft. Over-drawing the
            // six neighbours is the cheap direction to be wrong in.
            if (step.entryFace >= 0 && !vis.connects(step.entryFace, face)) {
                continue;
            }

            const mesh::FaceOffset& offset = mesh::kFaceOffset[face];
            const i32 nx = step.chunkX + offset.dx;
            const int ny = step.sectionY + offset.dy;
            const i32 nz = step.chunkZ + offset.dz;

            if (ny < 0 || ny >= SectionField::kSectionsY) {
                continue;
            }
            if (!field.inRange(nx, nz)) {
                continue;
            }

            const usize index = visitedIndex(nx, ny, nz);
            if (visited[index]) {
                continue;
            }
            visited[index] = 1;

            queue.push_back(Step{nx, nz, static_cast<i16>(ny),
                                 static_cast<i16>(opposite(face))});
        }
    }
}

}  // namespace mc::render
