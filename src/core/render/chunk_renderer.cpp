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

    // **The block stays.** This used to call releaseSection and set the slot to
    // kNoMesh, which is how a block change made the section vanish for a frame:
    // the draw list is built at the top of the frame and the remesh runs after
    // it, so the replacement geometry could not reach a draw list until the
    // frame after next. Marking it instead keeps the old mesh drawable -- one
    // tick stale -- until its replacement is actually in hand. See
    // SectionField::sectionDirty and uploadSection.
    //
    // This also covers kEmptyMesh, which holds no slot and no resident record
    // but is still marked as done; the dirty bit is what puts it back in the
    // queue.
    field_.setSectionDirty(chunkX, sectionY, chunkZ, true);
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
    for (VisibleSection& section : visible_.draw) {
        pool_.touch(section.slot);
        section.slotGeneration = pool_.generation(section.slot);
        frameStats_.drawnBytes += pool_.size(section.slot);
    }

    // **Sections the player just changed go before sections that are merely
    // new.** `toMesh` comes out of the walk nearest-first, which is the right
    // order for filling in a world as it streams and the wrong one for a block
    // that was edited: a fluid front twenty metres away would queue behind
    // every unmeshed section closer to the camera and stay a tick stale for as
    // long as the streamer had work. A re-mesh is distinguished from a first
    // mesh by already holding a slot -- kNoMesh means it has never been through
    // the mesher.
    //
    // Two stable passes rather than a sort: the relative order within each
    // group is the walk's, which is the near-to-far priority that group wants
    // anyway, and there is no comparator to get wrong.
    meshQueue_.clear();
    const int budget = config_.meshBudgetPerFrame;
    const usize take = budget <= 0
                           ? visible_.toMesh.size()
                           : (usize(budget) < visible_.toMesh.size() ? usize(budget)
                                                                    : visible_.toMesh.size());

    for (const VisibleSection& section : visible_.toMesh) {
        if (meshQueue_.size() >= take) break;
        if (section.slot != SectionField::kNoMesh) meshQueue_.push_back(section);
    }
    for (const VisibleSection& section : visible_.toMesh) {
        if (meshQueue_.size() >= take) break;
        if (section.slot == SectionField::kNoMesh) meshQueue_.push_back(section);
    }
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

    // **Upload first, give the old block back second.** The obvious order --
    // release, then upload into the block that just came free -- is what let a
    // refused upload leave the section with nothing to draw, and it is also
    // what handed the section its own in-flight block back. Uploading first
    // costs one extra block of pool for the duration of the call and means a
    // pool too full to place the new mesh keeps the old one on screen.
    //
    // The old block cannot be evicted underneath this: it was drawn this frame,
    // so beginFrame touched it, and VboPool refuses to evict anything the GPU
    // may still be reading (VboPool::kRetireFrames).
    const u32 token = tokenFor(cell, sectionY);
    const u16 previousSlot = residents_[token].slot;
    const bool hadPrevious = residents_[token].live;

    const u16 slot = pool_.upload(vertices, token, ranges);

    // Before checking the result: a refused upload can still have evicted.
    drainEvictions();

    if (slot == VboPool::kNoSlot) {
        ++frameStats_.refused;
        // The section keeps whatever it was drawing and stays dirty, so the
        // next frame tries again.
        return false;
    }

    // Only if it is still ours. drainEvictions above may have taken it back to
    // make room for this very upload, in which case it is already parked and
    // releasing again would double-free it onto the list.
    if (hadPrevious && residents_[token].live && residents_[token].slot == previousSlot) {
        pool_.release(previousSlot);
    }

    residents_[token] = Resident{chunkX, chunkZ, slot, true};
    field_.setMeshSlot(chunkX, sectionY, chunkZ, slot);
    field_.setSectionDirty(chunkX, sectionY, chunkZ, false);

    // **Point this frame's draw list at the block that now holds the mesh.**
    //
    // The list was built by beginFrame, before this upload, and it names the
    // *old* slot -- which the release above has just handed back, bumping its
    // generation. Left alone, drawPass would find the generation changed, skip
    // the section, and draw nothing at all for it: a hole on the very frame the
    // new geometry became available, on every single remesh. That is the flash,
    // reintroduced by the staleness guard that was supposed to prevent a
    // different bug.
    //
    // Repointing is better than either alternative. The section draws this
    // frame, from the new mesh, one frame earlier than the old
    // draw-the-stale-block behaviour managed -- and the guard keeps doing its
    // job for slots that genuinely changed hands, which is what it is for.
    //
    // A linear scan over the draw list, a handful of times a frame: the mesh
    // budget bounds how often this runs, and the list is a few hundred entries
    // of three integers.
    for (VisibleSection& drawn : visible_.draw) {
        if (drawn.chunkX == chunkX && drawn.chunkZ == chunkZ
            && drawn.sectionY == i16(sectionY)) {
            drawn.slot = slot;
            drawn.slotGeneration = pool_.generation(slot);
            break;
        }
    }

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
    // Nothing to keep drawing here: the section really is empty now, so the
    // block goes back and the hole is the correct picture.
    releaseSection(cell, sectionY);
    field_.setMeshSlot(chunkX, sectionY, chunkZ, SectionField::kEmptyMesh);
    field_.setSectionDirty(chunkX, sectionY, chunkZ, false);
    ++frameStats_.empty;
}

}  // namespace mc::render
