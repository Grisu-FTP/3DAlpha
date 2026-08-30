#include "framework.hpp"

#include "core/mesh/vertex.hpp"
#include "core/render/vbo_pool.hpp"

#include <cstring>
#include <map>
#include <vector>

using namespace mc;
using render::SizeClasses;
using render::VboAllocator;
using render::VboPool;
using render::VboTier;

namespace {

// Both tiers are malloc, which is the point: the pool's tier policy is budget
// arithmetic, so it can be exercised for real on a host that has no VRAM. The
// caps are separate from the pool's own budget so that a tier can be made to
// *refuse* an allocation the budget thought was fine -- what a console with
// fragmented VRAM does.
class TestAllocator : public VboAllocator {
public:
    usize cap[2] = {usize(-1), usize(-1)};
    usize live[2] = {0, 0};
    int allocations[2] = {0, 0};
    int releases[2] = {0, 0};

    void* allocate(usize bytes, VboTier tier) override
    {
        const int t = int(tier);
        if (live[t] + bytes > cap[t]) {
            return nullptr;
        }
        live[t] += bytes;
        ++allocations[t];
        return std::malloc(bytes);
    }

    void release(void* pointer, usize bytes, VboTier tier) override
    {
        const int t = int(tier);
        live[t] -= bytes;
        ++releases[t];
        std::free(pointer);
    }

    bool balanced() const
    {
        return live[0] == 0 && live[1] == 0;
    }

    int totalAllocations() const { return allocations[0] + allocations[1]; }
};

constexpr usize kLargestMesh =
    usize(mesh::kMaxQuadsPerSection) * 4 * sizeof(mesh::WorldVertex);

SizeClasses defaultClasses()
{
    SizeClasses classes;
    classes.build(render::kSmallestSizeClass, kLargestMesh, render::kSizeClassRatio);
    return classes;
}

// A recognisable payload, so a mesh that lands in the wrong block shows up as
// wrong bytes rather than as a plausible one.
std::vector<u8> payload(usize bytes, u8 seed)
{
    std::vector<u8> data(bytes);
    for (usize i = 0; i < bytes; ++i) {
        data[i] = u8(seed + i);
    }
    return data;
}

bool holds(const VboPool& pool, u16 slot, const std::vector<u8>& want)
{
    return pool.size(slot) == want.size()
           && std::memcmp(pool.data(slot), want.data(), want.size()) == 0;
}

}  // namespace

TEST(size_classes_grow_and_cover_the_largest_section_a_mesher_can_produce)
{
    const usize largest = kLargestMesh;
    const SizeClasses classes = defaultClasses();

    CHECK(classes.count() > 1);
    CHECK(classes.count() <= SizeClasses::kMaxClasses);
    for (int i = 1; i < classes.count(); ++i) {
        CHECK(classes.bytes(i) > classes.bytes(i - 1));
    }

    // The top class has to hold the checkerboard worst case, or a legal mesh
    // would have nowhere to go. It generally overshoots it, because the last
    // class is the first one at or past the bound rather than the bound
    // itself -- which is the safe direction.
    const usize top = classes.bytes(classes.count() - 1);
    CHECK(top >= largest);
    CHECK(classes.classFor(largest) >= 0);
    CHECK(classes.classFor(top) >= 0);
    CHECK(classes.classFor(top + 1) < 0);

    // Smallest class that fits, not nearest.
    CHECK_EQ(classes.classFor(1), 0);
    CHECK_EQ(classes.classFor(2048), 0);
    CHECK_EQ(classes.classFor(2049), 1);
}

TEST(the_shipped_ratio_covers_a_section_with_room_to_spare)
{
    // The table is fixed-size, so a ratio fine enough to run out of classes
    // before reaching the largest legal mesh would leave real geometry with
    // nowhere to go. That is a startup check, not a runtime one -- but only if
    // build() says so instead of quietly truncating.
    SizeClasses classes;
    CHECK(classes.build(render::kSmallestSizeClass, kLargestMesh,
                        render::kSizeClassRatio));
    CHECK(classes.capacity() >= kLargestMesh);

    // Headroom, so a future version with a taller world or a finer ratio does
    // not silently walk into the ceiling.
    CHECK(classes.count() < SizeClasses::kMaxClasses * 3 / 4);
}

TEST(a_ratio_too_fine_to_reach_the_top_reports_failure_rather_than_truncating)
{
    SizeClasses classes;
    CHECK(!classes.build(16, usize(1) << 40, 1.01));
    CHECK_EQ(classes.count(), SizeClasses::kMaxClasses);
    CHECK(classes.capacity() < (usize(1) << 40));
}

TEST(a_ratio_close_to_one_still_terminates)
{
    // The table is built from a float ratio, so a ratio that rounds back to the
    // same size would loop forever. It is forced to grow by a byte instead.
    SizeClasses classes;
    CHECK(classes.build(16, 64, 1.000001));
    CHECK(classes.count() > 0);
    CHECK(classes.count() <= SizeClasses::kMaxClasses);
    for (int i = 1; i < classes.count(); ++i) {
        CHECK(classes.bytes(i) > classes.bytes(i - 1));
    }
}

TEST(an_uploaded_mesh_comes_back_byte_for_byte)
{
    TestAllocator allocator;
    VboPool pool;
    pool.reset(&allocator, {64 * 1024, 64 * 1024}, defaultClasses());
    pool.beginFrame(1);

    const std::vector<u8> mesh = payload(3000, 7);
    const u16 slot = pool.upload(mesh.data(), 42, {mesh.size(), 0, 0});

    CHECK(slot != VboPool::kNoSlot);
    CHECK(holds(pool, slot, mesh));
    CHECK_EQ(pool.owner(slot), u32(42));
    CHECK_EQ(pool.stats().residents, 1);
    CHECK_EQ(pool.stats().resident, usize(3000));

    // Reserved is the size class, not the mesh: the difference is the price of
    // not fragmenting.
    CHECK(pool.stats().reserved >= 3000);
    CHECK_EQ(pool.wastedBytes(), pool.stats().reserved - usize(3000));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(a_released_block_is_handed_to_the_next_mesh_of_its_size)
{
    // The reason for size-class free lists at all: 30 Hz of mesh churn must not
    // reach the allocator, or it fragments both heaps.
    TestAllocator allocator;
    VboPool pool;
    const SizeClasses classes = defaultClasses();
    pool.reset(&allocator, {1024 * 1024, 1024 * 1024}, classes);
    pool.beginFrame(1);

    // Two sizes that share a class, taken from the table rather than guessed:
    // a mesh that happens to land on a class boundary is in the class below,
    // and picking the numbers by eye gets that wrong.
    const usize band = classes.bytes(6);
    const usize smaller = classes.bytes(5) + 1;
    CHECK_EQ(classes.classFor(smaller), classes.classFor(band));

    const std::vector<u8> first = payload(smaller, 1);
    const u16 slot = pool.upload(first.data(), 1, {first.size(), 0, 0});
    CHECK(slot != VboPool::kNoSlot);
    const int afterFirst = allocator.totalAllocations();

    pool.release(slot);
    CHECK_EQ(pool.stats().residents, 0);
    CHECK_EQ(pool.stats().freeBlocks, 1);

    // **Two frames on, not one.** The block was drawn in frame 1, so frame 1's
    // command list is still in flight for the whole of frame 2 and the pool
    // refuses to write over it (VboPool::kRetireFrames). This test used to
    // re-upload in the same frame it released, which is precisely the
    // write-after-read that stretched geometry on hardware.
    pool.beginFrame(3);

    // Same class, so it must reuse rather than allocate.
    const std::vector<u8> second = payload(band, 2);
    const u16 again = pool.upload(second.data(), 2, {second.size(), 0, 0});
    CHECK(again != VboPool::kNoSlot);
    CHECK_EQ(allocator.totalAllocations(), afterFirst);
    CHECK_EQ(pool.stats().reused, u32(1));
    CHECK(holds(pool, again, second));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(vram_fills_before_linear_and_the_nearest_meshes_arrive_first)
{
    // There is no distance sorting in the pool on purpose: uploads arrive
    // nearest-first from the visibility walk, so filling VRAM in arrival order
    // is what puts the closest geometry in the fast tier.
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 4096, 2.0);  // one class, so the arithmetic is plain
    pool.reset(&allocator, {4096 * 3, 4096 * 10}, classes);
    pool.beginFrame(1);

    const std::vector<u8> mesh = payload(4000, 3);
    std::vector<u16> slots;
    for (u32 i = 0; i < 6; ++i) {
        slots.push_back(pool.upload(mesh.data(), i, {mesh.size(), 0, 0}));
    }

    for (u16 slot : slots) {
        CHECK(slot != VboPool::kNoSlot);
    }
    for (int i = 0; i < 3; ++i) {
        CHECK(pool.tier(slots[usize(i)]) == VboTier::Vram);
    }
    for (int i = 3; i < 6; ++i) {
        CHECK(pool.tier(slots[usize(i)]) == VboTier::Linear);
    }
    CHECK_EQ(pool.stats().reservedByTier[int(VboTier::Vram)], usize(4096 * 3));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(a_tier_that_refuses_an_allocation_falls_through_instead_of_failing)
{
    // VRAM can be gone even when the budget says otherwise -- fragmentation, or
    // something else took it. A console in that state has to keep drawing.
    TestAllocator allocator;
    allocator.cap[int(VboTier::Vram)] = 0;

    VboPool pool;
    pool.reset(&allocator, {1024 * 1024, 1024 * 1024}, defaultClasses());
    pool.beginFrame(1);

    const std::vector<u8> mesh = payload(4000, 5);
    const u16 slot = pool.upload(mesh.data(), 1, {mesh.size(), 0, 0});

    CHECK(slot != VboPool::kNoSlot);
    CHECK(pool.tier(slot) == VboTier::Linear);
    CHECK(holds(pool, slot, mesh));
    CHECK_EQ(allocator.allocations[int(VboTier::Vram)], 0);

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(the_least_recently_drawn_section_is_the_one_evicted)
{
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 4096, 2.0);
    pool.reset(&allocator, {0, 4096 * 3}, classes);

    const std::vector<u8> mesh = payload(4000, 9);
    std::vector<u16> slots;

    pool.beginFrame(1);
    for (u32 i = 0; i < 3; ++i) {
        slots.push_back(pool.upload(mesh.data(), i, {mesh.size(), 0, 0}));
    }

    // Frames 2 and 3 draw owners 1 and 2 but not 0, so 0 is the coldest -- and
    // by frame 3 it is also two frames clear of the GPU, which is what the pool
    // requires before it will take a block back. See VboPool::kRetireFrames.
    pool.beginFrame(2);
    pool.touch(slots[1]);
    pool.touch(slots[2]);
    pool.beginFrame(3);
    pool.touch(slots[1]);
    pool.touch(slots[2]);

    const u16 fresh = pool.upload(mesh.data(), 99, {mesh.size(), 0, 0});
    CHECK(fresh != VboPool::kNoSlot);
    CHECK_EQ(pool.stats().evictions, u32(1));
    CHECK_EQ(pool.evicted().size(), usize(1));
    CHECK_EQ(pool.evicted()[0], u32(0));
    pool.clearEvicted();
    CHECK_EQ(pool.evicted().size(), usize(0));

    // The block was reused as it stood: same class, so nothing reached the
    // allocator a fourth time.
    CHECK_EQ(allocator.totalAllocations(), 3);
    CHECK_EQ(pool.stats().residents, 3);

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(nothing_drawn_this_frame_is_ever_evicted_to_make_room)
{
    // The invariant that keeps geometry from flickering: a mesh built at the
    // end of a frame must not take memory from something that frame is
    // drawing. The pool refuses the upload instead, and the caller retries.
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 4096, 2.0);
    pool.reset(&allocator, {0, 4096 * 2}, classes);

    const std::vector<u8> mesh = payload(4000, 11);
    pool.beginFrame(1);
    const u16 a = pool.upload(mesh.data(), 1, {mesh.size(), 0, 0});
    const u16 b = pool.upload(mesh.data(), 2, {mesh.size(), 0, 0});
    CHECK(a != VboPool::kNoSlot);
    CHECK(b != VboPool::kNoSlot);

    // Both were uploaded this frame, so both count as drawn this frame.
    const u16 third = pool.upload(mesh.data(), 3, {mesh.size(), 0, 0});
    CHECK_EQ(third, VboPool::kNoSlot);
    CHECK_EQ(pool.stats().failures, u32(1));
    CHECK_EQ(pool.stats().evictions, u32(0));
    CHECK_EQ(pool.stats().residents, 2);

    // Still refused a frame later: frame 1's list is in flight for the whole of
    // frame 2, so its blocks are untouchable then too.
    pool.beginFrame(2);
    CHECK_EQ(pool.upload(mesh.data(), 3, {mesh.size(), 0, 0}), VboPool::kNoSlot);
    CHECK_EQ(pool.stats().evictions, u32(0));

    // Two frames on, the coldest gives way.
    pool.beginFrame(3);
    const u16 later = pool.upload(mesh.data(), 3, {mesh.size(), 0, 0});
    CHECK(later != VboPool::kNoSlot);
    CHECK_EQ(pool.stats().evictions, u32(1));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(evictions_survive_a_frame_boundary_and_a_refused_upload)
{
    // The pool's sharpest edge, and one a scripted test would not have found:
    // an upload that gives up geometry and *still* fails to place the mesh. The
    // caller must hear about those evictions, or it draws from a slot that now
    // belongs to someone else. So the list accumulates until read rather than
    // resetting each frame.
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 16384, 2.0);   // 4k, 8k, 16k
    pool.reset(&allocator, {0, 4096 * 2}, classes);

    pool.beginFrame(1);
    const std::vector<u8> small = payload(4000, 1);
    CHECK(pool.upload(small.data(), 1, {small.size(), 0, 0}) != VboPool::kNoSlot);
    CHECK(pool.upload(small.data(), 2, {small.size(), 0, 0}) != VboPool::kNoSlot);

    // Frame 3: both are two frames clear of the GPU, so both are evictable --
    // but a 16k mesh cannot be placed even after giving both of them up.
    pool.beginFrame(2);
    pool.beginFrame(3);
    const std::vector<u8> big = payload(16000, 2);
    CHECK_EQ(pool.upload(big.data(), 3, {big.size(), 0, 0}), VboPool::kNoSlot);
    CHECK(pool.stats().evictions > 0);
    CHECK(pool.evicted().size() > 0);

    // The notification must still be there a frame later, unread.
    pool.beginFrame(4);
    CHECK(pool.evicted().size() > 0);

    pool.clearEvicted();
    CHECK_EQ(pool.evicted().size(), usize(0));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(a_mesh_of_a_different_size_reclaims_free_blocks_before_evicting)
{
    // Blocks parked on the wrong free list are memory nobody is drawing from.
    // Giving those back costs an allocator call; evicting costs geometry. The
    // cheaper one has to come first.
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 65536, 2.0);   // 4k, 8k, 16k, 32k, 64k
    pool.reset(&allocator, {0, 4096 + 8192}, classes);

    pool.beginFrame(1);
    const std::vector<u8> small = payload(4000, 1);
    const u16 slot = pool.upload(small.data(), 1, {small.size(), 0, 0});
    CHECK(slot != VboPool::kNoSlot);

    const std::vector<u8> medium = payload(8000, 2);
    const u16 other = pool.upload(medium.data(), 2, {medium.size(), 0, 0});
    CHECK(other != VboPool::kNoSlot);

    // Give the 8k block up voluntarily; it stays in the pool, parked.
    pool.release(other);
    CHECK_EQ(pool.stats().freeBlocks, 1);

    // Two frames on, so the parked block is clear of the GPU and may be handed
    // back to the allocator. Returning it while frame 1 was still fetching from
    // it would free an address the GPU is reading.
    pool.beginFrame(3);

    // A 4k mesh cannot use the parked 8k block, and the budget is full. The
    // pool must hand the 8k back and allocate a 4k, not evict the live mesh.
    const std::vector<u8> another = payload(4000, 3);
    const u16 fresh = pool.upload(another.data(), 3, {another.size(), 0, 0});
    CHECK(fresh != VboPool::kNoSlot);
    CHECK_EQ(pool.stats().evictions, u32(0));
    CHECK(holds(pool, slot, small));
    CHECK(holds(pool, fresh, another));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(a_full_pool_settles_instead_of_thrashing)
{
    // The steady state the design is for: a working set that fits, moving one
    // section at a time. Once it has settled, every upload must be served from
    // a free block, with no allocator traffic at all -- that is the difference
    // between size-class free lists and craftus's sorted vector.
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 4096, 2.0);

    constexpr int kCapacity = 16;

    // **One block of headroom, and it is not slack -- it is the pipeline.** A
    // block the departing section gave up cannot be written again until the
    // frame that drew it has left the GPU (VboPool::kRetireFrames), so at any
    // moment one block is in that limbo. A pool sized exactly to the drawn set
    // would refuse an upload every frame and never settle.
    pool.reset(&allocator, {0, 4096 * (kCapacity + 1)}, classes);

    const std::vector<u8> mesh = payload(4000, 13);
    std::map<u32, u16> resident;   // owner -> slot, as the field would hold it

    // Fill it.
    pool.beginFrame(1);
    for (u32 owner = 0; owner < kCapacity; ++owner) {
        resident[owner] = pool.upload(mesh.data(), owner, {mesh.size(), 0, 0});
    }
    const int afterFill = allocator.totalAllocations();
    CHECK_EQ(afterFill, kCapacity);

    // Then walk: each frame one more owner stops being drawn and a new one
    // arrives. Owners are dropped from the draw list in order, so the coldest
    // is always the one that left earliest -- which is what the LRU should pick.
    for (u32 step = 0; step < 200; ++step) {
        const u32 frame = 2 + step;
        pool.beginFrame(frame);

        const u32 arriving = u32(kCapacity) + step;
        for (const auto& entry : resident) {
            if (entry.first > step) {
                pool.touch(entry.second);
            }
        }

        const u16 slot = pool.upload(mesh.data(), arriving, {mesh.size(), 0, 0});
        CHECK(slot != VboPool::kNoSlot);
        for (u32 owner : pool.evicted()) {
            resident.erase(owner);
        }
        pool.clearEvicted();
        resident[arriving] = slot;
    }

    CHECK_EQ(pool.stats().failures, u32(0));
    CHECK_EQ(pool.stats().residents, kCapacity + 1);
    // One allocator call past the initial fill -- the headroom block, taken on
    // the first step -- and not one after that: every later step recycled the
    // block the departing section gave up.
    CHECK_EQ(allocator.totalAllocations(), afterFill + 1);
    CHECK_EQ(pool.stats().evictions, u32(199));
    CHECK_EQ(pool.stats().reused, u32(199));

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(shutting_down_hands_every_block_back)
{
    // A pool that leaks on teardown leaks the linear heap, which is the one
    // pool that cannot be spared. Checked by balance, not by hope.
    TestAllocator allocator;
    VboPool pool;
    pool.reset(&allocator, {32 * 1024, 32 * 1024}, defaultClasses());

    pool.beginFrame(1);
    std::vector<u16> slots;
    for (u32 i = 0; i < 8; ++i) {
        const std::vector<u8> mesh = payload(1000 + i * 700, u8(i));
        slots.push_back(pool.upload(mesh.data(), i, {mesh.size(), 0, 0}));
    }
    pool.release(slots[2]);
    pool.release(slots[5]);

    pool.shutdown();
    CHECK(allocator.balanced());
    CHECK_EQ(allocator.allocations[0] + allocator.allocations[1],
             allocator.releases[0] + allocator.releases[1]);
    CHECK_EQ(pool.stats().residents, 0);
}

TEST(re_budgeting_starts_from_a_clean_pool)
{
    // `reset` is how the render-distance option takes effect. It has to return
    // everything first, or changing the option leaks the old pool.
    TestAllocator allocator;
    VboPool pool;
    pool.reset(&allocator, {16 * 1024, 16 * 1024}, defaultClasses());
    pool.beginFrame(1);

    const std::vector<u8> mesh = payload(3000, 4);
    CHECK(pool.upload(mesh.data(), 1, {mesh.size(), 0, 0}) != VboPool::kNoSlot);

    pool.reset(&allocator, {64 * 1024, 64 * 1024}, defaultClasses());
    CHECK(allocator.balanced());
    CHECK_EQ(pool.stats().residents, 0);
    CHECK_EQ(pool.stats().reserved, usize(0));
    CHECK_EQ(pool.stats().uploads, u32(0));

    pool.beginFrame(1);
    CHECK(pool.upload(mesh.data(), 1, {mesh.size(), 0, 0}) != VboPool::kNoSlot);
    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(the_pool_holds_its_invariants_under_random_traffic)
{
    // The rungs below "a free block of the right class" -- reclaiming from the
    // wrong class, evicting something of another size -- only run when the pool
    // is squeezed in a way one scripted sequence does not reproduce. Random
    // traffic against a tight budget reaches them, and the invariants are
    // checked every step rather than at the end, so a failure names the step
    // that broke it.
    //
    // Deterministic on purpose: a seeded LCG rather than <random>, so a failure
    // here is a failure everybody can reproduce.
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(1024, 64 * 1024, 1.5);
    pool.reset(&allocator, {24 * 1024, 40 * 1024}, classes);

    std::map<u32, u16> live;              // owner -> slot
    std::map<u32, std::vector<u8>> want;  // owner -> what was uploaded
    u32 seed = 12345;
    auto next = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) & 0x7FFF;
    };

    for (u32 frame = 1; frame <= 400; ++frame) {
        pool.beginFrame(frame);

        // Draw a random half of what is resident.
        for (const auto& entry : live) {
            if (next() % 2 == 0) {
                pool.touch(entry.second);
            }
        }

        // Hand a few back voluntarily, the way an unloading column does.
        if (!live.empty() && next() % 4 == 0) {
            auto it = live.begin();
            std::advance(it, next() % live.size());
            pool.release(it->second);
            want.erase(it->first);
            live.erase(it);
        }

        for (int attempt = 0; attempt < 3; ++attempt) {
            const usize bytes = 700 + next() % 60000;
            const u32 owner = 1000 + next();
            if (live.count(owner) != 0) {
                continue;
            }
            const std::vector<u8> mesh = payload(bytes, u8(owner));
            const u16 slot = pool.upload(mesh.data(), owner, {mesh.size(), 0, 0});

            // Unconditionally, because a refused upload can evict too.
            for (u32 gone : pool.evicted()) {
                live.erase(gone);
                want.erase(gone);
            }
            pool.clearEvicted();

            if (slot == VboPool::kNoSlot) {
                continue;
            }
            live[owner] = slot;
            want[owner] = mesh;
        }

        // Every mesh the pool still claims to hold is byte-for-byte what was
        // put in it. This is what catches a slot handed out twice, which is the
        // failure mode a free list gets wrong and which would otherwise show up
        // as one section wearing another's geometry.
        for (const auto& entry : live) {
            CHECK(holds(pool, entry.second, want[entry.first]));
            CHECK_EQ(pool.owner(entry.second), entry.first);
        }

        // No two live owners share a slot.
        std::map<u16, u32> bySlot;
        for (const auto& entry : live) {
            CHECK(bySlot.count(entry.second) == 0);
            bySlot[entry.second] = entry.first;
        }

        // The pool's own accounting agrees with the caller's.
        CHECK_EQ(pool.stats().residents, int(live.size()));
        CHECK(pool.stats().resident <= pool.stats().reserved);
        CHECK(pool.stats().reservedByTier[int(VboTier::Vram)] <= usize(24 * 1024));
        CHECK(pool.stats().reservedByTier[int(VboTier::Linear)] <= usize(40 * 1024));
        CHECK_EQ(allocator.live[int(VboTier::Vram)],
                 pool.stats().reservedByTier[int(VboTier::Vram)]);
        CHECK_EQ(allocator.live[int(VboTier::Linear)],
                 pool.stats().reservedByTier[int(VboTier::Linear)]);
    }

    // The squeeze was real: this only proves anything if the hard paths ran.
    CHECK(pool.stats().evictions > 0);
    CHECK(pool.stats().reused > 0);

    pool.shutdown();
    CHECK(allocator.balanced());
}

TEST(a_mesh_larger_than_the_top_size_class_is_refused_not_truncated)
{
    TestAllocator allocator;
    VboPool pool;
    SizeClasses classes;
    classes.build(4096, 8192, 2.0);
    pool.reset(&allocator, {0, 1024 * 1024}, classes);
    pool.beginFrame(1);

    const std::vector<u8> huge = payload(9000, 1);
    CHECK_EQ(pool.upload(huge.data(), 1, {huge.size(), 0, 0}), VboPool::kNoSlot);
    CHECK_EQ(pool.stats().failures, u32(1));
    CHECK_EQ(pool.stats().residents, 0);

    pool.shutdown();
    CHECK(allocator.balanced());
}
