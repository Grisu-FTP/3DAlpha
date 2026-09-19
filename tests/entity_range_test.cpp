// `kh.a(D)Z` against the boxes the jar gives each entity, and the reach of the
// units the far passes write. See core/render/entity_range.hpp.

#include "core/render/entity_range.hpp"
#include "framework.hpp"

#include <cmath>

using namespace mc;

namespace {

double range(double width, double height)
{
    return std::sqrt(render::entityDrawDistanceSq(width, height));
}

bool near(double a, double b) { return std::abs(a - b) < 0.05; }

}  // namespace

TEST(each_entity_is_drawn_as_far_as_its_box_says)
{
    // The `setSize` arguments off each class file, times 64 on the average edge.
    CHECK(near(range(0.6, 1.8), 64.0));     // zombie, skeleton, creeper
    CHECK(near(range(1.4, 0.9), 78.9));     // spider
    CHECK(near(range(0.3, 0.4), 21.3));     // chicken
    CHECK(near(range(1.5, 0.6), 76.8));     // boat, `dc`
    CHECK(near(range(0.98, 0.7), 56.7));    // minecart, `oc`
    CHECK(near(range(0.98, 0.98), 62.7));   // falling block `ff`, TNT `jd`
    CHECK(near(range(0.25, 0.25), 16.0));   // dropped item, `dx`
}

TEST(the_far_units_hold_every_admitted_model)
{
    // Everything but a size-4 slime fits inside the placement limit with its
    // model's reach to spare, so nothing admitted can wrap the short.
    CHECK(render::kEntityPlacementLimit > range(1.4, 0.9));
    CHECK(render::kEntityPlacementLimit + render::kEntityModelReach
          <= 32767.0 / double(render::kEntityUnitsPerBlock));
    CHECK(render::entityInDrawRange(60.0, 0.0, 0.0, 1.5, 0.6));
    CHECK(!render::entityInDrawRange(0.0, 0.0, 77.0, 1.5, 0.6));
    // A huge box is still stopped by the short, not only by its range.
    CHECK(!render::entityInDrawRange(122.0, 0.0, 0.0, 2.4, 2.4));
}
