#include "core/util/chunk_queue.hpp"

#include <cstring>

namespace mc {

namespace {

// The same multiply-xor MapStore hashes a chunk coordinate with, and for the
// same reason: the coordinates pushed here arrive as solid rectangles of
// neighbours, so a hash that maps neighbours to neighbouring slots would turn
// every probe into a walk down one long run.
u32 hashChunk(i32 x, i32 z)
{
    u32 h = u32(x) * 0x9E3779B1u;
    h ^= u32(z) * 0x85EBCA77u;
    h ^= h >> 15;
    return h;
}

}  // namespace

void ChunkQueue::setCapacity(int chunks)
{
    if (chunks < 1) {
        chunks = 1;
    }
    ring_.assign(usize(chunks), Slot{});
    capacity_ = chunks;

    usize slots = 1;
    while (slots < usize(chunks) * 4) {
        slots <<= 1;
    }
    index_.assign(slots, kEmpty);

    head_ = 0;
    count_ = 0;
    tombstones_ = 0;
    overflowed_ = false;
}

// The slot this coordinate lives in, or -1. Stops at the first never-used slot:
// a tombstone means the run continues, which is exactly what it is for.
int ChunkQueue::probeFor(i32 chunkX, i32 chunkZ) const
{
    if (index_.empty()) {
        return -1;
    }
    const usize mask = index_.size() - 1;
    usize slot = usize(hashChunk(chunkX, chunkZ)) & mask;
    for (usize probe = 0; probe <= mask; ++probe) {
        const i32 at = index_[slot];
        if (at == kEmpty) {
            return -1;
        }
        if (at != kDead && ring_[usize(at)].x == chunkX && ring_[usize(at)].z == chunkZ) {
            return int(slot);
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

// Throws the tombstones away by rebuilding from the live entries. One pass over
// the table plus one over the queue, run when the dead outnumber what four
// times the capacity leaves room for -- so not on any particular push, and
// never more than once per capacity's worth of pops.
void ChunkQueue::rehash()
{
    std::memset(index_.data(), 0xFF, index_.size() * sizeof(i32));
    static_assert(kEmpty == -1, "0xFF fill must spell kEmpty");
    tombstones_ = 0;

    const usize mask = index_.size() - 1;
    for (usize i = 0; i < count_; ++i) {
        const usize at = (head_ + i) % usize(capacity_);
        usize slot = usize(hashChunk(ring_[at].x, ring_[at].z)) & mask;
        while (index_[slot] != kEmpty) {
            slot = (slot + 1) & mask;
        }
        index_[slot] = i32(at);
    }
}

bool ChunkQueue::push(i32 chunkX, i32 chunkZ)
{
    if (index_.empty()) {
        return false;
    }
    if (probeFor(chunkX, chunkZ) >= 0) {
        return false;
    }
    if (full()) {
        overflowed_ = true;
        return false;
    }

    // Before the insert, so the insert below can trust that a free slot exists
    // and that the run it walks is not mostly dead.
    if (count_ + tombstones_ + 1 > index_.size() / 2) {
        rehash();
    }

    // **The ring position is the entry's identity and it never moves**, which
    // is what lets the table hold positions rather than coordinates: an entry
    // is written into a fixed slot of the ring and stays there until it is
    // popped, so nothing in the table goes stale while the queue is walked.
    const usize at = (head_ + count_) % usize(capacity_);
    ring_[at].x = chunkX;
    ring_[at].z = chunkZ;

    const usize mask = index_.size() - 1;
    usize slot = usize(hashChunk(chunkX, chunkZ)) & mask;
    while (index_[slot] != kEmpty && index_[slot] != kDead) {
        slot = (slot + 1) & mask;
    }
    if (index_[slot] == kDead) {
        --tombstones_;
    }
    index_[slot] = i32(at);
    ++count_;
    return true;
}

bool ChunkQueue::pop(i32* chunkX, i32* chunkZ)
{
    if (count_ == 0) {
        return false;
    }
    const Slot& slot = ring_[head_];
    if (chunkX != nullptr) {
        *chunkX = slot.x;
    }
    if (chunkZ != nullptr) {
        *chunkZ = slot.z;
    }

    const int at = probeFor(slot.x, slot.z);
    if (at >= 0) {
        index_[usize(at)] = kDead;
        ++tombstones_;
    }

    head_ = (head_ + 1) % usize(capacity_);
    --count_;
    return true;
}

bool ChunkQueue::contains(i32 chunkX, i32 chunkZ) const
{
    return probeFor(chunkX, chunkZ) >= 0;
}

void ChunkQueue::clear()
{
    if (!index_.empty()) {
        std::memset(index_.data(), 0xFF, index_.size() * sizeof(i32));
    }
    head_ = 0;
    count_ = 0;
    tombstones_ = 0;
    // **Not the overflow flag.** It says the consumer has lost coordinates and
    // must recover; emptying the queue is not the consumer having done so.
}

}  // namespace mc
