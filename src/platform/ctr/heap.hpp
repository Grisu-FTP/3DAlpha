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

// The whole newlib heap, as the split above chose it. Read by the game to size
// the streamer's column budget against the memory that actually exists rather
// than against a constant compiled in beside it -- the two consoles get very
// different figures, and so does a launch context that hands over less than
// either. See mc::render::WorldStreamer::setMemoryBudget.
usize heapTotalBytes();

// **What running out of heap looks like on this console, and why it needs
// saying out loud.**
//
// `operator new` here is libstdc++'s, built with exceptions on. Disassembled
// out of the shipped ELF rather than assumed:
//
//     _Znwj:  bl malloc ; subs r3, r0, #0 ; popne           @ the good path
//             bl _ZSt15get_new_handlerv ; cmp r0, #0
//             blxne r0 ; b <retry the malloc>               @ <-- the hook
//             bl __cxa_allocate_exception ; bl __cxa_throw  @ -> terminate
//
// Every frame in this binary is `-fno-exceptions`, so that throw has no landing
// pad to unwind to: it reaches `std::terminate`, which calls `abort`, which is
// `_exit`. **A 3DSX that exits drops cleanly to the HOME menu** -- no exception
// screen, no Luma dump, nothing written to the card, and nothing at all to tell
// it apart from the player having quit on purpose. It is the one failure this
// project can have that leaves no artifact, which is exactly why it was
// reported from hardware as "it rebooted to the HOME menu" and could not be
// chased any further than that.
//
// Installing a handler puts one report in the way of the exit. It cannot fix
// anything -- a handler that *returns* sends `operator new` round the retry
// loop above, so one that has freed nothing must not return -- and it does not
// try to: it says what ran out, on the bottom screen, which `consoleInit`
// leaves single-buffered and which therefore needs no GPU, and in a file on the
// card, which is the half that outlives the exit.
void installOutOfMemoryReporter();

// The figures that report prints. **Plain scalars written once a frame by the
// game loop**, because the allocation that fails may fail on the generation
// worker or the I/O thread, and a reporter that called back into the streamer
// would take the very locks whichever thread is already holding. A stale
// snapshot is worth having; a deadlock inside the crash handler is not.
struct MemorySnapshot {
    u32 blockKb = 0;    // columns resident, in the newlib heap
    u32 dirtyKb = 0;    // finished world owed to the card, which cannot be evicted

    // **The two that were missing, and between them they were the larger
    // half.** The first report this reporter produced read `blocks 10688k` of
    // `40960k` with `4405k` free -- so resident columns were 29 % of what was
    // in use and the line named none of the rest. The chunk cache's clean side
    // is capped at 8 MB and the generator's own cache is `16 * loadRadius + 96`
    // columns, which is another 8 MB at render distance 12; together they are
    // bigger than the grid the page was blaming.
    u32 cleanKb = 0;    // chunk cache, clean side, capped by cleanCapBytes
    u32 genLive = 0;    // generator cache high-water, in columns

    u32 poolKb = 0;     // mesh pool resident, in linear memory
    u32 columns = 0;    // columns the grid holds
    u32 sections = 0;   // sections the pool holds
    i32 chunkX = 0;     // where the player was, because the Far Lands are the case
    i32 chunkZ = 0;
    u8 quadFormat = 0;  // 1 while the geometry-shader cube format is live
};
void setMemorySnapshot(const MemorySnapshot& snapshot);

}  // namespace mc::ctr
