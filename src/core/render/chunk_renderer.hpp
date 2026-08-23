#pragma once

// One frame of the world renderer, with no GPU in it.
//
// The field says what exists, the walk says what is reachable, the pool says
// what fits. Joining those three is a fiddly little state machine -- a mesh
// uploaded now can evict one the walk listed a moment ago, a column arriving
// can displace another that still holds VBO memory -- and every one of those
// edges is a bug that shows up on a 240-line screen as one chunk wearing
// another's geometry. So it lives here, where a host test can drive it, and the
// platform layer is left with the part that genuinely needs a GPU: bind, set the
// matrix, draw.
//
// The device and the `--mesh` harness run the same code through this class,
// which is the only way the numbers in docs/3ds-performance.md keep meaning
// anything once the console is the thing being measured.
//
// What the caller still owns: the block data. This class never sees a
// ChunkColumn. It says which sections it wants meshed, nearest first, and the
// caller hands back vertices -- or says there were none.

#include "core/render/vbo_pool.hpp"
#include "core/render/visible_set.hpp"
#include "core/util/frustum.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::render {

struct ChunkRendererConfig {
    // Render distance in chunks, and the hard outer bound on everything: the
    // field is sized to it, the walk cannot step outside it, and a column
    // beyond it is refused rather than quietly wrapped onto a cell that
    // belongs to another column. The pool bounds memory, but a pool with no
    // radius limit still walks the field forever.
    int meshDistance = 8;

    VboPool::Budget budget;

    // Sections meshed per frame. Meshing costs 30.4 us per section on the dev
    // host, and the console is slower; letting a frame mesh everything the walk
    // asked for would stall it for tens of milliseconds the first time the
    // player turns round. Zero means no limit, which is what the offline
    // measurement harness wants -- it is pricing the pool's ceiling, and a
    // per-frame budget would hide it.
    int meshBudgetPerFrame = 4;

    // Geometric size classes; see docs/3ds-performance.md section 3.
    double sizeClassRatio = kSizeClassRatio;
};

class ChunkRenderer {
public:
    struct FrameStats {
        int sectionsVisited = 0;
        int rejectedByFrustum = 0;
        int drawn = 0;
        int queued = 0;      // reached, unmeshed, whether or not it fits the budget
        int uploaded = 0;    // meshes accepted this frame
        int empty = 0;       // sections meshed to nothing this frame
        int refused = 0;     // meshes the pool could not place
        usize drawnBytes = 0;
    };

    // The allocator must outlive the renderer.
    void reset(VboAllocator* allocator, const ChunkRendererConfig& config);
    void shutdown();

    const ChunkRendererConfig& config() const { return config_; }
    const SectionField& field() const { return field_; }
    const VboPool& pool() const { return pool_; }

    // Moving the centre does not unload anything by itself. Columns that fall
    // out of range keep their cells until something displaces them or the
    // caller drops them: the world layer knows which columns it still holds,
    // and this class does not.
    void setCentre(i32 chunkX, i32 chunkZ);

    bool inRange(i32 chunkX, i32 chunkZ) const { return field_.inRange(chunkX, chunkZ); }

    // Publishes a column's visibility masks. Re-publishing the same column
    // keeps its meshes -- that is what a neighbour arriving should do -- while
    // a different column landing on the same cell gives them back first.
    // False when the column is outside the render distance.
    bool publishColumn(i32 chunkX, i32 chunkZ, const mesh::SectionVisibility* perSection);

    // Hands back everything the column holds and forgets it.
    void dropColumn(i32 chunkX, i32 chunkZ);

    // **Is this exact column in the field right now?**
    //
    // Not the same question as "was it published": the field wraps modulo the
    // render distance, so publishing a column can displace one an entire
    // distance away without that column being told. A caller that remembers
    // having published something has to be able to check whether the answer
    // still holds.
    bool hasColumn(i32 chunkX, i32 chunkZ) const { return field_.isLoaded(chunkX, chunkZ); }

    // Puts a section back in the mesh queue, releasing whatever it held. This
    // is what a block edit, a lighting update or a neighbour arriving calls.
    void invalidateSection(i32 chunkX, int sectionY, i32 chunkZ);
    void invalidateColumn(i32 chunkX, i32 chunkZ);

    // Runs the walk and touches everything it is about to draw, which is what
    // protects the draw list from the uploads that follow it.
    void beginFrame(u32 frame, const Frustum& frustum, i32 cameraChunkX, int cameraSectionY,
                    i32 cameraChunkZ);

    // Nearest first, already cut to meshBudgetPerFrame. Valid until the next
    // beginFrame().
    const std::vector<VisibleSection>& meshQueue() const { return meshQueue_; }

    // Vertices for a section the queue asked for. False when the pool had no
    // room, which is not an error: the section stays unmeshed and comes back
    // round next frame.
    //
    // Safe to call for a section that is not in the queue -- a caller that
    // meshes on a worker thread will finish a section a frame or two after it
    // was asked for, by which time the walk may have moved on.
    // `ranges` says how the buffer divides into the three draw passes, in the
    // order MeshBuilder::copyTo wrote them.
    bool uploadSection(i32 chunkX, int sectionY, i32 chunkZ, const void* vertices,
                       const mesh::MeshRanges& ranges);

    // The section was meshed and came out empty. Recording that is what keeps
    // it out of the queue for good: 15 % of the sections worth meshing produce
    // nothing, and asking them again every frame is pure loss.
    void noteEmptySection(i32 chunkX, int sectionY, i32 chunkZ);

    // What to draw, in breadth-first order. Each entry carries the pool slot,
    // so the draw loop is a bind and a matrix per section.
    const std::vector<VisibleSection>& drawList() const { return visible_.draw; }

    const FrameStats& frameStats() const { return frameStats_; }

private:
    // Names a section by the cell it wraps onto rather than by its coordinates,
    // so it survives the centre moving between the upload and the eviction that
    // hands it back.
    u32 tokenFor(int cell, int sectionY) const
    {
        return u32(cell * SectionField::kSectionsY + sectionY);
    }

    // Everything the pool is holding for us, indexed by token. Both the column
    // and the slot are stored rather than looked up, because by the time a
    // block has to go back its column may be outside the render distance -- the
    // centre moves first and the displaced column is released second, so a
    // field lookup at that moment answers "not here" and the block would be
    // lost for the lifetime of the pool.
    struct Resident {
        i32 chunkX = 0;
        i32 chunkZ = 0;
        u16 slot = 0;
        bool live = false;
    };

    void drainEvictions();
    void releaseSection(int cell, int sectionY);
    void releaseCell(int cell);

    ChunkRendererConfig config_;
    SectionField field_;
    VboPool pool_;
    SizeClasses classes_;

    VisibleSet visible_;
    std::vector<VisibleSection> meshQueue_;
    std::vector<Resident> residents_;

    FrameStats frameStats_;
};

}  // namespace mc::render
