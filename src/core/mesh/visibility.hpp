#pragma once

// Per-section "can you see from face A through to face B" masks.
//
// This is Tommaso Checchi's visibility graph, the thing that makes underground
// rendering cheap. The PICA has no occlusion queries, so visibility is entirely
// a CPU problem, and the naive answer -- frustum-cull every section in range --
// draws the whole world through a cave wall.
//
// At mesh time each section's connected air volumes are flood-filled, and for
// every volume touching two of the six faces the corresponding pair bit is set.
// At render time a BFS walks outward from the camera's section and only steps
// through a section when the face it entered by connects to the face it wants
// to leave by. A solid section has mask 0 and stops the search dead.
//
// Fifteen unordered pairs of six faces fit a u16, so the whole answer for a
// section is two bytes.

#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"
#include "core/world/section.hpp"

namespace mc::mesh {

// Bit position for the unordered pair (a, b), a != b.
constexpr int visibilityBit(int a, int b)
{
    const int lo = a < b ? a : b;
    const int hi = a < b ? b : a;
    // Triangular numbering: pairs starting at face 0 come first, then face 1,
    // and so on. lo * (11 - lo) / 2 is where each run begins.
    return lo * (11 - lo) / 2 + (hi - lo - 1);
}

inline constexpr int kVisibilityPairCount = 15;

static_assert(visibilityBit(0, 1) == 0, "");
static_assert(visibilityBit(4, 5) == kVisibilityPairCount - 1, "");

// Every face reachable from every other: an entirely open section.
inline constexpr u16 kVisibilityAll = (1u << kVisibilityPairCount) - 1;

class SectionVisibility {
public:
    SectionVisibility() = default;
    explicit SectionVisibility(u16 mask) : mask_(mask) {}

    u16 mask() const { return mask_; }

    bool connects(int fromFace, int toFace) const
    {
        // Leaving by the face you entered is not a step the search ever makes,
        // and answering "yes" would let it walk back and forth forever.
        if (fromFace == toFace) {
            return false;
        }
        return (mask_ & (1u << visibilityBit(fromFace, toFace))) != 0;
    }

    bool opaqueToEverything() const { return mask_ == 0; }

private:
    u16 mask_ = 0;
};

// Working memory for the flood fill: 12 KB, too much for a stack frame on a
// 3DS thread, so the caller owns it and reuses it exactly like MeshScratch.
struct VisibilityScratch {
    u8 visited[world::Section::kVolume];
    u16 stack[world::Section::kVolume];
};

// A cell is passable when it is not an opaque cube -- deliberately the same
// predicate the mesher culls faces with. If the two ever disagreed, the search
// would either walk through a wall it just drew or stop at one it did not.
SectionVisibility computeVisibility(const world::Section& section, VisibilityScratch& scratch);

}  // namespace mc::mesh
