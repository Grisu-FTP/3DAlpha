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

#include <3ds.h>

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

// Newlib heap holds block data (palette sections), code, stacks and scratch.
// At render distance 12 the block data is ~15 MB, so 40 MB is comfortable;
// everything beyond that is better spent on meshes.
constexpr u32 kHeapTargetDivisor = 3;
constexpr u32 kHeapMin = 16u << 20;
constexpr u32 kHeapMax = 40u << 20;

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

u32 chooseHeapSize(u32 remaining)
{
    u32 heap = remaining / kHeapTargetDivisor;
    if (heap > kHeapMax) {
        heap = kHeapMax;
    }
    if (heap < kHeapMin) {
        heap = kHeapMin;
    }
    // Pathologically small region (unexpected launch context): fall back to an
    // even split rather than asking for more than exists.
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
