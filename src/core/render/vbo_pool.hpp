#pragma once

// The bounded pool of vertex memory that section meshes live in.
//
// This is the structure the M2 measurement argued for. Meshing everything
// loaded costs 25.7 MB at render distance 8 against a ~12 MB budget, and the
// visibility walk only trims that to 17.7 MB -- standing under open sky, nearly
// everything is reachable. What actually fits is the *drawn* set: 2.20-7.72 MB
// at distance 8 over a full turn on the spot. So the bound is a pool with
// eviction, and the walk's job is to supply priority order rather than to
// decide what gets meshed. See docs/3ds-performance.md section 4.
//
// Three properties this has to have, all of them from measurement:
//
//   * **Size-class free lists**, not a sorted vector. craftus's VBOCache has
//     the right idea and scans O(n) under a lock; at 30 Hz mesh churn that is
//     both slow and a fragmentation source. A freed block here goes back to its
//     class's list and is handed straight to the next mesh of that size, with
//     no allocator traffic at all.
//   * **VRAM first, linear fallback.** VRAM is 6 MB with materially higher
//     bandwidth, and ~4.5 MB of it is free once the render targets and the
//     atlas are placed. Nothing extra is needed to give it to the nearest
//     sections: `VisibleSet::toMesh` already comes out nearest first, so
//     filling VRAM in upload order fills it with the closest geometry.
//   * **Eviction by least recently drawn.** A section behind you stops being
//     touched and ages out on its own, which is what keeps the pool tracking
//     the view rather than the load radius. `mesh_distance` stays the hard
//     outer bound, because a pool with no ceiling still fragments.
//
// Platform-independent on purpose, like the rest of core: the allocator is an
// interface so the host tests can cap either tier and watch the fallback and
// the eviction path actually run. On the 3DS the two tiers are `vramAlloc` and
// `linearAlloc`; on the host they are both malloc.

#include "core/mesh/vertex.hpp"
#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::render {

// Which memory the GPU will fetch a mesh from. Both are GPU-visible; the
// difference is bandwidth and how much there is.
enum class VboTier : u8 {
    Vram = 0,
    Linear = 1,
    kCount = 2,
};

// The platform's vertex-memory allocator.
//
// One of the few vtables in the project, and it earns its place the same way
// `io::FileSystem` does: it is called a handful of times per frame at most --
// only when a mesh needs a size class the pool has no free block for -- never
// per section and never per vertex. In exchange the host tests can run the
// whole pool with either tier set to zero.
class VboAllocator {
public:
    virtual ~VboAllocator() = default;

    // Null when the tier cannot satisfy the request. The pool treats that as
    // "this tier is full" and moves on rather than failing, because a console
    // that has lost VRAM to fragmentation should still draw.
    virtual void* allocate(usize bytes, VboTier tier) = 0;
    virtual void release(void* pointer, usize bytes, VboTier tier) = 0;

    // Called after the pool has written vertex data, before anything draws it.
    // Nothing to do on a host; on the console the CPU's data cache and the GPU
    // do not see the same memory, and a mesh that is never flushed draws as
    // whatever was in that block before.
    //
    // The allocator is the right place for it because it is the only thing that
    // knows what kind of memory it handed out -- VRAM needs no flush, and
    // citro3d's own C3D_TexFlush skips it there for the same reason.
    virtual void flush(void* pointer, usize bytes, VboTier tier)
    {
        (void)pointer;
        (void)bytes;
        (void)tier;
    }
};

// Geometric size classes.
//
// Rounding every mesh up to a power of two would waste about a third of the
// budget, which at 12 MB is 4 MB of geometry thrown away. A finer ratio costs
// only a longer table -- the classes are a handful of words, and choosing one
// is a lookup. The ratio in use is a measured choice; see
// docs/3ds-performance.md.
class SizeClasses {
public:
    static constexpr int kMaxClasses = 96;

    // Classes run from `smallest` up to at least `largest`, each `ratio` times
    // the one below it. Both bounds are in bytes.
    //
    // False when the table ran out of room before reaching `largest`, which
    // would leave legal meshes with no class to go in. Returned rather than
    // asserted because sweeping ratios to *find* that edge is exactly what the
    // host harness does; the game checks it at startup.
    bool build(usize smallest, usize largest, double ratio);

    int count() const { return count_; }
    usize bytes(int index) const { return bytes_[index]; }

    // The largest mesh this table can hold.
    usize capacity() const { return count_ > 0 ? bytes_[count_ - 1] : 0; }

    // The smallest class that holds `bytes`, or -1 if it is off the top of the
    // table. Linear from the bottom: the table is short, most meshes are small,
    // and this runs once per upload rather than once per section per frame.
    int classFor(usize bytes) const;

private:
    usize bytes_[kMaxClasses] = {};
    int count_ = 0;
};

// The table the game builds. Measured, not chosen: `--mesh` sweeps candidate
// ratios over every section mesh in a real world and reports how much real
// geometry each one fits in a fixed budget.
//
// Powers of two get 8.27 MB of geometry into a 12 MB pool. This ratio gets
// 11.14 MB into the same 12 MB -- 35 % more world on screen -- and costs two
// points of block-recycling rate and under one extra allocator call per frame.
// Going finer still keeps paying, but only 0.26 MB more for half again as many
// classes, so this is where it stops. See docs/3ds-performance.md section 3.
inline constexpr double kSizeClassRatio = 1.15;
inline constexpr usize kSmallestSizeClass = 2048;

class VboPool {
public:
    // Slot indices are u16 because the field stores one per section and there
    // are only ever a few thousand.
    static constexpr u16 kNoSlot = 0xFFFF;

    // **How many frames a block must sit idle before it may be written or
    // handed back.** This is the one rule that keeps the pool from corrupting
    // geometry the GPU is still fetching, and it is not obvious, so:
    //
    // `C3D_FrameEnd` only *enqueues* the command list -- the wait for the GPU
    // queue to drain lives inside the next `C3D_FrameBegin`. So the CPU leaves
    // drawFrame with the frame it just recorded still executing, and runs the
    // whole of the next frame's streaming and meshing -- `upload`'s memcpy and
    // `returnToAllocator`'s free -- against a pool the GPU is reading from.
    // There is no fence anywhere on that path: the allocator's flush pushes CPU
    // caches *toward* the GPU and waits for nothing.
    //
    // A slot last drawn in frame N is referenced by frame N's list, which is in
    // flight for the whole of CPU frame N+1. It is free at CPU frame N+2. So
    // the block is untouchable while `frame_ - slot.frame < 2`, and `slot.frame`
    // -- maintained by `lruPushBack` on every upload and every touch -- is
    // already exactly the number this needs. Nothing else has to be recorded.
    //
    // Two is the value for `C3D_FRAME_SYNCDRAW`, where at most one frame is
    // ever in flight. It is a count of frames, not a guess at a duration; a
    // deeper pipeline would raise it.
    static constexpr u32 kRetireFrames = 2;

    struct Budget {
        usize vram = 0;
        usize linear = 0;
    };

    struct Stats {
        usize resident = 0;   // vertex bytes actually being drawn from
        usize reserved = 0;   // bytes held from the allocator, free lists included
        usize residentByTier[int(VboTier::kCount)] = {};
        usize reservedByTier[int(VboTier::kCount)] = {};

        int residents = 0;    // slots holding a live mesh
        int freeBlocks = 0;   // allocated blocks parked on a free list

        // Cumulative since reset, which is what makes churn visible: a pool
        // that is thrashing has evictions climbing every frame.
        u32 uploads = 0;
        u32 evictions = 0;
        u32 reused = 0;       // uploads served from a free list, no allocator call
        u32 allocations = 0;  // calls that reached the allocator
        u32 failures = 0;     // uploads the pool could not place at all

        // Blocks the ladder walked past because the GPU may still be reading
        // them. A few per frame is the policy working; a lot means the pool is
        // too small for the churn and uploads are being refused for it, which
        // is a different problem from being out of memory and has to look
        // different on the debug page.
        u32 heldInFlight = 0;
    };

    // Hands everything back. The allocator must outlive the pool, which on
    // both targets it does -- it is owned by the renderer that owns this.
    ~VboPool() { shutdown(); }

    // Sizes the pool. Safe to call again to re-budget; everything held is
    // returned to the allocator first.
    void reset(VboAllocator* allocator, Budget budget, const SizeClasses& classes);

    // Releases everything and forgets the allocator.
    void shutdown();

    // Starts a frame. Nothing is evicted that was drawn in the frame this
    // names, so a mesh uploaded after the draw list is built can never take
    // memory out from under something about to be drawn.
    //
    // Deliberately does *not* clear the eviction list: see `evicted()`.
    void beginFrame(u32 frame);

    // Copies `ranges.total()` bytes of vertex data in and returns the slot
    // holding it, or `kNoSlot` if the pool is full of geometry this frame is
    // already drawing. `owner` is an opaque token handed back through
    // `evicted()`.
    //
    // A section's mesh is three ranges back to back -- see mesh::MeshRanges --
    // and the pool carries their sizes so the draw loop can take them out
    // again. One block and one slot per section regardless: the split is about
    // draw order, and the passes are what change the shader, the attribute
    // layout and the blend state, each of them once per frame rather than once
    // per section.
    u16 upload(const void* data, u32 owner, const mesh::MeshRanges& ranges);

    // Marks a slot as drawn this frame, which is what keeps it out of the way
    // of eviction. Cheap: an unlink and a relink at the tail.
    void touch(u16 slot);

    // Gives a slot up voluntarily -- the section unloaded or changed. The block
    // stays in the pool, on its class's free list.
    void release(u16 slot);

    // Owners whose slots the pool has taken back and not yet been asked about.
    // The caller clears its own bookkeeping from this and then calls
    // `clearEvicted()`; the pool does not know what a section is.
    //
    // These accumulate until read, rather than being reset each frame, because
    // **an upload that returns `kNoSlot` can still have evicted**: the last
    // rung of the ladder gives up geometry and may then fail to place the mesh
    // anyway. Clearing per frame silently dropped exactly those, and a dropped
    // notification means the caller goes on drawing from a slot that now
    // belongs to a different section. The randomised test in
    // tests/vbo_pool_test.cpp caught it; a console would have shown it as one
    // chunk wearing another's geometry.
    Span<const u32> evicted() const { return Span<const u32>(evicted_); }
    void clearEvicted() { evicted_.clear(); }

    const void* data(u16 slot) const { return slots_[slot].data; }
    usize size(u16 slot) const { return slots_[slot].used; }

    // How the slot's one block divides into the three draw passes.
    const mesh::MeshRanges& ranges(u16 slot) const { return slots_[slot].ranges; }

    VboTier tier(u16 slot) const { return slots_[slot].tier; }
    u32 owner(u16 slot) const { return slots_[slot].owner; }

    // Bumped every time a block stops holding the mesh it held, so a draw list
    // built earlier in the frame can tell that the slot it recorded has since
    // changed hands. `beginFrame` snapshots the visible set, and an upload or an
    // eviction later in the same frame can hand that slot to another section --
    // which would otherwise be drawn with the *old* section's model matrix, i.e.
    // one chunk's geometry rendered where a different chunk is.
    u16 generation(u16 slot) const { return slots_[slot].generation; }
    bool resident(u16 slot) const { return slots_[slot].resident; }

    const Stats& stats() const { return stats_; }

    // Bytes the pool would have to hold to keep every resident at its exact
    // size. `reserved - resident` is the price of the size classes plus
    // whatever is parked on the free lists.
    usize wastedBytes() const { return stats_.reserved - stats_.resident; }

private:
    struct Slot {
        void* data = nullptr;
        usize capacity = 0;   // the size class, i.e. what the allocator gave
        usize used = 0;       // vertex bytes written into it
        mesh::MeshRanges ranges;  // how `used` divides into the three passes
        u32 owner = 0;
        u32 frame = 0;        // the frame it was last drawn in
        VboTier tier = VboTier::Linear;
        i16 sizeClass = -1;
        u16 prev = kNoSlot;   // least-recently-drawn list
        u16 next = kNoSlot;
        u16 generation = 0;   // bumped whenever the block changes hands
        bool resident = false;

        // Whether this block has ever held a mesh. `frame` is only meaningful
        // once it has: a block straight from the allocator has never been named
        // by any command list, so it is safe to write immediately, and frame
        // zero is a real frame number rather than a sentinel.
        bool everHeld = false;
    };

    // Whether the GPU can possibly still be fetching from this block. See
    // kRetireFrames. Written as a subtraction on unsigned frame numbers so it
    // stays correct across the counter wrapping.
    bool retired(u16 slot) const
    {
        const Slot& record = slots_[slot];
        return !record.everHeld || u32(frame_ - record.frame) >= kRetireFrames;
    }

    int freeListIndex(VboTier tier, int sizeClass) const
    {
        return int(tier) * classes_.count() + sizeClass;
    }

    u16 newSlot();
    u16 takeFree(int sizeClass);
    u16 allocateNew(int sizeClass);
    u16 reclaimFree(int sizeClass);
    u16 evictOfClass(int sizeClass);
    u16 evictAnyThenAllocate(int sizeClass);

    void evict(u16 slot);
    void parkOnFreeList(u16 slot);
    void returnToAllocator(u16 slot);

    void lruPushBack(u16 slot);
    void lruUnlink(u16 slot);

    VboAllocator* allocator_ = nullptr;
    Budget budget_;
    SizeClasses classes_;

    std::vector<Slot> slots_;
    std::vector<u16> unusedSlots_;              // slot records with no block
    std::vector<std::vector<u16>> freeLists_;   // [tier * classes + class]
    std::vector<u32> evicted_;

    u16 lruHead_ = kNoSlot;   // least recently drawn
    u16 lruTail_ = kNoSlot;   // most recently drawn
    u32 frame_ = 0;
    Stats stats_;
};

}  // namespace mc::render
