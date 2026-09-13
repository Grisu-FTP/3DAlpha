#pragma once

// A snapshot of the session's persistent entity pools. The current
// pools are world-wide, so their save is a namespaced level.dat extension,
// independent of chunk eviction. Native chunk Entities stay preserved verbatim;
// importing/exporting those entities remains a separate compatibility task.
// Snapshots are immutable once queued: the I/O worker never reads live pools.
//
// The pools have no count limit, so neither does this: each list is a heap
// array sized when it is filled, through `malloc` so that running out is an
// answer (`capture` returns false) rather than the end of the process.

#include "core/entity/painting.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/falling_block.hpp"
#include "core/entity/primed_tnt.hpp"
#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"

#include <cstdlib>
#include <type_traits>

namespace mc::entity {
struct EntityPools {
    PaintingSystem* paintings = nullptr;
    ArrowSystem* arrows = nullptr;
    BoatSystem* boats = nullptr;
    MinecartSystem* minecarts = nullptr;
    ItemEntitySystem* items = nullptr;
    FallingBlockSystem* fallingBlocks = nullptr;

    // **What is counting down.** Held with the rest rather than in a chunk's
    // `Entities` for the same reason they are -- see the note on `mobs` below.
    // A world saved with TNT in the air reloads with the fuse where it was.
    PrimedTntSystem* primedTnt = nullptr;

    // **The animals**, which are the first entities here that a1.1.2 itself
    // would have written into a chunk's `Entities` list rather than into
    // level.dat. They go where the other six go for the same reason -- the
    // `entitydata` slot is still `none` and native chunk entities stay
    // preserved verbatim -- so a world carried back to the real client keeps
    // its animals only as long as this port is the thing opening it. See
    // docs/current-work.md, *Compatibility limit*.
    MobSystem* mobs = nullptr;
};

template <class T> class SavedPool {
    static_assert(std::is_trivially_copyable<T>::value, "grown with realloc");

public:
    SavedPool() = default;
    ~SavedPool();
    SavedPool(const SavedPool&) = delete;
    SavedPool& operator=(const SavedPool&) = delete;

    int count() const { return count_; }
    const T& operator[](int i) const { return values_[i]; }
    T& operator[](int i) { return values_[i]; }

    // False when the heap refused; what was already pushed is kept.
    bool push(const T& value);
    bool reserve(int n);

private:
    T* values_ = nullptr;
    int count_ = 0;
    int capacity_ = 0;
};

struct PersistentEntities {
    SavedPool<Painting> paintings;
    SavedPool<Arrow> arrows;
    SavedPool<Boat> boats;
    SavedPool<Minecart> minecarts;
    SavedPool<ItemEntity> items;
    SavedPool<FallingBlock> fallingBlocks;
    SavedPool<PrimedTnt> primedTnt;
    SavedPool<Mob> mobs;

    // False when the heap could not hold the copy. The caller keeps the
    // snapshot it had rather than writing a partial one.
    bool capture(const EntityPools& pools);

    // Anything a pool refuses -- because the heap would not let it grow --
    // is dropped and counted in that pool's `refused()`.
    void restore(const EntityPools& pools) const;
};

template <class T> SavedPool<T>::~SavedPool()
{
    std::free(values_);
}

template <class T> bool SavedPool<T>::reserve(int n)
{
    if (n <= capacity_) {
        return true;
    }
    // Checked before the multiply: a 32-bit size_t on the console, and a count
    // read out of a damaged file can be anything.
    if (n < 0 || usize(n) > usize(-1) / sizeof(T)) {
        return false;
    }
    void* grown = std::realloc(values_, sizeof(T) * usize(n));
    if (grown == nullptr) {
        return false;
    }
    values_ = static_cast<T*>(grown);
    capacity_ = n;
    return true;
}

template <class T> bool SavedPool<T>::push(const T& value)
{
    if (count_ == capacity_) {
        if (capacity_ > (1 << 29) || !reserve(capacity_ < 8 ? 8 : capacity_ * 2)) {
            return false;
        }
    }
    values_[count_++] = value;
    return true;
}

bool readPersistentEntities(nbt::Reader& reader, PersistentEntities* out);
void writePersistentEntities(nbt::Writer& writer, const PersistentEntities& state);
}  // namespace mc::entity
