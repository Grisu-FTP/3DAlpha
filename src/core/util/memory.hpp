#pragma once

// How much heap is left, asked of the platform.
//
// A seam for the same reason core/util/worker.hpp is one: there is no portable
// answer. On the console the newlib heap is a fixed arena carved out at startup
// by platform/ctr/heap.cpp, so "free" is a real number with a real ceiling and
// the thing that runs out is block data, not address space. On a host it is
// whatever the allocator feels like saying, which is why nothing here has to be
// set -- unset means "no answer", and every caller has to have a policy for
// that anyway.
//
// **It is a hint, not an allocator.** The number is read to size a cache, never
// to decide whether an allocation will succeed: it can be stale by the time it
// is used and it says nothing about fragmentation. A caller that would break if
// it were wrong is using it for the wrong thing.
//
// Called from whichever thread wants it -- the chunk cache asks on its
// generation worker -- so an implementation must be safe to call concurrently.

#include "core/util/types.hpp"

namespace mc {

// Bytes still available on the heap that ordinary allocations come from. Zero
// means "no answer", which is what a host reports.
using HeapFreeQuery = usize (*)();

// Process-wide, set once by the platform before any world is opened.
void setHeapFreeQuery(HeapFreeQuery query);

// The platform's answer, or 0 if none was set.
usize heapFreeBytes();

}  // namespace mc
