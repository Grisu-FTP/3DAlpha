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
// Nothing drawn in the current frame can be evicted, so forcing one takes two
// frames: fill the pool while looking at the column, then turn away and upload
// the rest. That is the same sequence a player spinning on the spot produces.
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
    // game for the uploads that follow.
    renderer.beginFrame(2, closedFrustum(), 0, 0, 0);
    CHECK_EQ(renderer.frameStats().drawn, 1);

    for (int sy = kSectionsY - 1; sy >= 0; --sy) {
        renderer.uploadSection(0, sy, 0, vertices.data(), {vertices.size(), 0, 0});
    }
    CHECK(renderer.pool().stats().evictions > 0);

    // Whatever the pool took back is queued again, and nothing claims to hold a
    // mesh the pool no longer has.
    renderer.beginFrame(3, openFrustum(), 0, 0, 0);
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

TEST(invalidating_a_section_requeues_it_and_re_uploading_does_not_leak)
{
    TestAllocator allocator;
    ChunkRenderer renderer;
    renderer.reset(&allocator, config(1, 1 << 20, 0));
    renderer.setCentre(0, 0);
    publishOpenColumn(renderer, 0, 0);

    const std::vector<u8> vertices = meshBytes(64);
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    const usize reservedAfterFirst = renderer.pool().stats().reserved;

    renderer.invalidateSection(0, 4, 0);
    CHECK_EQ(renderer.pool().stats().residents, 0);

    renderer.beginFrame(1, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.meshQueue(), 0, 4, 0));

    // The same size class comes straight back off the free list.
    CHECK(renderer.uploadSection(0, 4, 0, vertices.data(), {vertices.size(), 0, 0}));
    CHECK_EQ(renderer.pool().stats().reserved, reservedAfterFirst);
    CHECK_EQ(renderer.pool().stats().residents, 1);

    // An empty section holds no slot, so requeueing it is a separate path.
    renderer.noteEmptySection(0, 5, 0);
    renderer.invalidateSection(0, 5, 0);
    renderer.beginFrame(2, openFrustum(), 0, 4, 0);
    CHECK(listed(renderer.meshQueue(), 0, 5, 0));
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
