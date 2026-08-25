#include "core/util/memory.hpp"

namespace mc {
namespace {

// Read from the generation worker and written once at startup, before any
// thread that reads it exists. A plain pointer is enough for that ordering, and
// the alternative -- an atomic -- would suggest it may be changed while the
// game is running, which it may not.
HeapFreeQuery gQuery = nullptr;

}  // namespace

void setHeapFreeQuery(HeapFreeQuery query)
{
    gQuery = query;
}

usize heapFreeBytes()
{
    return gQuery != nullptr ? gQuery() : 0;
}

}  // namespace mc
