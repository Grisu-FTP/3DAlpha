#pragma once

// **Nearest first, when there is more to draw than room to draw it.**
//
// The entity pools have no cap (core/util/segmented_pool.hpp), but each entity
// pass draws out of one vertex buffer taken once at init, and it has to: the
// GPU reads the buffer after the frame is submitted, so it cannot be grown or
// reused mid-frame, and linear memory is the chunk meshes' too. So what is
// bounded is what is *drawn*, not what exists.
//
// A builder used to fill its buffer in pool order and stop, which drops
// whichever entities happen to sit late in the pool -- and swap-removal
// reshuffles that order, so the missing ones changed from frame to frame. Now
// a pass that would overflow first sorts its entities into half-block rings
// round the eye and adds up what each ring costs. Every ring that fits is drawn
// whole; the first that does not -- the **edge** -- gets exactly the room that
// is left, in pool order; nothing past it is drawn. What a player sees past the
// budget is the far side of a crowd not drawn, which is what a shorter render
// distance looks like. A pile of drops all in one ring still draws as much of
// itself as fits rather than vanishing.
//
// **No allocation and no sort**: 128 counters on the stack and two passes, and
// a pass that already fits skips the first one entirely.

#include <cmath>

namespace mc::render {

// Half a block each, out to 64 blocks. The detail passes stop at 31.25 blocks
// and the signs at 64, so for them the last ring is never crowded. The passes
// in core/render/entity_range.hpp reach further (a spider to 79 blocks, a boat
// to 77), and everything past 63.5 shares the last ring -- which only means
// that of two far-off spiders over budget, the one kept is not always the
// nearer.
inline constexpr int kDrawRings = 128;
inline constexpr double kDrawRingsPerBlock = 2.0;

inline int drawRing(double dx, double dy, double dz)
{
    const double ring = std::sqrt(dx * dx + dy * dy + dz * dz) * kDrawRingsPerBlock;
    return ring >= double(kDrawRings - 1) ? kDrawRings - 1 : int(ring);
}

class DrawCutoff {
public:
    // A default cutoff draws everything; `compute` is what narrows it.
    bool computed() const { return computed_; }

    // Settled without counting, because the caller knows everything fits. A
    // cutoff shared between two passes has to be settled by the first, which
    // is the one handed the whole buffer.
    void drawAll()
    {
        computed_ = true;
        edgeRing_ = kDrawRings;
    }
    bool drawsEverything() const { return edgeRing_ >= kDrawRings; }

    // `place(i, &dx, &dy, &dz)` answers whether entity `i` would be drawn at
    // all and where it is relative to the origin; `cost(i)` how many vertices
    // it takes, at most. **The builder must pass `admit` the same position**,
    // or the two passes disagree about which ring an entity is in.
    template <class Place, class Cost>
    void compute(int count, int budget, Place place, Cost cost)
    {
        computed_ = true;
        edgeRing_ = kDrawRings;
        int ringCost[kDrawRings] = {};
        int total = 0;
        for (int i = 0; i < count; ++i) {
            double dx = 0.0;
            double dy = 0.0;
            double dz = 0.0;
            if (!place(i, &dx, &dy, &dz)) {
                continue;
            }
            const int c = cost(i);
            ringCost[drawRing(dx, dy, dz)] += c;
            total += c;
        }
        if (total <= budget) {
            return;
        }
        int nearer = 0;
        for (int ring = 0; ring < kDrawRings; ++ring) {
            if (nearer + ringCost[ring] > budget) {
                edgeRing_ = ring;
                edgeStart_ = budget - nearer;
                edgeLeft_ = edgeStart_;
                return;
            }
            nearer += ringCost[ring];
        }
    }

    // Whether an entity this far away and this dear is drawn. An entity in the
    // edge ring is charged against what the edge has left, so call this once
    // per entity and only for one that will be built.
    bool admit(double dx, double dy, double dz, int cost)
    {
        if (edgeRing_ >= kDrawRings) {
            return true;
        }
        const int ring = drawRing(dx, dy, dz);
        if (ring != edgeRing_) {
            return ring < edgeRing_;
        }
        if (cost > edgeLeft_) {
            return false;
        }
        edgeLeft_ -= cost;
        return true;
    }

    // Back to the edge's full allowance, so a second pass over the *same*
    // entities, charging the same costs, admits exactly the same ones. The sign
    // text uses it to draw the signs whose boards were drawn.
    void rewind() { edgeLeft_ = edgeStart_; }

private:
    int edgeRing_ = kDrawRings;
    int edgeStart_ = 0;
    int edgeLeft_ = 0;
    bool computed_ = false;
};

}  // namespace mc::render
