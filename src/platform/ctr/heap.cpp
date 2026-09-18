// Heap policy.
//
// libctru's default split caps the linear heap at 32 MB and gives everything
// else to the newlib heap -- measured on a New 3DS as 91 MB heap / 32 MB linear.
// That is backwards for a voxel renderer: chunk meshes must live in linear
// memory (the GPU cannot fetch from the newlib heap), while block data, code and
// scratch live on the newlib heap and need far less than 91 MB.
//
// __system_allocateHeaps is weak, so we replace it wholesale. Overriding the
// function rather than just __ctru_{heap,linear_heap}_size is deliberate: the
// sizes alone are fixed constants, and libctru panics if they exceed what is
// actually available, which differs between Old and New 3DS and between launch
// methods. Computing the split from the real figure is the safe form.
//
// This runs before services are up, so the model cannot be queried -- the policy
// derives from the available memory itself. See docs/3ds-performance.md.

#include "platform/ctr/heap.hpp"
#include "platform/ctr/bottom_screen.hpp"

#include <3ds.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;

extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;

void __system_allocateHeaps(void);
}

namespace {

// **The split is chosen from what linear memory actually needs, not from a
// fraction.** It used to be `remaining / 3` capped at 40 MB, on the reasoning
// that "at render distance 12 the block data is ~15 MB, so 40 MB is
// comfortable". Two things were wrong with that.
//
// The first is that the figure was for ordinary terrain. Measured through the
// real generator, a resident column costs **14.0 KB over ordinary ground and
// 21.7 KB at the Far Lands** -- 1.55x, because the Far Lands has far fewer of
// the *uniform* sections that cost nothing, not because the palette fails.
// (Two obvious savings were measured and both are dead: `compact()` recovers
// 0.0%, since the generator's `assign()` already collapses, and a 1-/2-bit
// palette tier recovers 2%, since sections hold 5-8 distinct ids, not 4.) So
// the grid costs:
//
//     distance   columns   ordinary   far lands
//         12       729      10.0 MB     15.5 MB
//         16      1089      14.9 MB     23.1 MB
//         20      1681      23.0 MB     35.6 MB
//         24      2401      32.8 MB     50.9 MB   <- the debug page's ceiling
//
// against 40 MB that also holds code, stacks, the generator's own column cache,
// the dirty set and the map. The Far Lands at a large render distance cannot
// fit, and that is a `std::bad_alloc` into `abort()` -- see heap.hpp.
//
// The second is that nothing was on the other side of the trade. **The linear
// heap was handed ~83 MB and the VBO pool caps itself at 32** (`vboBudget` in
// gpu_memory.cpp, which says so in its own comment). Roughly 38 MB sat idle
// while the heap next to it ran out. So linear is now given what it asks for
// plus a margin, and the heap gets the rest.
//
// The model cannot be queried here -- this runs before services are up -- so
// the pool's own two figures are keyed off the available memory itself, which
// is what distinguishes the consoles anyway.
// **Keyed off available memory because the model cannot be asked for yet**, and
// the two real configurations sit far apart -- a New 3DS reports ~123 MB here
// and an Old one well under half that -- so the floor between them is not a
// close call. Getting it wrong costs a *smaller VBO pool*, never a panic:
// `vboBudget` decides the pool from the real `isNew3DS`, and if it finds less
// linear than it wanted it clamps to what is there. Reserving the larger figure
// unconditionally is what would be unsafe, since on an Old 3DS it would starve
// the heap to its floor to hold 20 MB of linear that nothing would ever ask for.
constexpr u32 kPoolBudgetNew = 32u << 20;  // vboBudget's New 3DS total
constexpr u32 kPoolBudgetOld = 12u << 20;  // and its Old 3DS one
constexpr u32 kNewConsoleFloor = 96u << 20;

// Everything in linear that is not the pool: the 1 MB GPU command buffer, the
// shared index buffer, the block atlas when VRAM refuses it, the audio ring,
// the tiled staging textures -- and `vboBudget`'s own 8 MB refusal margin,
// which is why this is not smaller.
constexpr u32 kLinearOverhead = 16u << 20;

constexpr u32 kHeapMin = 16u << 20;
constexpr u32 kHeapMax = 80u << 20;

constexpr u32 kPageMask = ~0xFFFu;
constexpr u32 kLinearRetryStep = 4u << 20;
constexpr u32 kLinearMin = 8u << 20;

u32 availableMemory()
{
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
        svcBreak(USERBREAK_PANIC);
    }

    s64 maxCommit = 0;
    s64 currentCommit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
    svcCloseHandle(reslimit);

    return static_cast<u32>(maxCommit - currentCommit) & kPageMask;
}

// What linear has to keep for itself. See the note above.
u32 linearReserve(u32 remaining)
{
    const u32 pool = remaining >= kNewConsoleFloor ? kPoolBudgetNew : kPoolBudgetOld;
    return pool + kLinearOverhead;
}

u32 chooseHeapSize(u32 remaining)
{
    const u32 reserve = linearReserve(remaining);
    u32 heap = remaining > reserve ? remaining - reserve : 0;
    if (heap > kHeapMax) {
        heap = kHeapMax;
    }
    if (heap < kHeapMin) {
        heap = kHeapMin;
    }
    // Pathologically small region (unexpected launch context): fall back to an
    // even split rather than asking for more than exists. **Reached by the
    // kHeapMin clamp above and by nothing else**, since the subtraction cannot
    // exceed `remaining` on its own -- which is the property that makes raising
    // kHeapMax safe. A console with less memory than the clamp gets the same
    // even split it always did rather than a boot that panics.
    if (heap >= remaining) {
        heap = remaining / 2;
    }
    return heap & kPageMask;
}

}  // namespace

extern "C" void __system_allocateHeaps(void)
{
    const u32 remaining = availableMemory();
    const u32 heap = chooseHeapSize(remaining);

    constexpr MemPerm kRw = static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE);

    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, heap, MEMOP_ALLOC, kRw))) {
        svcBreak(USERBREAK_PANIC);
    }
    __ctru_heap_size = heap;

    // Claim the rest as linear. If the linear address space cannot satisfy the
    // full amount, step down rather than panicking -- a smaller mesh pool is a
    // degraded game, a panic is no game at all.
    u32 linear = (remaining - heap) & kPageMask;
    while (linear >= kLinearMin) {
        if (R_SUCCEEDED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, linear, MEMOP_ALLOC_LINEAR,
                                         kRw))) {
            break;
        }
        linear = (linear > kLinearRetryStep) ? (linear - kLinearRetryStep) : 0;
        linear &= kPageMask;
    }
    if (linear < kLinearMin) {
        svcBreak(USERBREAK_PANIC);
    }
    __ctru_linear_heap_size = linear;

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

    fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
    fake_heap_end = fake_heap_start + heap;
}

namespace mc::ctr {

// **What malloc has not handed out**, which is not the same as what a single
// allocation could get -- fragmentation is invisible from here. Everything that
// reads it treats it as a hint and halves it before spending it; see
// ChunkCache::dirtyCapLocked.
//
// `uordblks` rather than `fordblks`: the free figure only counts what has been
// sbrk'd and released, so a heap that has never been filled reports almost
// nothing free when in fact all of it is. Subtracting what is in use from the
// arena the split fixed at startup is the number that means what it says.
//
// Safe from any thread: newlib takes the malloc lock inside mallinfo, and the
// chunk cache asks on its generation worker.
usize heapTotalBytes()
{
    return usize(__ctru_heap_size);
}

usize heapFreeBytes()
{
    const struct mallinfo info = mallinfo();
    const u32 used = static_cast<u32>(info.uordblks);
    return __ctru_heap_size > used ? usize(__ctru_heap_size - used) : 0;
}

namespace {

// **Not behind a mutex, and that is deliberate.** Taking a lock in a new
// handler is taking a lock at the one moment the process is least able to
// afford it, and the writer is the game loop while the readers are whichever
// thread happened to be allocating. Word-sized stores of a POD are what this
// costs, and the worst a torn read produces is one figure from the frame
// before -- which is still the right order of magnitude, which is all any of
// these are ever read as.
MemorySnapshot g_snapshot;

// The line, formatted into static storage because the heap is what has just
// run out and a stack buffer would be gone before anything read it.
char g_oomLine[256];

// **A file, because the screen does not survive the exit.** Raw `open`/`write`
// rather than `std::fopen`, which allocates a FILE and a buffer -- exactly what
// is not available. Failure is ignored: there is no card, or no room on it, and
// the bottom screen still carries the same text either way.
void writeReportToCard(const char* line, usize length)
{
    const int fd = open("sdmc:/3dalpha-oom.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    // One short write, unchecked for the same reason: there is nothing this
    // could do about a partial one that it is not already doing.
    (void)write(fd, line, length);
    (void)write(fd, "\n", 1);
    close(fd);
}

// **Must not return.** `operator new` calls this and then retries the malloc
// (see the disassembly in heap.hpp), so a handler that has freed nothing and
// returns anyway spins forever printing the same line. This one has freed
// nothing on purpose -- an emergency reserve handed back here would let the
// game limp on in a state nobody has reasoned about -- so it ends the process
// itself, which is what would have happened anyway, only now with a cause
// attached.
void reportOutOfMemory()
{
    const MemorySnapshot snapshot = g_snapshot;

    std::snprintf(g_oomLine, sizeof(g_oomLine),
                  "OUT OF HEAP  free %luk of %luk  blocks %luk  owed %luk  clean %luk  "
                  "gen %lu  pool %luk  cols %lu  sect %lu  chunk %ld %ld  %s",
                  static_cast<unsigned long>(heapFreeBytes() / 1024),
                  static_cast<unsigned long>(__ctru_heap_size / 1024),
                  static_cast<unsigned long>(snapshot.blockKb),
                  static_cast<unsigned long>(snapshot.dirtyKb),
                  static_cast<unsigned long>(snapshot.cleanKb),
                  static_cast<unsigned long>(snapshot.genLive),
                  static_cast<unsigned long>(snapshot.poolKb),
                  static_cast<unsigned long>(snapshot.columns),
                  static_cast<unsigned long>(snapshot.sections),
                  static_cast<long>(snapshot.chunkX), static_cast<long>(snapshot.chunkZ),
                  snapshot.quadFormat != 0 ? "geoshader" : "4-vertex");

    // Rows 28-31 of the bottom screen, wrapping past the overlay's footer,
    // and flushed the way geoTrace flushes: `bottom::flush` copies it across
    // synchronously, so it is on the glass before the next instruction runs.
    std::printf("\x1b[28;1H\x1b[2K\x1b[31m%s\x1b[0m", g_oomLine);
    bottom::flush();

    writeReportToCard(g_oomLine, std::strlen(g_oomLine));

    // Give the glass a moment to be photographed. Two seconds is the whole
    // difference between a player who can report a number and one who can only
    // report that it happened.
    svcSleepThread(2000000000ull);
    std::abort();
}

}  // namespace

void setMemorySnapshot(const MemorySnapshot& snapshot)
{
    g_snapshot = snapshot;
}

void installOutOfMemoryReporter()
{
    std::set_new_handler(&reportOutOfMemory);
}

}  // namespace mc::ctr
