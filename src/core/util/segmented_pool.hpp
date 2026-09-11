#pragma once

// **A pool that grows instead of refusing** -- what every entity pool is.
//
// a1.1.2 keeps its entities in `ArrayList`s and caps none of them:
// `World.spawnEntityInWorld` adds, `EffectRenderer.addEffect` adds (read off
// `bq.a(Lnq;)V`: one `List.add` and a return), and neither asks how many there
// are. The fixed arrays this replaces -- eight boats, thirty-two carts, 128
// arrows -- were this port's limit and not the game's, and a player met them as
// arrows that stopped firing and carts that would not go down.
//
// Three properties, and each is the reason for a piece of the shape:
//
//   * **Elements never move.** Storage is a list of fixed-size segments, so
//     growing allocates a new segment and copies nothing. A reference taken
//     into the pool stays valid across a push into the same pool -- a
//     `std::vector` would break that silently the first time it reallocated.
//   * **The first segment is the old fixed capacity and is taken at
//     construction**, so ordinary play allocates exactly as often as it did:
//     never. Only a pool pushed past it allocates, one segment at a time.
//   * **Growth can be refused, and a refusal is not a crash.** It goes through
//     `std::malloc`, never `operator new` -- which on the console ends the
//     process (platform/ctr/heap.hpp) -- and asks `poolGrowthAllowed` first,
//     so the pools cannot eat the headroom the chunk cache and the save need.
//     A refused `push` answers null and the caller does what it used to do on a
//     full array. More entities cost frames; they never cost the process.
//
// Removal is swap-with-last, which every pool here already did. **`trim` is
// separate and explicit** rather than part of removal, so a tick that removes
// while it walks never has a segment freed under a reference it still holds;
// the pools trim at the end of their tick. It keeps one empty segment of
// hysteresis, so a pool sitting on a boundary does not allocate and free on
// alternate ticks.

#include "core/util/memory.hpp"
#include "core/util/types.hpp"

#include <cstdlib>
#include <new>
#include <type_traits>

namespace mc {

template <class T, int SegmentSize>
class SegmentedPool {
    static_assert(SegmentSize > 0 && (SegmentSize & (SegmentSize - 1)) == 0,
                  "a segment is a power of two, so indexing is a shift and a mask");
    static_assert(std::is_trivially_destructible<T>::value,
                  "segments are released without running destructors");

public:
    static constexpr int kSegment = SegmentSize;
    static constexpr usize kSegmentBytes = sizeof(T) * usize(SegmentSize);

    SegmentedPool() { grow(true); }
    ~SegmentedPool()
    {
        releaseDownTo(0);
        std::free(segments_);
    }
    SegmentedPool(const SegmentedPool&) = delete;
    SegmentedPool& operator=(const SegmentedPool&) = delete;

    // Moves hand the segments over and copy nothing. A moved-from pool is
    // empty and holds no segment; its next push asks the heap like any growth.
    SegmentedPool(SegmentedPool&& other) noexcept { swap(other); }
    SegmentedPool& operator=(SegmentedPool&& other) noexcept
    {
        swap(other);
        return *this;
    }

    int size() const { return count_; }
    bool empty() const { return count_ == 0; }
    int capacity() const { return segmentCount_ * SegmentSize; }
    int segments() const { return segmentCount_; }

    T& operator[](int i) { return segments_[unsigned(i) / kSeg][unsigned(i) % kSeg]; }
    const T& operator[](int i) const
    {
        return segments_[unsigned(i) / kSeg][unsigned(i) % kSeg];
    }

    // A fresh, default-constructed slot at the end, or null when the pool is
    // full and may not grow.
    T* push()
    {
        if (count_ == capacity() && !grow(false)) {
            return nullptr;
        }
        T& slot = (*this)[count_++];
        slot = T{};
        return &slot;
    }

    // The last element into the hole. Invalidates only references to the
    // element removed and to the one that was last.
    void swapRemove(int index)
    {
        if (index != count_ - 1) {
            (*this)[index] = (*this)[count_ - 1];
        }
        --count_;
    }

    // Keeps the first `n`, for a pool that compacts in place.
    void truncate(int n)
    {
        if (n >= 0 && n < count_) {
            count_ = n;
        }
    }

    void clear()
    {
        count_ = 0;
        trim();
    }

    // Hands back every segment past the first that sits behind a second empty
    // one. Never touches a live element.
    void trim()
    {
        int keep = (count_ + SegmentSize - 1) / SegmentSize + 1;
        if (keep < 1) {
            keep = 1;
        }
        if (segmentCount_ > keep) {
            releaseDownTo(keep);
        }
    }

private:
    static constexpr unsigned kSeg = unsigned(SegmentSize);

    void swap(SegmentedPool& other)
    {
        T** segments = segments_;
        const int segmentCount = segmentCount_;
        const int tableSlots = tableSlots_;
        const int count = count_;
        segments_ = other.segments_;
        segmentCount_ = other.segmentCount_;
        tableSlots_ = other.tableSlots_;
        count_ = other.count_;
        other.segments_ = segments;
        other.segmentCount_ = segmentCount;
        other.tableSlots_ = tableSlots;
        other.count_ = count;
    }

    bool grow(bool first)
    {
        if (!first && !poolGrowthAllowed(kSegmentBytes)) {
            return false;
        }
        if (segmentCount_ == tableSlots_) {
            const int slots = tableSlots_ == 0 ? 4 : tableSlots_ * 2;
            void* table = std::realloc(segments_, sizeof(T*) * usize(slots));
            if (table == nullptr) {
                return false;
            }
            segments_ = static_cast<T**>(table);
            tableSlots_ = slots;
        }
        T* segment = static_cast<T*>(std::malloc(kSegmentBytes));
        if (segment == nullptr) {
            return false;
        }
        for (int i = 0; i < SegmentSize; ++i) {
            new (segment + i) T();
        }
        segments_[segmentCount_++] = segment;
        poolBytesTaken(kSegmentBytes);
        return true;
    }

    void releaseDownTo(int keep)
    {
        while (segmentCount_ > keep) {
            std::free(segments_[--segmentCount_]);
            poolBytesReleased(kSegmentBytes);
        }
    }

    T** segments_ = nullptr;
    int segmentCount_ = 0;
    int tableSlots_ = 0;
    int count_ = 0;
};

}  // namespace mc
