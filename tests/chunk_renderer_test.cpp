#include "framework.hpp"

#include "core/mesh/vertex.hpp"
#include "core/render/chunk_renderer.hpp"

#include <cstdlib>
#include <vector>

using namespace mc;
using mesh::SectionVisibility;
using render::ChunkRenderer;
using render::ChunkRendererConfig;
using render::SectionField;
using render::VboAllocator;
using render::VboTier;
using render::VisibleSection;

namespace {

constexpr int kSectionsY = SectionField::kSectionsY;

// Both tiers are malloc with a cap, so a budget can be made to run out for real
// on a host with no VRAM.
class TestAllocator : public VboAllocator {
public:
    usize live[2] = {0, 0};
    int outstanding = 0;

    void* allocate(usize bytes, VboTier tier) override
    {
        live[int(tier)] += bytes;
        ++outstanding;
        return std::malloc(bytes);
    }

    void release(void* pointer, usize bytes, VboTier tier) override
    {
        live[int(tier)] -= bytes;
        --outstanding;
        std::free(pointer);
    }
};

Frustum openFrustum()
{
    Frustum f;
    f.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);
    return f;
}

// Rejects every box there is. The walk keeps the camera's own section anyway --
// you are standing in it -- so this is "looking at nothing", which is what a
// player turning their back on the geometry they were just drawing does.
Frustum closedFrustum()
{
    Frustum f;
    for (int i = 0; i < Frustum::kPlaneCount; ++i) {
        f.setPlane(i, Plane{0.0f, 0.0f, 0.0f, -1.0f});
    }
    return f;
}

ChunkRendererConfig config(int distance, usize linearBytes, int meshBudget)
{
    ChunkRendererConfig c;
    c.meshDistance = distance;
    c.budget = {0, linearBytes};  // linear only: VRAM is not what is under test
    c.meshBudgetPerFrame = meshBudget;
    return c;
}

void publishOpenColumn(ChunkRenderer& renderer, i32 cx, i32 cz)
{
    SectionVisibility masks[kSectionsY];
    for (int sy = 0; sy < kSectionsY; ++sy) {
        masks[sy] = SectionVisibility(mesh::kVisibilityAll);
    }
    renderer.publishColumn(cx, cz, masks);
}

// Vertex bytes to hand the pool. The contents never matter -- nothing reads
// them back -- but the size does, because it picks the class.
std::vector<u8> meshBytes(usize quads)
{
    return std::vector<u8>(quads * 4 * sizeof(mesh::WorldVertex), 0);
}

bool listed(const std::vector<VisibleSection>& list, i32 cx, int sy, i32 cz)
{
    for (const VisibleSection& s : list) {
        if (s.chunkX == cx && s.sectionY == sy && s.chunkZ == cz) {
            return true;
        }
    }
    return false;
}

// The entry itself, so a test can check what the draw loop would read from it.
const VisibleSection* find(const std::vector<VisibleSection>& list, i32 cx, int sy, i32 cz)
{
    for (const VisibleSection& s : list) {
        if (s.chunkX == cx && s.sectionY == sy && s.chunkZ == cz) {
            return &s;
        }
    }
    return nullptr;
}

}  // namespace

TEST(a_reached_section_is_queued_then_drawn_once_it_has_a_mesh)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.meshQueue(), 0, 4, 0));
    CHECK(!listed(renderer.drawList(), 0, 4, 0));

    const std::vector<u8> vertices = meshBytes(64);
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));

    renderer.beginFrame(2, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));
    CHECK(!listed(renderer.meshQueue(), 0, 4, 0));
    CHECK_EQ(renderer.frameStats().drawnBytes >= vertices.size(), true);
}

// The state that keeps 15 % of the sections worth meshing from coming back
// round every single frame.
TEST(a_section_meshed_to_nothing_never_returns_to_the_queue)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    renderer.noteEmptySection(0, 4, 0);

    renderer.beginFrame(2, openFrustum(), 0, 4, 0);
    CHECK(!listed(renderer.meshQueue(), 0, 4, 0));
    CHECK(!listed(renderer.drawList(), 0, 4, 0));
    CHECK_EQ(allocator.outstanding, 0);
}

TEST(the_mesh_budget_caps_the_queue_without_losing_the_rest)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(2, 1 << 20, 3));
    renderer.setCentre(0, 0);
    for (i32 dz = -2; dz <= 2; ++dz) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            publishOpenColumn(renderer, dx, dz);
        }
    }

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK_EQ(renderer.meshQueue().size(), usize(3));
    // Everything reachable is still counted, so the overlay can show a backlog
    // rather than pretending the world is fully meshed.
    CHECK(renderer.frameStats().queued > 3);

    // The queue is nearest first, so the camera's own section leads it.
    CHECK_EQ(renderer.meshQueue()[0].chunkX, 0);
    CHECK_EQ(renderer.meshQueue()[0].chunkZ, 0);
    CHECK_EQ(int(renderer.meshQueue()[0].sectionY), 4);
}

// A pool small enough to force eviction has to leave the field agreeing with
// it, or the draw loop reads a slot that now belongs to another section -- one
// chunk wearing another's geometry.
//
// Nothing the GPU may still be reading can be evicted, so forcing an eviction
// takes three frames rather than two: fill the pool while looking at the
// column, then turn away and let the frame that drew it leave the pipeline
// before uploading the rest. That is the same sequence a player spinning on the
// spot produces, one frame later. See VboPool::kRetireFrames.
TEST(an_evicted_section_goes_back_into_the_queue)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 40 * 1024, 0));  // room for three
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(256);  // 12 KB each

    renderer.beginFrame(1, openFrustum(), 0, 0, 0);
    int uploaded = 0;
    for (int sy = 0; sy < kSectionsY; ++sy) {
        uploaded += renderer.uploadSection(0, sy, 0, vertices.data(), {vertices.size(), 0, 0});
    }
    CHECK(uploaded > 0);
    CHECK(uploaded < kSectionsY);              // the pool did fill up
    CHECK_EQ(renderer.pool().stats().evictions, 0u);  // within a frame it refuses
    CHECK_EQ(renderer.frameStats().uploaded, uploaded);
    CHECK_EQ(renderer.frameStats().refused, kSectionsY - uploaded);

    // Turn away: only the camera's own section is drawn, so the rest are fair
    // game for the uploads that follow -- but not until frame 1's command list
    // has left the GPU, which is one more frame.
    renderer.beginFrame(2, closedFrustum(), 0, 0, 0);
    CHECK_EQ(renderer.frameStats().drawn, 1);
    renderer.beginFrame(3, closedFrustum(), 0, 0, 0);

    for (int sy = kSectionsY - 1; sy >= 0; --sy) {
        renderer.uploadSection(0, sy, 0, vertices.data(), {vertices.size(), 0, 0});
    }
    CHECK(renderer.pool().stats().evictions > 0);

    // Whatever the pool took back is queued again, and nothing claims to hold a
    // mesh the pool no longer has.
    renderer.beginFrame(4, openFrustum(), 0, 0, 0);
    CHECK_EQ(renderer.frameStats().drawn + renderer.frameStats().queued, kSectionsY);
    CHECK_EQ(renderer.frameStats().drawn, renderer.pool().stats().residents);

    // Every slot the draw list names still holds the section that claims it.
    for (const VisibleSection& s : renderer.drawList()) {
        CHECK_EQ(renderer.pool().size(s.slot), vertices.size());
    }
}

// The grid wraps, so a column arriving can displace one a whole render distance
// away. Its VBO memory is only reachable through its own cell, so if it is not
// handed back here it is lost for the lifetime of the pool.
TEST(a_column_displaced_from_its_cell_gives_its_memory_back)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 1, 0);

    const std::vector<u8> vertices = meshBytes(64);
    for (int sy = 0; sy < kSectionsY; ++sy) {
        CHECK(renderer.uploadSection(1, sy, 0, vertices.data(), {vertices.size(), 0, 0}));
    }
    CHECK_EQ(renderer.pool().stats().residents, kSectionsY);

    // Walk east until column 4 is in range. Edge is 3, so 4 lands on the same
    // cell as 1.
    renderer.setCentre(3, 0);
    CHECK(!renderer.inRange(1, 0));
    publishOpenColumn(renderer, 4, 0);

    CHECK_EQ(renderer.pool().stats().residents, 0);
    CHECK_EQ(renderer.pool().stats().reserved, renderer.pool().stats().reserved);
    CHECK_EQ(renderer.field().meshSlot(4, 4, 0), SectionField::kNoMesh);

    // The blocks stay in the pool on their free lists rather than going back to
    // the allocator -- that is what the free lists are for.
    CHECK(allocator.outstanding > 0);
    renderer.shutdown();
    CHECK_EQ(allocator.outstanding, 0);
}

TEST(re_publishing_the_same_column_keeps_its_meshes)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(64);
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));

    // What a neighbour arriving does: the masks are recomputed and republished,
    // and the geometry is still good.
    publishOpenColumn(renderer, 0, 0);
    CHECK_EQ(renderer.pool().stats().residents, 1);

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));
}

// **An invalidated section keeps drawing its old mesh until the new one lands.**
// This is the fix for chunks flashing transparent when a block changed. The
// draw list is built at the top of the frame and the remesh runs after it, so a
// section that gave its block up on invalidation could not be drawn again until
// the frame *after* the remesh -- a hole, and a hole that stays for as long as
// the mesh budget is behind, which is the whole time a fluid is flowing.
TEST(invalidating_a_section_keeps_it_drawn_and_queues_it_for_remesh)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(64);
    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    const usize reservedAfterFirst = renderer.pool().stats().reserved;

    renderer.beginFrame(2, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));

    // The block change. The mesh it is holding is now a tick out of date, and
    // that is the thing to keep drawing.
    renderer.invalidateSection(0, 4, 0);
    CHECK_EQ(renderer.pool().stats().residents, 1);

    // Both lists: drawn from the stale mesh, and queued to be replaced.
    renderer.beginFrame(3, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));
    CHECK(listed(renderer.meshQueue(), 0, 4, 0));

    // The replacement goes into a second block, because the one being drawn is
    // still being drawn -- upload first, release second. So the pool is
    // momentarily holding two, and exactly one is resident afterwards.
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    CHECK_EQ(renderer.pool().stats().residents, 1);
    CHECK(renderer.pool().stats().reserved > reservedAfterFirst);

    // Remeshed, so out of the queue and still drawn.
    renderer.beginFrame(4, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));
    CHECK(!listed(renderer.meshQueue(), 0, 4, 0));

    // Once the first block has left the GPU it is recycled rather than added
    // to, so the pool stops growing.
    renderer.invalidateSection(0, 4, 0);
    renderer.beginFrame(5, openFrustum(), 0, 4, 0);
    const usize reservedBefore = renderer.pool().stats().reserved;
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    CHECK_EQ(renderer.pool().stats().reserved, reservedBefore);
    CHECK_EQ(renderer.pool().stats().residents, 1);

    // An empty section holds no slot, so requeueing it is a separate path.
    renderer.noteEmptySection(0, 5, 0);
    renderer.invalidateSection(0, 5, 0);
    renderer.beginFrame(6, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.meshQueue(), 0, 5, 0));
}

// **The section must be drawable on every single frame, including the one its
// remesh lands on.** This is the regression that put the flash back after the
// first fix: the draw list is built before the frame's meshing, so it names the
// block the section held *then*; uploadSection gives that block back once the
// replacement is in hand, which bumps its generation; and the staleness guard in
// the draw loop -- added to stop one chunk being drawn with another's model
// matrix -- then correctly refused to draw the entry it no longer recognised.
// Correct, and a hole, on every remesh.
TEST(a_remesh_leaves_the_section_drawable_in_the_same_frame)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(64);
    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));

    renderer.beginFrame(2, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));

    renderer.invalidateSection(0, 4, 0);
    renderer.beginFrame(3, openFrustum(), 0, 4, 0);

    // The entry the draw loop will read, before the remesh.
    const VisibleSection* before = find(renderer.drawList(), 0, 4, 0);
    CHECK(before != nullptr);
    CHECK(renderer.pool().generation(before->slot) == before->slotGeneration);

    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));

    // **After the remesh, that same entry must still be drawable** -- the slot
    // it names resident, and its generation still matching, which together are
    // exactly what drawPass tests before it will draw a section.
    const VisibleSection* after = find(renderer.drawList(), 0, 4, 0);
    CHECK(after != nullptr);
    CHECK(renderer.pool().resident(after->slot));
    CHECK_EQ(renderer.pool().generation(after->slot), after->slotGeneration);

    // And it names the new mesh, not the block that was handed back.
    CHECK_EQ(renderer.pool().size(after->slot), vertices.size());
}

// A pool too full to place the replacement must leave the old mesh on screen
// rather than take it away and fail -- the reason uploadSection uploads before
// it releases.
TEST(a_refused_remesh_keeps_the_old_mesh_drawn)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    // Room for exactly one mesh, so the replacement cannot be placed beside the
    // original and the original cannot be evicted to make room for it.
    renderer.reset(&allocator, config(1, 14 * 1024, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(256);   // 12 KB
    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));

    renderer.invalidateSection(0, 4, 0);
    renderer.beginFrame(2, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));

    CHECK(!renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    CHECK_EQ(renderer.frameStats().refused, 1);

    // Still resident, still drawn, still queued to try again.
    CHECK_EQ(renderer.pool().stats().residents, 1);
    renderer.beginFrame(3, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.drawList(), 0, 4, 0));
    CHECK(listed(renderer.meshQueue(), 0, 4, 0));
}

TEST(dropping_a_column_hands_everything_back)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(64);
    for (int sy = 0; sy < kSectionsY; ++sy) {
        CHECK(renderer.uploadSection(0, sy, 0, vertices.data(), {vertices.size(), 0, 0}));
    }

    renderer.dropColumn(0, 0);
    CHECK_EQ(renderer.pool().stats().residents, 0);
    CHECK(!renderer.field().isLoaded(0, 0));

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK_EQ(renderer.frameStats().drawn, 0);
}

// mesh_distance is the hard outer bound: the pool bounds memory, but a renderer
// that accepted columns beyond the radius would wrap them onto cells belonging
// to columns that are actually in view.
TEST(a_column_beyond_the_mesh_distance_is_refused)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(2, 1 << 20, 0));
    renderer.setCentre(0, 0);

    SectionVisibility masks[kSectionsY];
    for (int sy = 0; sy < kSectionsY; ++sy) {
        masks[sy] = SectionVisibility(mesh::kVisibilityAll);
    }
    CHECK(renderer.publishColumn(2, 0, masks));
    CHECK(!renderer.publishColumn(3, 0, masks));
    CHECK(!renderer.publishColumn(0, -3, masks));

    const std::vector<u8> vertices = meshBytes(8);
    CHECK(!renderer.uploadSection(3, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    CHECK_EQ(renderer.pool().stats().residents, 0);
}

// A worker thread finishes a mesh a frame or two after it was asked for, by
// which time the column may be gone. Dropping the result is right; writing it
// into a cell that now belongs to someone else is not.
TEST(a_mesh_for_a_column_that_has_gone_is_dropped)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    renderer.dropColumn(0, 0);

    const std::vector<u8> vertices = meshBytes(64);
    CHECK(!renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    CHECK_EQ(renderer.pool().stats().residents, 0);
    CHECK_EQ(allocator.outstanding, 0);
}
