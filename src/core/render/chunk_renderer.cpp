#include "core/render/chunk_renderer.hpp"

#include "core/mesh/vertex.hpp"

namespace mc::render {

void ChunkRenderer::reset(VboAllocator* allocator, const ChunkRendererConfig& config)
{
    config_ = config;

    field_.reset(config_.meshDistance);
    residents_.assign(usize(field_.cellCount()) * SectionField::kSectionsY, Resident{});

    // The largest single mesh the shared index buffer can draw. A class table
    // that stopped short of it would leave a legal section with nowhere to go.
    const usize largest = usize(mesh::kMaxQuadsPerSection) * 4 * sizeof(mesh::WorldVertex);
    classes_.build(kSmallestSizeClass, largest, config_.sizeClassRatio);

    pool_.reset(allocator, config_.budget, classes_);

    visible_.clear();
    meshQueue_.clear();
    frameStats_ = FrameStats{};
}

void ChunkRenderer::shutdown()
{
    pool_.shutdown();
    residents_.assign(residents_.size(), Resident{});
    visible_.clear();
    meshQueue_.clear();
}

void ChunkRenderer::setCentre(i32 chunkX, i32 chunkZ)
{
    field_.setCentre(chunkX, chunkZ);
}

void ChunkRenderer::releaseSection(int cell, int sectionY)
{
    Resident& resident = residents_[tokenFor(cell, sectionY)];
    if (!resident.live) {
        return;
    }

    pool_.release(resident.slot);
    resident.live = false;

    // Best effort: a column that has already fallen out of range answers
    // nothing here, and does not need to -- publishColumn is about to overwrite
    // the cell wholesale. What matters is that the block went back.
    field_.setMeshSlot(resident.chunkX, sectionY, resident.chunkZ, SectionField::kNoMesh);
}

void ChunkRenderer::releaseCell(int cell)
{
    for (int sy = 0; sy < SectionField::kSectionsY; ++sy) {
        releaseSection(cell, sy);
    }
}

bool ChunkRenderer::publishColumn(i32 chunkX, i32 chunkZ, const mesh::SectionVisibility* perSection)
{
    const int cell = field_.cellOf(chunkX, chunkZ);
    if (cell < 0) {
        return false;
    }

    // The grid wraps, so publishing here can displace a column an entire render
    // distance away. Its meshes are still in the pool and only this cell knows
    // about them, so they have to go back before the field forgets which column
    // they belonged to.
    i32 occupantX = 0;
    i32 occupantZ = 0;
    if (field_.cellOccupant(chunkX, chunkZ, &occupantX, &occupantZ)
        && (occupantX != chunkX || occupantZ != chunkZ)) {
        releaseCell(cell);
    }

    field_.setColumn(chunkX, chunkZ, perSection);
    return true;
}

void ChunkRenderer::dropColumn(i32 chunkX, i32 chunkZ)
{
    const int cell = field_.cellOf(chunkX, chunkZ);
    if (cell >= 0 && field_.isLoaded(chunkX, chunkZ)) {
        releaseCell(cell);
    }
    field_.clearColumn(chunkX, chunkZ);
}

void ChunkRenderer::invalidateSection(i32 chunkX, int sectionY, i32 chunkZ)
{
    const int cell = field_.cellOf(chunkX, chunkZ);
    if (cell < 0 || sectionY < 0 || sectionY >= SectionField::kSectionsY) {
        return;
    }
    if (!field_.isLoaded(chunkX, chunkZ)) {
        return;
    }
    releaseSection(cell, sectionY);
    // An empty section holds no slot and so no resident record, but it is still
    // marked as done and has to be put back in the queue by hand.
    field_.setMeshSlot(chunkX, sectionY, chunkZ, SectionField::kNoMesh);
}

void ChunkRenderer::invalidateColumn(i32 chunkX, i32 chunkZ)
{
    for (int sy = 0; sy < SectionField::kSectionsY; ++sy) {
        invalidateSection(chunkX, sy, chunkZ);
    }
}

// Anything the pool took back on its own initiative. Draining has to happen
// after *every* upload rather than once at the end of the frame, because the
// caller may be about to draw from a slot that has just changed hands -- and an
// upload that returns kNoSlot can still have evicted on its way to giving up.
void ChunkRenderer::drainEvictions()
{
    for (u32 token : pool_.evicted()) {
        Resident& resident = residents_[token];
        if (!resident.live) {
            continue;
        }
        const int sectionY = int(token % SectionField::kSectionsY);
        field_.setMeshSlot(resident.chunkX, sectionY, resident.chunkZ, SectionField::kNoMesh);
        resident.live = false;
    }
    pool_.clearEvicted();
}

void ChunkRenderer::beginFrame(u32 frame, const Frustum& frustum, i32 cameraChunkX,
                               int cameraSectionY, i32 cameraChunkZ)
{
    pool_.beginFrame(frame);
    buildVisibleSet(field_, frustum, cameraChunkX, cameraSectionY, cameraChunkZ, &visible_);

    frameStats_ = FrameStats{};
    frameStats_.sectionsVisited = visible_.sectionsVisited;
    frameStats_.rejectedByFrustum = visible_.rejectedByFrustum;
    frameStats_.drawn = int(visible_.draw.size());
    frameStats_.queued = int(visible_.toMesh.size());

    // Touch before uploading, never after. A slot that is drawn this frame but
    // not yet touched is the pool's best eviction candidate, so an upload
    // between the walk and the touch can take memory out from under geometry
    // the frame is about to read.
    for (const VisibleSection& section : visible_.draw) {
        pool_.touch(section.slot);
        frameStats_.drawnBytes += pool_.size(section.slot);
    }

    meshQueue_.clear();
    const int budget = config_.meshBudgetPerFrame;
    const int take = budget <= 0 ? int(visible_.toMesh.size())
                                 : (budget < int(visible_.toMesh.size())
                                        ? budget
                                        : int(visible_.toMesh.size()));
    meshQueue_.assign(visible_.toMesh.begin(), visible_.toMesh.begin() + take);
}

bool ChunkRenderer::uploadSection(i32 chunkX, int sectionY, i32 chunkZ, const void* vertices,
                                  const mesh::MeshRanges& ranges)
{
    const int cell = field_.cellOf(chunkX, chunkZ);
    if (cell < 0 || sectionY < 0 || sectionY >= SectionField::kSectionsY) {
        return false;
    }
    // The column may have been unloaded or replaced while the mesh was being
    // built. Dropping the result is right: whatever replaced it will be asked
    // for its own.
    if (!field_.isLoaded(chunkX, chunkZ)) {
        return false;
    }

    // Re-meshing a section that already holds one: give the old block back
    // first, or it is leaked for the lifetime of the pool.
    releaseSection(cell, sectionY);

    const u32 token = tokenFor(cell, sectionY);
    const u16 slot = pool_.upload(vertices, token, ranges);

    // Before checking the result: a refused upload can still have evicted.
    drainEvictions();

    if (slot == VboPool::kNoSlot) {
        ++frameStats_.refused;
        return false;
    }

    residents_[token] = Resident{chunkX, chunkZ, slot, true};
    field_.setMeshSlot(chunkX, sectionY, chunkZ, slot);
    ++frameStats_.uploaded;
    return true;
}

void ChunkRenderer::noteEmptySection(i32 chunkX, int sectionY, i32 chunkZ)
{
    const int cell = field_.cellOf(chunkX, chunkZ);
    if (cell < 0 || sectionY < 0 || sectionY >= SectionField::kSectionsY) {
        return;
    }
    if (!field_.isLoaded(chunkX, chunkZ)) {
        return;
    }
    releaseSection(cell, sectionY);
    field_.setMeshSlot(chunkX, sectionY, chunkZ, SectionField::kEmptyMesh);
    ++frameStats_.empty;
}

}  // namespace mc::render
