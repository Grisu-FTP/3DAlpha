// What the crosshair is on, against a real a1.1.2 World.
//
// The fixture built a floor with one of everything awkward standing on it and
// swept several thousand rays across it from six eye positions; this rebuilds
// the same world block for block and casts the same rays. Positions are
// compared as **raw bit patterns**, because a hit point half a millimetre out
// is a face reported wrong at a grazing angle.
//
// The comparison covers the three things the caller uses: which block, which
// face, and where. Getting the block right and the face wrong would place
// against the wrong side, which is the bug this is here to prevent.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/entity/ray_trace.hpp"
#include "framework.hpp"
#include "ray_trace_vectors.hpp"
#include "scene_world.hpp"

#include <cstdio>
#include <cstring>

using namespace mc;
using mc::block::BlockId;
using mc::entity::RayHit;
using mc::entity::rayTrace;
using mc::test::SceneWorld;

namespace {

double asDouble(u64 bits)
{
    double out = 0.0;
    std::memcpy(&out, &bits, sizeof out);
    return out;
}

u64 bitsOf(double v)
{
    u64 out = 0;
    std::memcpy(&out, &v, sizeof out);
    return out;
}

void buildScene(SceneWorld& scene)
{
    for (int i = 0; i < test::kRayScenePlacementCount; ++i) {
        const test::RayScenePlacement& p = test::kRayScene[i];
        scene.place(test::kRayOriginX + p.dx, test::kRayOriginY + p.dy,
                    test::kRayOriginZ + p.dz, BlockId(p.id), p.metadata);
    }
}

}  // namespace

TEST(the_ray_trace_agrees_with_a_real_world_on_every_cast)
{
    SceneWorld scene(test::kRayOriginX >> 4, test::kRayOriginZ >> 4);
    buildScene(scene);

    for (int i = 0; i < test::kRayCaseCount; ++i) {
        const test::RayCase& c = test::kRayCases[i];
        const RayHit got = rayTrace(scene.w(), asDouble(c.eyeX), asDouble(c.eyeY),
                                    asDouble(c.eyeZ), asDouble(c.dirX), asDouble(c.dirY),
                                    asDouble(c.dirZ));

        char message[400];
        if (got.hit != c.hit) {
            std::snprintf(message, sizeof message,
                          "ray %d from (%.4f %.4f %.4f) dir (%.4f %.4f %.4f): %s, expected %s",
                          i, asDouble(c.eyeX), asDouble(c.eyeY), asDouble(c.eyeZ),
                          asDouble(c.dirX), asDouble(c.dirY), asDouble(c.dirZ),
                          got.hit ? "hit" : "missed", c.hit ? "a hit" : "a miss");
            test::reportFailure(__FILE__, __LINE__, message);
            return;
        }
        if (!c.hit) {
            continue;
        }
        if (got.x != c.blockX || got.y != c.blockY || got.z != c.blockZ) {
            std::snprintf(message, sizeof message,
                          "ray %d: hit block (%d %d %d), expected (%d %d %d)",
                          i, got.x, got.y, got.z, c.blockX, c.blockY, c.blockZ);
            test::reportFailure(__FILE__, __LINE__, message);
            return;
        }
        if (int(got.face) != c.side) {
            std::snprintf(message, sizeof message,
                          "ray %d: hit block (%d %d %d) on face %d, expected face %d",
                          i, got.x, got.y, got.z, int(got.face), c.side);
            test::reportFailure(__FILE__, __LINE__, message);
            return;
        }
        if (bitsOf(got.hitX) != c.hitX || bitsOf(got.hitY) != c.hitY
            || bitsOf(got.hitZ) != c.hitZ) {
            std::snprintf(message, sizeof message,
                          "ray %d: hit point (%.17g %.17g %.17g), expected (%.17g %.17g %.17g)",
                          i, got.hitX, got.hitY, got.hitZ, asDouble(c.hitX), asDouble(c.hitY),
                          asDouble(c.hitZ));
            test::reportFailure(__FILE__, __LINE__, message);
            return;
        }
    }
}

TEST(the_ray_fixture_hits_enough_different_things_to_mean_something)
{
    // A sweep that only ever hit the floor would agree with a ray trace that
    // could not see anything else. These counts are what stop that.
    CHECK(test::kRayHitCount > 200);
    CHECK(test::kRayCaseCount - test::kRayHitCount > 200);  // and misses too
    CHECK(test::kRayDistinctFaces >= 5);
    CHECK(test::kRayDistinctBlocks >= 8);
}

TEST(water_is_not_targetable_and_a_torch_is)
{
    // The two halves of why the selection table exists, stated directly rather
    // than left implicit in the sweep.
    CHECK(!block::isTargetable(BlockId(mcver::Block::Water)));
    CHECK(!block::isTargetable(BlockId(mcver::Block::Lava)));
    CHECK(block::isTargetable(BlockId(mcver::Block::Torch)));

    // And a torch has no collision box at all, so it is targetable *and*
    // walk-through-able, which no single table could express.
    AABB boxes[block::kMaxCollisionBoxes];
    CHECK_EQ(block::collisionBoxes(BlockId(mcver::Block::Torch), 5, boxes,
                                   block::kMaxCollisionBoxes),
             0);
}

TEST(the_face_a_ray_strikes_names_the_block_a_placement_goes_into)
{
    // placeX/Y/Z is the neighbour on the struck face, which is where a placed
    // block belongs. Aiming down at a floor must offer the block above it.
    SceneWorld scene(0, 0);
    for (i32 x = -3; x <= 3; ++x) {
        for (i32 z = -3; z <= 3; ++z) {
            scene.place(x, 64, z, BlockId(mcver::Block::Stone), 0);
        }
    }

    const RayHit down = rayTrace(scene.w(), 0.5, 67.0, 0.5, 0.0, -1.0, 0.0);
    CHECK(down.hit);
    CHECK_EQ(down.y, 64);
    CHECK_EQ(int(down.face), int(mesh::kFacePosY));
    CHECK_EQ(down.placeY(), 65);
    CHECK_EQ(down.placeX(), down.x);

    // And a ray that reaches nothing within its reach is a miss, not a hit at
    // the far end.
    const RayHit up = rayTrace(scene.w(), 0.5, 67.0, 0.5, 0.0, 1.0, 0.0);
    CHECK(!up.hit);
}
