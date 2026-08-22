#pragma once

// The two kinds of memory the PICA can fetch vertices from, behind the pool's
// allocator seam.
//
// This is the whole of the platform-specific part of the VBO pool: linear is
// linearAlloc, and everything else about size classes, eviction and recycling
// is core code the host tests already cover.
//
// **The VRAM tier is switched off on hardware and the code below says why.**
// It stays in `core/render/vbo_pool` because the pool is tier-generic, the
// host tests exercise both tiers, and the day meshes are uploaded once instead
// of restreamed the trade changes. Nothing on the console reaches it.

#include "core/render/vbo_pool.hpp"

namespace mc::ctr {

class GpuVboAllocator : public render::VboAllocator {
public:
    void* allocate(usize bytes, render::VboTier tier) override;
    void release(void* pointer, usize bytes, render::VboTier tier) override;

    // A mesh is written by the CPU and read by the GPU, which does not see the
    // data cache. Linear memory therefore needs a flush; VRAM does not, and
    // citro3d's own C3D_TexFlush skips it there for the same reason.
    void flush(void* pointer, usize bytes, render::VboTier tier) override;
};

// How much of each tier to give the pool, decided against what is actually
// free rather than against a figure measured offline. The VRAM share is
// currently always zero, so this is the linear budget and a reason.
//
// `reserveVram` -- how much to leave for textures loaded later, so a texture
// pack swapped in mid-session does not find VRAM full of chunk meshes -- is
// kept and ignored. It is the one parameter a staged VRAM path would need
// back, and dropping it would only mean rediscovering it.
render::VboPool::Budget vboBudget(bool isNew3DS, usize reserveVram);

}  // namespace mc::ctr
