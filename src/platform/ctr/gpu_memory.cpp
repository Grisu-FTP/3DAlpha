#include "platform/ctr/gpu_memory.hpp"

#include <3ds.h>

namespace mc::ctr {

void* GpuVboAllocator::allocate(usize bytes, render::VboTier tier)
{
    // Null is not a failure the pool has to be protected from: it treats the
    // tier as full and falls through to the next one, which is exactly what a
    // console that has lost VRAM to fragmentation should do.
    //
    // VRAM always answers null, and that is the fix for a hardware crash
    // rather than a fragmentation policy -- see vboBudget below. The refusal
    // is here as well as in the budget because the two guard different
    // mistakes: the budget stops the pool asking, this stops it being served.
    return tier == render::VboTier::Vram ? nullptr : linearAlloc(bytes);
}

void GpuVboAllocator::release(void* pointer, usize, render::VboTier tier)
{
    if (tier == render::VboTier::Vram) {
        vramFree(pointer);
    } else {
        linearFree(pointer);
    }
}

render::VboPool::Budget vboBudget(bool isNew3DS, usize /*reserveVram*/)
{
    constexpr usize kMb = 1024 * 1024;

    // The totals docs/architecture.md budgets: 12 MB on an old 3DS, 32 on a
    // New one. Both were measured to turn on the spot without evicting, at
    // render distance 6 and 10 respectively.
    const usize total = (isNew3DS ? 32 : 12) * kMb;

    render::VboPool::Budget budget;

    // **No VBOs in VRAM.** The CPU cannot write it: a plain store into a
    // vramAlloc'd pointer takes a permission fault, which is how this arrived
    // -- a memcpy of a freshly meshed section died on hardware at 0x1F38CA00.
    // citro3d says the same thing in code: C3D_TexLoadImage range-checks its
    // destination against [0x1F000000, +0x600000) and routes VRAM through
    // C3D_SyncTextureCopy instead of memcpy. Atlas::init already obeys the
    // rule; the pool did not, because its allocator seam covered allocate,
    // release and flush but not the write itself.
    //
    // So a VRAM VBO is not "free performance" as docs/3ds-performance.md §3
    // assumed. It costs a second copy into a linear staging buffer, a cache
    // flush and a GPU round trip, per upload, on a path that re-meshes up to
    // 58 sections in a frame while the player turns. That trade was never
    // measured -- the doc's own table lists VBOs-in-VRAM as "measure against
    // linear-only" -- so the unmeasured optimisation is the part that goes.
    budget.vram = 0;

    // Whatever the linear heap can actually stand to lose. The heap policy hands
    // us 82 MB of it, so this is normally the full remainder, but asking for
    // more than exists would have the pool discover it one failed allocation at
    // a time in the middle of a frame.
    const usize freeLinear = linearSpaceFree();
    const usize spare = freeLinear > 8 * kMb ? freeLinear - 8 * kMb : 0;
    budget.linear = total < spare ? total : spare;

    return budget;
}

void GpuVboAllocator::flush(void* pointer, usize bytes, render::VboTier tier)
{
    if (tier == render::VboTier::Linear) {
        GSPGPU_FlushDataCache(pointer, u32(bytes));
    }
}

}  // namespace mc::ctr
