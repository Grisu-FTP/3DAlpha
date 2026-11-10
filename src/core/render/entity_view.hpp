#pragma once

// Whether the camera can see a thing an entity builder is about to pay for.
//
// a1.1.2 tests every entity against the view before drawing it (`e.class`,
// `oe.a(Lcf;)Z` on the entity's box). Here the test has a second job: each pass
// draws out of a fixed buffer, nearest first once it is over budget
// (draw_budget.hpp), so anything behind the camera that is still counted takes
// the room of something in front of it. The builders ask this before they ask
// the budget, so the budget is only ever spent on what can be seen.
//
// **A sphere, generous on purpose.** The original's box is the entity's collision
// box, and a model overhangs that: a zombie's arms, a spider's legs. Culling
// a little late costs one model built for nothing; culling a little early is
// a mob that pops out of existence at the edge of the screen. The radius each
// builder passes is measured from whatever point its `place` already computes.

#include "core/util/frustum.hpp"

namespace mc::render {

class EntityView {
public:
    // A default view sees everything, which is what the tests and the host
    // harness want.
    EntityView() = default;

    // `originX/Y/Z` is how far the builders' origin is from the origin the
    // frustum's planes are measured from -- the eye's block against the
    // camera's chunk, on every platform.
    EntityView(const Frustum& frustum, double originX, double originY, double originZ)
        : frustum_(&frustum),
          originX_(float(originX)),
          originY_(float(originY)),
          originZ_(float(originZ))
    {
    }

    // (x, y, z) is relative to the builders' origin, as `place` computes it.
    bool sees(double x, double y, double z, double radius) const
    {
        if (frustum_ == nullptr) {
            return true;
        }
        const float px = float(x) + originX_;
        const float py = float(y) + originY_;
        const float pz = float(z) + originZ_;
        for (int i = 0; i < Frustum::kPlaneCount; ++i) {
            if (frustum_->plane(i).distanceTo(px, py, pz) < -float(radius)) {
                return false;
            }
        }
        return true;
    }

private:
    const Frustum* frustum_ = nullptr;
    float originX_ = 0.0f;
    float originY_ = 0.0f;
    float originZ_ = 0.0f;
};

// The builders take an optional view, so one that was not given sees everything.
inline bool canSee(const EntityView* view, double x, double y, double z, double radius)
{
    return view == nullptr || view->sees(x, y, z, radius);
}

}  // namespace mc::render
