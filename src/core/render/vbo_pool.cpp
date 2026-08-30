#include "core/render/vbo_pool.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace mc::render {

bool SizeClasses::build(usize smallest, usize largest, double ratio)
{
    assert(smallest > 0 && ratio > 1.0);

    count_ = 0;
    usize size = smallest;
    while (count_ < kMaxClasses) {
        bytes_[count_++] = size;
        if (size >= largest) {
            break;
        }
        // Rounded up, and forced to grow by at least one byte: a ratio close
        // to 1 would otherwise produce two equal classes and stall the loop.
        const usize next = usize(std::ceil(double(size) * ratio));
        size = next > size ? next : size + 1;
    }
    return capacity() >= largest;
}

int SizeClasses::classFor(usize bytes) const
{
    for (int i = 0; i < count_; ++i) {
        if (bytes_[i] >= bytes) {
            return i;
        }
    }
    return -1;
}

void VboPool::reset(VboAllocator* allocator, Budget budget, const SizeClasses& classes)
{
    shutdown();

    allocator_ = allocator;
    budget_ = budget;
    classes_ = classes;
    freeLists_.assign(usize(int(VboTier::kCount) * classes_.count()), {});
    stats_ = Stats();
}

void VboPool::shutdown()
{
    if (allocator_ != nullptr) {
        for (Slot& slot : slots_) {
            if (slot.data != nullptr) {
                allocator_->release(slot.data, slot.capacity, slot.tier);
            }
        }
    }
    slots_.clear();
    unusedSlots_.clear();
    freeLists_.clear();
    evicted_.clear();
    lruHead_ = kNoSlot;
    lruTail_ = kNoSlot;
    allocator_ = nullptr;
    stats_ = Stats();
}

void VboPool::beginFrame(u32 frame)
{
    frame_ = frame;
}

u16 VboPool::upload(const void* data, u32 owner, const mesh::MeshRanges& ranges)
{
    assert(allocator_ != nullptr && "reset() the pool before uploading to it");
    const usize bytes = ranges.total();
    if (bytes == 0) {
        return kNoSlot;
    }

    const int sizeClass = classes_.classFor(bytes);
    if (sizeClass < 0) {
        // Larger than the biggest class. The mesher already asserts against
        // kMaxQuadsPerSection, so this means the pool was built with a table
        // that does not cover a section -- a configuration error, not a
        // runtime condition.
        ++stats_.failures;
        return kNoSlot;
    }

    // In order of what it costs. A free block of the right class is free; the
    // allocator is cheap but fragments; eviction throws geometry away.
    u16 slot = takeFree(sizeClass);
    if (slot == kNoSlot) {
        slot = allocateNew(sizeClass);
    }
    if (slot == kNoSlot) {
        slot = reclaimFree(sizeClass);
    }
    if (slot == kNoSlot) {
        slot = evictOfClass(sizeClass);
    }
    if (slot == kNoSlot) {
        slot = evictAnyThenAllocate(sizeClass);
    }
    if (slot == kNoSlot) {
        // Everything resident is being drawn this frame. Refusing is correct:
        // the caller retries next frame, when the view has moved on, and
        // nothing on screen flickers out to make room for something that is
        // not on screen yet.
        ++stats_.failures;
        return kNoSlot;
    }

    // Every rung of the ladder above refuses a block the GPU may still be
    // reading, so by here this is guaranteed rather than hoped for. Stated as
    // an assert because the failure it guards against is invisible on the host
    // and intermittent on hardware.
    assert(retired(slot) && "the ladder handed back a block still in flight");

    std::memcpy(slots_[slot].data, data, bytes);

    // The CPU wrote it and the GPU is about to read it, and on a console those
    // two do not see the same cache. Doing it here rather than at the call site
    // is the difference between "correct" and "correct as long as everybody
    // remembers": a missed flush shows up as a chunk of stale or garbage
    // geometry, intermittently, on hardware only.
    allocator_->flush(slots_[slot].data, bytes, slots_[slot].tier);

    slots_[slot].used = bytes;
    slots_[slot].ranges = ranges;
    slots_[slot].owner = owner;
    slots_[slot].resident = true;
    lruPushBack(slot);

    stats_.resident += bytes;
    stats_.residentByTier[int(slots_[slot].tier)] += bytes;
    ++stats_.residents;
    ++stats_.uploads;
    return slot;
}

void VboPool::touch(u16 slot)
{
    if (slot == kNoSlot || !slots_[slot].resident) {
        return;
    }
    if (slots_[slot].frame == frame_) {
        return;  // already at the tail; relinking would be pure work
    }
    lruUnlink(slot);
    lruPushBack(slot);
}

void VboPool::release(u16 slot)
{
    if (slot == kNoSlot || !slots_[slot].resident) {
        return;
    }
    lruUnlink(slot);
    parkOnFreeList(slot);
}

// --------------------------------------------------------------- the ladder

u16 VboPool::takeFree(int sizeClass)
{
    // VRAM first, and only for a block that already exists: this is the step
    // that costs nothing, so it never has a reason to prefer the slower tier.
    //
    // **Oldest first, and never one the GPU may still be reading.** This used
    // to take `list.back()`, which is the worst possible choice: the block a
    // section gave up when it was invalidated is the newest on the list, so the
    // very next upload of that size handed the section its own block back and
    // memcpy'd over it -- while the frame that drew it was still in flight. The
    // symptom on hardware is triangles stretched between two unrelated
    // positions, intermittently, mostly while moving. See kRetireFrames.
    for (int tier = 0; tier < int(VboTier::kCount); ++tier) {
        std::vector<u16>& list = freeLists_[usize(freeListIndex(VboTier(tier), sizeClass))];
        for (usize i = 0; i < list.size(); ++i) {
            if (!retired(list[i])) {
                ++stats_.heldInFlight;
                continue;
            }
            const u16 slot = list[i];
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
            --stats_.freeBlocks;
            ++stats_.reused;
            return slot;
        }
    }
    return kNoSlot;
}

u16 VboPool::allocateNew(int sizeClass)
{
    const usize size = classes_.bytes(sizeClass);

    // VRAM first. Nothing here sorts by distance -- `VisibleSet::toMesh` hands
    // sections over nearest first, so filling VRAM in arrival order fills it
    // with the nearest geometry, which is the whole of the policy.
    for (int tier = 0; tier < int(VboTier::kCount); ++tier) {
        const usize cap = tier == int(VboTier::Vram) ? budget_.vram : budget_.linear;
        if (stats_.reservedByTier[tier] + size > cap) {
            continue;
        }
        void* memory = allocator_->allocate(size, VboTier(tier));
        if (memory == nullptr) {
            continue;  // the tier is gone even though the budget said otherwise
        }

        const u16 slot = newSlot();
        slots_[slot].data = memory;
        slots_[slot].capacity = size;
        slots_[slot].tier = VboTier(tier);
        slots_[slot].sizeClass = i16(sizeClass);
        stats_.reserved += size;
        stats_.reservedByTier[tier] += size;
        ++stats_.allocations;
        return slot;
    }
    return kNoSlot;
}

u16 VboPool::reclaimFree(int sizeClass)
{
    // Both budgets are spoken for, but some of that is blocks parked on the
    // wrong free lists. Hand the biggest ones back until the allocator can
    // serve the size actually wanted. Biggest first because it takes the
    // fewest releases to make room, and because a big free block is the one
    // least likely to be asked for again soon.
    const usize wanted = classes_.bytes(sizeClass);
    bool released = false;

    for (int cls = classes_.count() - 1; cls >= 0; --cls) {
        for (int tier = 0; tier < int(VboTier::kCount); ++tier) {
            std::vector<u16>& list = freeLists_[usize(freeListIndex(VboTier(tier), cls))];
            // Front to back, skipping anything still in flight: `linearFree` on
            // a block the GPU is fetching from is the same bug as memcpy'ing
            // over it, only louder -- the address can be handed straight back
            // out for something else entirely.
            for (usize i = 0; i < list.size();) {
                if (!retired(list[i])) {
                    ++stats_.heldInFlight;
                    ++i;
                    continue;
                }
                const u16 slot = list[i];
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                --stats_.freeBlocks;
                returnToAllocator(slot);
                released = true;

                // Enough room in either tier is enough; try the allocator
                // again rather than releasing more than necessary.
                if (stats_.reservedByTier[int(VboTier::Vram)] + wanted <= budget_.vram
                    || stats_.reservedByTier[int(VboTier::Linear)] + wanted
                           <= budget_.linear) {
                    const u16 fresh = allocateNew(sizeClass);
                    if (fresh != kNoSlot) {
                        return fresh;
                    }
                }
            }
        }
    }
    return released ? allocateNew(sizeClass) : kNoSlot;
}

u16 VboPool::evictOfClass(int sizeClass)
{
    // The steady state. The working set is roughly constant in size, so the
    // section that just went out of view is usually holding a block the
    // section coming into view can use as it stands -- no allocator traffic,
    // no fragmentation, one memcpy.
    for (u16 slot = lruHead_; slot != kNoSlot; slot = slots_[slot].next) {
        if (!retired(slot)) {
            // The LRU order means everything past here is at least as recent.
            // This used to test `frame == frame_`, which protects only the
            // frame being recorded -- and the frame the GPU is *executing* is
            // the one before it. A section drawn last frame and dropped from
            // this frame's draw list, which is exactly what turning the camera
            // produces, was a legal eviction target mid-draw.
            ++stats_.heldInFlight;
            break;
        }
        if (slots_[slot].sizeClass == i16(sizeClass)) {
            evict(slot);
            // evict() parks it; take it straight back off the list.
            return takeFree(sizeClass);
        }
    }
    return kNoSlot;
}

u16 VboPool::evictAnyThenAllocate(int sizeClass)
{
    // Nothing of the right size is going spare, so this costs both an eviction
    // and allocator traffic. It is the path that runs while the pool is still
    // settling into a new view, and it should get rarer, not more common --
    // which is why `evictions` is cumulative in the stats.
    while (lruHead_ != kNoSlot && retired(lruHead_)) {
        evict(lruHead_);
        const u16 slot = reclaimFree(sizeClass);
        if (slot != kNoSlot) {
            return slot;
        }
    }
    return kNoSlot;
}

// ------------------------------------------------------------- bookkeeping

u16 VboPool::newSlot()
{
    if (!unusedSlots_.empty()) {
        const u16 slot = unusedSlots_.back();
        unusedSlots_.pop_back();
        const u16 generation = slots_[slot].generation;
        slots_[slot] = Slot();
        slots_[slot].generation = generation;
        return slot;
    }
    assert(slots_.size() < kNoSlot && "more pool slots than a u16 can address");
    slots_.push_back(Slot());
    return u16(slots_.size() - 1);
}

void VboPool::evict(u16 slot)
{
    assert(slots_[slot].resident);
    evicted_.push_back(slots_[slot].owner);
    ++stats_.evictions;
    lruUnlink(slot);
    parkOnFreeList(slot);
}

void VboPool::parkOnFreeList(u16 slot)
{
    Slot& record = slots_[slot];
    stats_.resident -= record.used;
    stats_.residentByTier[int(record.tier)] -= record.used;
    --stats_.residents;

    record.resident = false;
    record.used = 0;
    // The mesh this block held is no longer the mesh anyone recorded a slot
    // index for. A draw list built earlier this frame still names it; bumping
    // here is what lets the draw loop notice rather than render one chunk's
    // geometry at another chunk's origin.
    ++record.generation;
    freeLists_[usize(freeListIndex(record.tier, record.sizeClass))].push_back(slot);
    ++stats_.freeBlocks;
}

void VboPool::returnToAllocator(u16 slot)
{
    Slot& record = slots_[slot];
    assert(!record.resident && "a live mesh must be evicted before its block goes back");
    assert(retired(slot) && "a block the GPU may still be reading must not be freed");
    allocator_->release(record.data, record.capacity, record.tier);
    stats_.reserved -= record.capacity;
    stats_.reservedByTier[int(record.tier)] -= record.capacity;

    // The slot record is recycled but its generation is not: a draw list that
    // named this index has to keep disagreeing with it, and resetting to zero
    // would eventually let a stale index match again.
    const u16 generation = record.generation;
    record = Slot();
    record.generation = u16(generation + 1);
    unusedSlots_.push_back(slot);
}

void VboPool::lruPushBack(u16 slot)
{
    Slot& record = slots_[slot];
    record.frame = frame_;
    record.everHeld = true;
    record.prev = lruTail_;
    record.next = kNoSlot;
    if (lruTail_ != kNoSlot) {
        slots_[lruTail_].next = slot;
    } else {
        lruHead_ = slot;
    }
    lruTail_ = slot;
}

void VboPool::lruUnlink(u16 slot)
{
    Slot& record = slots_[slot];
    if (record.prev != kNoSlot) {
        slots_[record.prev].next = record.next;
    } else if (lruHead_ == slot) {
        lruHead_ = record.next;
    }
    if (record.next != kNoSlot) {
        slots_[record.next].prev = record.prev;
    } else if (lruTail_ == slot) {
        lruTail_ = record.prev;
    }
    record.prev = kNoSlot;
    record.next = kNoSlot;
}

}  // namespace mc::render
