#include "core/util/memory.hpp"

#include <atomic>

namespace mc {
namespace {

// Read from the generation worker and written once at startup, before any
// thread that reads it exists. A plain pointer is enough for that ordering, and
// the alternative -- an atomic -- would suggest it may be changed while the
// game is running, which it may not.
HeapFreeQuery gQuery = nullptr;

std::atomic<usize> gPoolBytes{0};

}  // namespace

bool poolGrowthAllowed(usize bytes)
{
    const usize free = heapFreeBytes();
    if (free == 0) {
        return true;  // no answer from the platform
    }
    // Written so nothing can wrap: `free - bytes` only once `free` is known to
    // cover it, and the multiplied side divided rather than multiplied.
    if (free <= bytes + kPoolHeapReserve) {
        return false;
    }
    const usize after = free - bytes - kPoolHeapReserve;
    return (poolBytes() + bytes) <= after / kPoolSaveCopies;
}

void poolBytesTaken(usize bytes)
{
    gPoolBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void poolBytesReleased(usize bytes)
{
    gPoolBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

usize poolBytes()
{
    return gPoolBytes.load(std::memory_order_relaxed);
}

void setHeapFreeQuery(HeapFreeQuery query)
{
    gQuery = query;
}

usize heapFreeBytes()
{
    return gQuery != nullptr ? gQuery() : 0;
}

}  // namespace mc
