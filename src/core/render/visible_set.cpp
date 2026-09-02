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
    baseX_ = floorMod(centreX_, edge_);
    baseZ_ = floorMod(centreZ_, edge_);
    cells_.assign(static_cast<usize>(edge_) * edge_, Cell{});
}

void SectionField::setCentre(i32 chunkX, i32 chunkZ)
{
    centreX_ = chunkX;
    centreZ_ = chunkZ;

    // The two divisions the whole class used to do per lookup, now done twice a
    // frame.
    //
    // **The guard is not decoration.** This used to be two assignments and had
    // no preconditions at all, and ChunkRenderer::setCentre forwards to it
    // without knowing whether reset() has run -- so a field that was never
    // sized would divide by zero here, on a build with no exceptions to catch
    // it. reset() recomputes both bases anyway, so leaving them at zero until
    // then loses nothing.
    if (edge_ == 0) {
        return;
    }
    baseX_ = floorMod(chunkX, edge_);
    baseZ_ = floorMod(chunkZ, edge_);
}

// **The same value floorMod(chunkX, edge_) gives, without dividing.**
//
// ARMv6k has no integer divide instruction, so `%` is an __aeabi_idivmod call,
// and this is on the walk's innermost path -- once per section visited and once
// per face stepped through, which measured out at about twenty calls a section.
//
// The identity that removes them: every caller checks inRange() first, so the
// offset from the centre is in [-radius_, +radius_], and edge_ is 2 * radius_ + 1.
// Added to a base already reduced into [0, edge_ - 1] that lands in
// [-radius_, 3 * radius_], which is one conditional correction away from
// [0, edge_ - 1] -- a subtraction cannot overshoot because 3 * radius_ - edge_
// is radius_ - 1, and an addition cannot because edge_ - radius_ is radius_ + 1.
//
// **Callers must have checked inRange.** Every one of them does: find(),
// setColumn(), cellOccupant() and cellOf() all guard on it, which is what makes
// this safe to apply to cellIndex itself rather than to a second fast variant.
int SectionField::cellIndex(i32 chunkX, i32 chunkZ) const
{
    int cx = baseX_ + static_cast<int>(chunkX - centreX_);
    if (cx < 0) {
        cx += edge_;
    } else if (cx >= edge_) {
        cx -= edge_;
    }

    int cz = baseZ_ + static_cast<int>(chunkZ - centreZ_);
    if (cz < 0) {
        cz += edge_;
    } else if (cz >= edge_) {
        cz -= edge_;
    }

    return cz * edge_ + cx;
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
    if (!sameColumn) {
        // A different column's pending remeshes are not this one's.
        cell.dirty = 0;
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

bool SectionField::sectionDirty(i32 chunkX, int sectionY, i32 chunkZ) const
{
    if (sectionY < 0 || sectionY >= kSectionsY) {
        return false;
    }
    const Cell* cell = find(chunkX, chunkZ);
    return cell != nullptr && (cell->dirty & u8(1u << sectionY)) != 0;
}

void SectionField::setSectionDirty(i32 chunkX, int sectionY, i32 chunkZ, bool dirty)
{
    if (sectionY < 0 || sectionY >= kSectionsY) {
        return;
    }
    if (Cell* cell = find(chunkX, chunkZ)) {
        const u8 bit = u8(1u << sectionY);
        cell->dirty = dirty ? u8(cell->dirty | bit) : u8(cell->dirty & ~bit);
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
    //
    // **`visited` holds a stamp, not a flag**, and that is what removes the
    // clear. Assigning zero over every entry was a memset of edge^2 * 8 bytes
    // on every frame -- 19 KB at the debug page's render distance of 24, on a
    // console whose old model has no L2 cache at all. A counter that only goes
    // up needs the array cleared once every 65,535 frames instead.
    static thread_local std::vector<u16> visited;
    static thread_local std::vector<Step> queue;
    static thread_local u16 visitStamp = 0;

    // A different render distance is a different grid, and a stamp left over
    // from the old one means nothing in it. Same clear on the wrap, so that the
    // one value that would alias a live entry never gets handed out.
    if (visited.size() != cellCount || visitStamp == 0xFFFF) {
        visited.assign(cellCount, 0);
        visitStamp = 0;
    }
    ++visitStamp;

    queue.clear();

    // **The one division left in the walk**, and it is here because the camera
    // is not guaranteed to be inside the field: the centre follows it a frame
    // late, since `WorldStreamer::update` runs after this. Once a frame is not
    // worth an identity to reason about, and the value is exactly the one
    // cellIndex() gives for a camera that is in range.
    const int cameraCell = floorMod(cameraChunkZ, edge) * edge + floorMod(cameraChunkX, edge);

    visited[static_cast<usize>(cameraCell) * SectionField::kSectionsY
            + static_cast<usize>(startY)] = visitStamp;
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

        // **One lookup for all four questions below.** Resolving the column once
        // is worth more than it looks on this hardware: the four accessors this
        // replaces each walked the grid again to reach three fields of the same
        // struct. See SectionField::column.
        const SectionField::ColumnRef col = field.column(step.chunkX, step.chunkZ);

        if (col) {
            const u16 slot = col.meshSlot(step.sectionY);
            const bool dirty = col.dirty(step.sectionY);

            if (slot == SectionField::kNoMesh) {
                // This is the whole point: reached, therefore worth meshing.
                // Anything the walk never gets to never enters this list.
                out->toMesh.push_back({step.chunkX, step.chunkZ, step.sectionY, slot});
            } else if (slot == SectionField::kEmptyMesh) {
                // Meshed, and there was nothing there. Only worth revisiting if
                // a block change since says there might be now.
                if (dirty) {
                    out->toMesh.push_back({step.chunkX, step.chunkZ, step.sectionY, slot});
                }
            } else {
                // **Both lists when it is dirty**, and that is the fix for the
                // flash: the geometry it is holding is one tick out of date, and
                // one tick out of date draws far better than not at all. See
                // SectionField::sectionDirty.
                out->draw.push_back({step.chunkX, step.chunkZ, step.sectionY, slot});
                if (dirty) {
                    out->toMesh.push_back({step.chunkX, step.chunkZ, step.sectionY, slot});
                }
            }
            // A section that meshed to nothing is in neither list. It is still
            // walked through: it is air, so it connects everything it touches.
        } else if (!isCamera) {
            // An unloaded section is not see-through -- we have no idea what is
            // in it. Traversing anyway would let the search escape the loaded
            // area and queue meshes for columns that do not exist.
            continue;
        }

        // An empty ref reads as mask 0, which is what the walk wants: an unloaded
        // column is opaque to it. The camera reaches here with entryFace < 0 and
        // never consults the mask at all.
        const SectionVisibility vis = col.visibility(step.sectionY);

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

            // cellOf() is the in-range test and the index in one, and it no
            // longer divides to produce it.
            const int cell = field.cellOf(nx, nz);
            if (cell < 0) {
                continue;
            }

            const usize index =
                static_cast<usize>(cell) * SectionField::kSectionsY + static_cast<usize>(ny);
            if (visited[index] == visitStamp) {
                continue;
            }
            visited[index] = visitStamp;

            queue.push_back(Step{nx, nz, static_cast<i16>(ny),
                                 static_cast<i16>(opposite(face))});
        }
    }
}

}  // namespace mc::render
