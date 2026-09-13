#pragma once

// A list of chunk coordinates waiting for something to be done to them, which
// holds each coordinate **once** however many times it is offered.
//
// Two things in this tree want exactly that and neither of them wants it
// urgently: `WorldStreamer` collects the columns the world has written into, so
// the map can find out what changed without walking its window, and the map
// collects the chunks it still owes a sample. Both producers are far faster
// than their consumer -- a spreading fluid changes hundreds of blocks in one
// chunk in one tick, and the map samples a handful of chunks a frame -- so the
// dedupe is not a tidiness measure. Without it a chunk under a waterfall would
// be sampled once per block change for ever, and the frame would be spent
// re-reading the same 256 columns.
//
// **Nothing here allocates after `setCapacity`**, and nothing here can grow
// without bound: a push onto a full queue is refused and counted, and a
// consumer that must miss nothing reads `overflowed()` and falls back to
// whatever it does from cold. The alternative -- a queue that grows -- puts the
// worst case on the heap of a console with 36 MB of it, at the one moment the
// world is busiest.
//
// **It is FIFO, and the order it is filled in is the order it is drained in.**
// That is worth saying because the map's producer is a nearest-first ring walk,
// so the queue inherits nearest-first without knowing anything about where the
// player is. Nothing here sorts.

#include "core/util/types.hpp"

#include <vector>

namespace mc {

class ChunkQueue {
public:
    // How many coordinates may be waiting at once. Allocates the ring and the
    // membership table once, here. Called again with a different number throws
    // the contents away rather than moving them; it happens at world open.
    void setCapacity(int chunks);
    int capacity() const { return capacity_; }

    int size() const { return int(count_); }
    bool empty() const { return count_ == 0; }
    bool full() const { return capacity_ > 0 && count_ == usize(capacity_); }

    // **True when this coordinate was actually added.** False both for one
    // already waiting -- the common answer, and the whole point -- and for a
    // push onto a full queue, which is counted in `overflowed()` instead.
    bool push(i32 chunkX, i32 chunkZ);

    // The oldest coordinate waiting. False when there is none.
    bool pop(i32* chunkX, i32* chunkZ);

    bool contains(i32 chunkX, i32 chunkZ) const;

    void clear();

    // **Pushes refused because the queue was full**, cumulative until read.
    // Reading it clears it, because the only sensible response is to recover
    // once rather than once per lost coordinate.
    bool overflowed()
    {
        const bool was = overflowed_;
        overflowed_ = false;
        return was;
    }

private:
    struct Slot {
        i32 x = 0;
        i32 z = 0;
    };

    // Open addressing with linear probing, the same shape MapStore's index
    // uses, over a table four times the capacity -- wider than that one on
    // purpose. This table has to survive removal, and removal from a
    // linear-probe run means either shifting the run back or leaving a
    // tombstone; tombstones are the simpler of the two to be sure of, and they
    // fill the table up as the queue is drained and refilled. Four times the
    // capacity leaves room for as many tombstones as live entries before the
    // rehash below is needed, so a queue drained once per frame rehashes every
    // few hundred frames and never in the middle of a push.
    static constexpr i32 kEmpty = -1;
    static constexpr i32 kDead = -2;

    int probeFor(i32 chunkX, i32 chunkZ) const;
    void rehash();

    std::vector<Slot> ring_;
    std::vector<i32> index_;

    int capacity_ = 0;
    usize head_ = 0;
    usize count_ = 0;
    usize tombstones_ = 0;
    bool overflowed_ = false;
};

}  // namespace mc
