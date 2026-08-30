#pragma once

// Blocks that have asked to be updated at a stated tick, in the order the
// original would reach them.
//
// This is a1.1.2's `pendingTickListEntries` (a `TreeSet<NextTickListEntry>`)
// and `scheduledTickSet` (a `HashSet` beside it holding exactly the same
// members) collapsed into one structure. The pair exists in the original
// because a TreeSet answers "what is next" and a HashSet answers "is this
// already queued", and it checks them against each other every tick --
// `if (sizes differ) throw new IllegalStateException("TickNextTick list out of
// synch")`. We keep both answers and the invariant, in one object.
//
// **The ordering is the behaviour, not an implementation detail.** Entries
// compare on `scheduledTime` first and on a global monotonic *insertion
// sequence* second, recovered from `jf.a(Ljf;)I`. Two updates due on the same
// tick therefore run oldest-first, and that is what makes water spread in a
// stable, reproducible pattern instead of a different one each run. Dropping
// the sequence and comparing on coordinates -- the obvious thing -- changes
// the shape of every lake.
//
// Identity is `(x, y, z, blockId)`, from `jf.equals`: the same position with a
// different block is a different entry, which is how a block that replaces
// itself does not inherit the old block's pending update.
//
// **Two deviations from the TreeSet, both forced by the console.**
//
//   * It is bounded. `std::set` allocates a node per entry and the frame path
//     may not allocate, so this is a fixed-capacity pool: a binary min-heap
//     over an array, with an open-addressed index beside it for the identity
//     test. Capacity is chosen at construction and never grows. When it is
//     full, the *new* entry is refused and `overflowed()` counts it, because
//     refusing the newest is the failure that recovers -- the queue drains and
//     the next neighbour notification re-schedules whatever was lost.
//   * The original caps a tick at 1,000 entries processed; that cap is kept
//     (see `kMaxPerTick`) and is a1.1.2's, not ours.

#include "core/block/block_def.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::tick {

struct ScheduledTick {
    i32 x = 0;
    i32 z = 0;
    i16 y = 0;
    block::BlockId blockId = 0;
    i64 time = 0;
    // The original's `NextTickListEntry.tickEntryID`, a static counter bumped
    // in the constructor. It is the whole tie-break, so it must be assigned
    // when the entry is *created* rather than when it is popped.
    u64 sequence = 0;
};

class TickScheduler {
public:
    // a1.1.2's own limit on how many pending updates one world tick runs, from
    // `cn.a(Z)Z`. Everything past the thousandth waits for the next tick.
    static constexpr int kMaxPerTick = 1000;

    explicit TickScheduler(usize capacity = 4096);

    // False when the entry is already queued -- the original's
    // `if (!scheduledTickSet.contains(entry))` -- or when the pool is full.
    bool schedule(i32 x, int y, i32 z, block::BlockId id, i64 time);

    // Is anything due at or before `time`? Cheap: the heap root.
    bool dueAt(i64 time) const { return !heap_.empty() && heap_.front().time <= time; }

    // Removes and returns the next entry. The caller must have checked
    // `dueAt`; popping an empty scheduler returns a zeroed entry.
    ScheduledTick pop();

    usize size() const { return heap_.size(); }
    usize capacity() const { return capacity_; }
    bool empty() const { return heap_.empty(); }
    i64 overflowed() const { return overflowed_; }

    void clear();

    // The original's own consistency check, kept because it caught real bugs
    // in 2010 and catches the same ones here: the ordered structure and the
    // membership structure must hold exactly the same entries.
    bool consistent() const;

private:
    usize capacity_;
    std::vector<ScheduledTick> heap_;

    // Membership, kept beside the heap rather than inside it: open addressing
    // with linear probing over a power-of-two table twice the pool's size, and
    // tombstones on removal. It stores the identity only and never a heap
    // position, so the heap can sift freely without touching it -- which is
    // the whole reason the two structures are separate here as they are in the
    // original. Tombstones are swept when they reach a quarter of the table.
    struct Slot {
        i32 x = 0;
        i32 z = 0;
        i16 y = 0;
        block::BlockId blockId = 0;
        u8 state = kEmpty;
    };
    static constexpr u8 kEmpty = 0;
    static constexpr u8 kLive = 1;
    static constexpr u8 kDead = 2;

    std::vector<Slot> index_;

    // The other half of the pair indexRebuild swaps between, so a rebuild is a
    // fill and a swap rather than an allocation and a free. Sized once, with
    // index_, and never resized.
    std::vector<Slot> scratch_;
    u32 mask_ = 0;
    usize live_ = 0;
    usize dead_ = 0;

    u64 nextSequence_ = 0;
    i64 overflowed_ = 0;

    static u32 hash(i32 x, int y, i32 z, block::BlockId id);
    bool contains(i32 x, int y, i32 z, block::BlockId id) const;

    void indexInsert(i32 x, int y, i32 z, block::BlockId id);
    void indexRemove(i32 x, int y, i32 z, block::BlockId id);
    void indexRebuild();

    static bool before(const ScheduledTick& a, const ScheduledTick& b)
    {
        if (a.time != b.time) return a.time < b.time;
        return a.sequence < b.sequence;
    }

    void siftUp(usize i);
    void siftDown(usize i);
};

}  // namespace mc::tick
