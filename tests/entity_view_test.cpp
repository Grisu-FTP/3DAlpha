// Asking the camera before an entity pass spends its budget. See
// core/render/entity_view.hpp.

#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "core/entity/mob.hpp"
#include "core/render/entity_view.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/util/frustum.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using entity::MobSystem;
using entity::MobType;
using render::EntityView;
using test::SceneWorld;

namespace {

constexpr double kGround = 64.0;

// A plain frustum down +Z from (0, eyeY, 0), built from its planes so this test
// does not depend on any platform's projection convention. Sixty degrees to
// each side, and far enough to hold everything here.
Frustum lookingNorth(float eyeY)
{
    const float s = std::sqrt(0.75f);
    const float c = 0.5f;
    Frustum f;
    f.setPlane(Frustum::kLeft, Plane{c, 0.0f, s * 0.5f, 0.0f});
    f.setPlane(Frustum::kRight, Plane{-c, 0.0f, s * 0.5f, 0.0f});
    f.setPlane(Frustum::kBottom, Plane{0.0f, c, s * 0.5f, -c * eyeY});
    f.setPlane(Frustum::kTop, Plane{0.0f, -c, s * 0.5f, c * eyeY});
    f.setPlane(Frustum::kNear, Plane{0.0f, 0.0f, 1.0f, -0.05f});
    f.setPlane(Frustum::kFar, Plane{0.0f, 0.0f, -1.0f, 500.0f});
    return f;
}

struct Pen {
    test::DropCatcher drops;
    SceneWorld scene{0, 0};
    MobSystem mobs{31337};

    Pen()
    {
        for (i32 x = -30; x <= 30; ++x) {
            for (i32 z = -30; z <= 30; ++z) {
                scene.place(x, 63, z, block::BlockId(mcver::Block::Grass), 0);
            }
        }
        scene.lightColumnsFrom(-30, 30, -30, 30, 64);
        drops.watch(scene.w());
    }

    void add(double x, double z)
    {
        CHECK(mobs.spawn(scene.w(), MobType::Zombie, x, kGround, z, 0.0f));
    }

    int build(const EntityView* view, std::vector<mesh::DetailVertex>* out)
    {
        out->resize(render::kMobMaxVertices);
        return render::buildMobs(mobs, 0.0, kGround, 0.0, 0.0f, out->data(),
                                 render::kMobMaxVertices, view);
    }
};

}  // namespace

TEST(a_default_view_sees_everything)
{
    const EntityView all;
    CHECK(all.sees(0.0, 0.0, -1000.0, 0.0));
    CHECK(render::canSee(nullptr, 1e6, 1e6, 1e6, 0.0));
}

TEST(a_view_sees_what_is_in_front_and_not_what_is_behind)
{
    const Frustum frustum = lookingNorth(0.0f);
    const EntityView view(frustum, 0.0, 0.0, 0.0);
    CHECK(view.sees(0.0, 0.0, 10.0, 0.5));
    CHECK(!view.sees(0.0, 0.0, -10.0, 0.5));
    CHECK(!view.sees(40.0, 0.0, 10.0, 0.5));  // well off to one side
}

// The radius is what stops a model popping out at the edge of the screen.
TEST(something_straddling_the_edge_of_the_view_is_kept)
{
    const Frustum frustum = lookingNorth(0.0f);
    const EntityView view(frustum, 0.0, 0.0, 0.0);
    CHECK(!view.sees(0.0, 0.0, -2.0, 1.0));
    CHECK(view.sees(0.0, 0.0, -2.0, 3.0));
}

// The builders' origin is not the frustum's: the view has to add the difference.
TEST(a_view_measures_from_the_builders_origin)
{
    const Frustum frustum = lookingNorth(70.0f);
    const EntityView view(frustum, 0.0, 70.0, 0.0);
    CHECK(view.sees(0.0, 0.0, 10.0, 0.5));
    CHECK(!view.sees(0.0, -60.0, 10.0, 0.5));  // sixty below the eye, ten ahead
}

// What this exists for: a crowd behind the camera used to take the whole
// budget, nearest first, and hide the zombies in front of it.
TEST(a_crowd_behind_the_camera_does_not_use_up_the_budget)
{
    Pen pen;
    std::vector<mesh::DetailVertex> out;

    pen.add(0.5, 20.5);
    const int oneMob = pen.build(nullptr, &out);
    CHECK(oneMob > 0);
    pen.add(3.5, 20.5);
    pen.add(-3.5, 20.5);

    // More than the buffer holds -- the budget is in vertices, and a zombie
    // takes fewer than the worst model it is sized for -- and every one of them
    // nearer than the three ahead.
    const int crowd = render::kMobMaxVertices / oneMob + 4;
    for (int i = 0; i < crowd; ++i) {
        pen.add(-6.5 + double(i % 7) * 2.0, -8.5 - double(i / 7));
    }

    // Without a view the nearest fill the buffer and the three are lost.
    const int blind = pen.build(nullptr, &out);
    CHECK(blind > render::kMobMaxVertices - oneMob);
    bool anyAhead = false;
    for (int v = 0; v < blind; ++v) {
        anyAhead = anyAhead || out[usize(v)].z > 0;
    }
    CHECK(!anyAhead);

    const Frustum frustum = lookingNorth(float(kGround) + 1.0f);
    const EntityView view(frustum, 0.0, kGround, 0.0);
    CHECK_EQ(pen.build(&view, &out), 3 * oneMob);
}
