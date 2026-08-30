#include "core/tick/tick_scheduler.hpp"

#include <cstring>

namespace mc::tick {

namespace {

usize roundUpPowerOfTwo(usize n)
{
    usize p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

TickScheduler::TickScheduler(usize capacity) : capacity_(capacity ? capacity : 1)
{
    heap_.reserve(capacity_);
    // Half full at worst, so the probe chains stay short even once the pool is
    // saturated and every slot that is not live is a tombstone.
    const usize slots = roundUpPowerOfTwo(capacity_ * 2);
    index_.assign(slots, Slot{});
    // Allocated here so indexRebuild never has to; see indexRebuild.
    scratch_.assign(slots, Slot{});
    mask_ = u32(slots - 1);
}

u32 TickScheduler::hash(i32 x, int y, i32 z, block::BlockId id)
{
    // The original's `NextTickListEntry.hashCode` is
    // `(x * 128 * 1024 + z * 128 + y) * 256 + blockID`, which is a packing
    // rather than a hash and collides in long runs for the flat, axis-aligned
    // positions a fluid produces. Ours mixes, because an open-addressed table
    // is far more sensitive to clustering than java.util.HashMap's chaining.
    // Nothing observable depends on the function: it decides probe order, not
    // tick order.
    u32 h = u32(x) * 0x9E3779B1u;
    h ^= u32(z) * 0x85EBCA77u;
    h ^= u32(y) * 0xC2B2AE3Du;
    h ^= u32(id) * 0x27D4EB2Fu;
    h ^= h >> 15;
    return h;
}

bool TickScheduler::contains(i32 x, int y, i32 z, block::BlockId id) const
{
    u32 i = hash(x, y, z, id) & mask_;
    for (usize probe = 0; probe <= mask_; ++probe) {
        const Slot& s = index_[i];
        if (s.state == kEmpty) return false;
        if (s.state == kLive && s.x == x && s.z == z && s.y == i16(y) && s.blockId == id) {
            return true;
        }
        i = (i + 1) & mask_;
    }
    return false;
}

void TickScheduler::indexInsert(i32 x, int y, i32 z, block::BlockId id)
{
    u32 i = hash(x, y, z, id) & mask_;
    while (index_[i].state == kLive) i = (i + 1) & mask_;
    if (index_[i].state == kDead) --dead_;
    index_[i] = Slot{x, z, i16(y), id, kLive};
    ++live_;
}

void TickScheduler::indexRemove(i32 x, int y, i32 z, block::BlockId id)
{
    u32 i = hash(x, y, z, id) & mask_;
    for (usize probe = 0; probe <= mask_; ++probe) {
        Slot& s = index_[i];
        if (s.state == kEmpty) return;
        if (s.state == kLive && s.x == x && s.z == z && s.y == i16(y) && s.blockId == id) {
            s.state = kDead;
            --live_;
            ++dead_;
            if (dead_ * 4 >= index_.size()) indexRebuild();
            return;
        }
        i = (i + 1) & mask_;
    }
}

void TickScheduler::indexRebuild()
{
    // **Into a table that already exists.** This used to build a fresh vector
    // of index_.size() slots -- 8,192 x 16 bytes = 128 KB at the default
    // capacity -- and free the old one, inside runScheduled, once every couple
    // of thousand pops. Every scheduled tick that fires leaves a tombstone, so
    // an active fluid reaches that rate routinely, and the frame path is
    // exactly where this project does not allocate. The spare table is
    // allocated once beside the first one and the two alternate.
    scratch_.assign(index_.size(), Slot{});
    index_.swap(scratch_);
    live_ = 0;
    dead_ = 0;
    for (const ScheduledTick& e : heap_) indexInsert(e.x, e.y, e.z, e.blockId);
}

bool TickScheduler::schedule(i32 x, int y, i32 z, block::BlockId id, i64 time)
{
    if (contains(x, y, z, id)) return false;
    if (heap_.size() >= capacity_) {
        ++overflowed_;
        return false;
    }

    ScheduledTick entry;
    entry.x = x;
    entry.z = z;
    entry.y = i16(y);
    entry.blockId = id;
    entry.time = time;
    entry.sequence = nextSequence_++;

    heap_.push_back(entry);
    indexInsert(x, y, z, id);
    siftUp(heap_.size() - 1);
    return true;
}

ScheduledTick TickScheduler::pop()
{
    if (heap_.empty()) return ScheduledTick{};
    const ScheduledTick out = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();
    if (!heap_.empty()) siftDown(0);
    indexRemove(out.x, out.y, out.z, out.blockId);
    return out;
}

void TickScheduler::clear()
{
    heap_.clear();
    index_.assign(index_.size(), Slot{});
    live_ = 0;
    dead_ = 0;
    overflowed_ = 0;
}

bool TickScheduler::consistent() const
{
    if (live_ != heap_.size()) return false;
    for (const ScheduledTick& e : heap_) {
        if (!contains(e.x, e.y, e.z, e.blockId)) return false;
    }
    return true;
}

void TickScheduler::siftUp(usize i)
{
    while (i > 0) {
        const usize parent = (i - 1) / 2;
        if (!before(heap_[i], heap_[parent])) break;
        ScheduledTick tmp = heap_[parent];
        heap_[parent] = heap_[i];
        heap_[i] = tmp;
        i = parent;
    }
}

void TickScheduler::siftDown(usize i)
{
    const usize n = heap_.size();
    for (;;) {
        const usize left = i * 2 + 1;
        if (left >= n) break;
        usize best = left;
        const usize right = left + 1;
        if (right < n && before(heap_[right], heap_[left])) best = right;
        if (!before(heap_[best], heap_[i])) break;
        ScheduledTick tmp = heap_[i];
        heap_[i] = heap_[best];
        heap_[best] = tmp;
        i = best;
    }
}

}  // namespace mc::tick
