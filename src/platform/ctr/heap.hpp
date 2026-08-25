#pragma once

// What is left of the heap this file's .cpp carved out at startup.
//
// Its own header because heap.cpp is otherwise all weak-symbol overrides that
// nothing calls by name, and this is the one thing in it a caller wants.

#include "core/util/types.hpp"

namespace mc::ctr {

// Bytes still available on the newlib heap: what the split gave it, less what
// malloc has handed out. **A hint** -- see core/util/memory.hpp -- and this is
// the query the game installs there.
usize heapFreeBytes();

}  // namespace mc::ctr
