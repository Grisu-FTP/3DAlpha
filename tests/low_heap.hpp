#pragma once

// A heap as the console reports one that is nearly full, for as long as this
// object lives: every growable pool keeps what it was built with and is refused
// the next segment. RAII because a failed CHECK returns from the test, and the
// query is process-wide.

#include "core/util/memory.hpp"

namespace mc::test {

inline usize nearlyFullHeap()
{
    return 1;  // one byte: an answer, and far below any pool's reserve
}

struct LowHeap {
    LowHeap() { setHeapFreeQuery(&nearlyFullHeap); }
    ~LowHeap() { setHeapFreeQuery(nullptr); }
    LowHeap(const LowHeap&) = delete;
    LowHeap& operator=(const LowHeap&) = delete;
};

}  // namespace mc::test
