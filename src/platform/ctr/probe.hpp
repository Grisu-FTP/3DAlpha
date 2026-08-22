#pragma once

// The M0 hardware probe, reachable by holding SELECT at boot.
//
// It measures rather than renders: heap and linear split, free VRAM after the
// render targets, SD cluster size, fill rate, and the A/B cost of the lightmap's
// second texture fetch. Those numbers are what docs/3ds-performance.md and the
// memory budget rest on, so they have to stay re-derivable on a console that is
// running the current build rather than an M0 tag.
//
// gfx and the bottom-screen console must already be up.

namespace mc::ctr {

int runProbe(bool isNew3DS);

}  // namespace mc::ctr
