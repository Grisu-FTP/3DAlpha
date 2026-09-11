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

// **Growable pools ask here before they take another segment** -- see
// core/util/segmented_pool.hpp. Entities have no count limit (a1.1.2 has none),
// so what stops them is the heap, and it has to stop them *before* the heap is
// gone: on the console an `operator new` that fails ends the process, and the
// chunk cache allocates with it.
//
// So a pool may grow only while the heap would still hold, after the growth,
// a fixed reserve for everything else **plus three more copies of every pool
// byte** -- a save's snapshot, the NBT it is encoded into and the compressed
// file -- so the save that follows a spike can always be written. This is the
// hint being used to size something, which is what it is for; a pool never
// trusts it alone and still checks `malloc` for null.
//
// With no answer from the platform (a host), growth is always allowed and
// `malloc` returning null is the only refusal.
bool poolGrowthAllowed(usize bytes);

// Every pool segment taken and handed back, process-wide. Atomic because a
// save's snapshot is released on the I/O worker.
void poolBytesTaken(usize bytes);
void poolBytesReleased(usize bytes);
usize poolBytes();

// The reserve and the multiplier above, exposed for the tests.
inline constexpr usize kPoolHeapReserve = 8u << 20;
inline constexpr usize kPoolSaveCopies = 3;

}  // namespace mc
