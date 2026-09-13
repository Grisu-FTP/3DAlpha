// Block collision shapes, against every combination a real jar answers.
//
// The fixture is the whole truth table -- each of the seventy blocks a1.1.2
// constructs, crossed with all sixteen metadata values -- so this is not a
// sample of the resolver's behaviour, it is all of it. See
// tests/collision_box_vectors.hpp for how it was taken.
//
// The negative controls matter as much as the comparison. A resolver that
// answered "a full cube, always" would satisfy a naive walk over the rows if
// the fixture happened to be all full cubes, and one that answered "nothing
// collides" would satisfy it if the fixture were all empties. The counts
// asserted below make both of those fail.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "collision_box_vectors.hpp"
#include "framework.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::block::collisionBoxes;
using mc::block::kMaxCollisionBoxes;

namespace {

// Exact equality is the right comparison, not a tolerance. Every bound in the
// table is a multiple of a sixteenth, which is exact in binary, and the C++
// literals were emitted by a generator that refused to print any that did not
// parse back to the same double. A disagreement here is a wrong number, never a
// rounding step.
bool same(const AABB& box, const test::CollisionBox& want)
{
    return box.minX == want.minX && box.minY == want.minY && box.minZ == want.minZ
        && box.maxX == want.maxX && box.maxY == want.maxY && box.maxZ == want.maxZ;
}

}  // namespace

TEST(every_block_and_metadata_collides_exactly_as_the_jar_does)
{
    for (int i = 0; i < test::kCollisionCaseCount; ++i) {
        const test::CollisionCase& c = test::kCollisionCases[i];

        AABB boxes[kMaxCollisionBoxes];
        const int count = collisionBoxes(BlockId(c.id), c.metadata, boxes, kMaxCollisionBoxes);

        if (count != c.boxCount) {
            CHECK_EQ(count, c.boxCount);
            return;  // One mismatch is the finding; 1,120 lines of it is not.
        }
        for (int b = 0; b < count; ++b) {
            if (!same(boxes[b], c.boxes[b])) {
                CHECK(same(boxes[b], c.boxes[b]));
                return;
            }
        }
    }
}

TEST(the_fixture_contains_enough_variety_to_catch_a_degenerate_resolver)
{
    int empty = 0;
    int multi = 0;
    int nonUnit = 0;
    for (int i = 0; i < test::kCollisionCaseCount; ++i) {
        const test::CollisionCase& c = test::kCollisionCases[i];
        if (c.boxCount == 0) { ++empty; }
        if (c.boxCount > 1) { ++multi; }
        for (int b = 0; b < c.boxCount; ++b) {
            const test::CollisionBox& x = c.boxes[b];
            const bool unit = x.minX == 0.0 && x.minY == 0.0 && x.minZ == 0.0
                           && x.maxX == 1.0 && x.maxY == 1.0 && x.maxZ == 1.0;
            if (!unit) { ++nonUnit; }
        }
    }
    CHECK_EQ(empty, test::kCollisionEmptyCases);
    CHECK_EQ(multi, test::kCollisionMultiBoxCases);
    CHECK_EQ(nonUnit, test::kCollisionNonUnitBoxes);

    // And that those controls are not themselves vacuous.
    CHECK(test::kCollisionEmptyCases > 0);
    CHECK(test::kCollisionMultiBoxCases > 0);
    CHECK(test::kCollisionNonUnitBoxes > 0);
}

TEST(an_unknown_block_id_is_something_to_stand_on_rather_than_fall_through)
{
    // A world written by a mod or a newer server can hold an id this build has
    // never heard of. Falling through it would be the worse failure of the two.
    AABB boxes[kMaxCollisionBoxes];
    const int count = collisionBoxes(BlockId(mcver::kBlockTableSize - 1), 0, boxes,
                                     kMaxCollisionBoxes);
    CHECK_EQ(count, 1);
    CHECK(boxes[0].maxY == 1.0);
}

TEST(a_caller_with_room_for_one_box_gets_one_box_and_is_told_so)
{
    // Stairs are the only block that would overrun a single-box buffer, so they
    // are the only case worth asserting -- and the count has to be truthful, or
    // the caller reads a box that was never written.
    const test::CollisionCase* stairs = nullptr;
    for (int i = 0; i < test::kCollisionCaseCount; ++i) {
        if (test::kCollisionCases[i].boxCount == 2) {
            stairs = &test::kCollisionCases[i];
            break;
        }
    }
    CHECK(stairs != nullptr);
    if (stairs == nullptr) {
        return;
    }

    AABB one[1];
    const int count = collisionBoxes(BlockId(stairs->id), stairs->metadata, one, 1);
    CHECK_EQ(count, 1);
    CHECK(same(one[0], stairs->boxes[0]));
}

TEST(air_collides_with_nothing)
{
    AABB boxes[kMaxCollisionBoxes];
    CHECK_EQ(collisionBoxes(block::kAir, 0, boxes, kMaxCollisionBoxes), 0);
}

TEST(ground_friction_matches_the_jar_for_every_block)
{
    // Slipperiness rides in the collision fixture because it is the other half
    // of what a block does to something standing on it, and because
    // moveEntityWithHeading reads it in the same breath as the collision box.
    int defined = 0;
    int slippery = 0;
    for (int id = 0; id < 256; ++id) {
        const float want = test::kBlockSlipperiness[id];
        if (want == 0.0f) {
            continue;  // a block this version never constructs
        }
        ++defined;
        if (want != 0.6f) {
            ++slippery;
        }
        CHECK_EQ(double(block::slipperinessOf(BlockId(id))), double(want));
    }
    CHECK_EQ(defined, 70);

    // Exactly one block in a1.1.2 is slippery, and a table that lost it would
    // still pass every equality above if the fixture were read wrong.
    CHECK_EQ(slippery, 1);
}

TEST(the_selection_shape_matches_the_jar_for_every_block_and_metadata)
{
    // The shape a ray is tested against, which is a different table from the
    // collision one and differs from it for exactly the blocks that matter: a
    // torch has no collision box and a perfectly good selection box.
    for (int i = 0; i < test::kCollisionCaseCount; ++i) {
        const test::CollisionCase& c = test::kCollisionCases[i];
        const test::CollisionBox& want = test::kSelectionBoxes[i];
        const AABB got = block::selectionBox(BlockId(c.id), c.metadata);
        if (!same(got, want)) {
            CHECK(same(got, want));
            return;
        }
    }
}

TEST(the_selection_and_collision_shapes_genuinely_disagree)
{
    // If they were the same table there would be no reason for two of them, and
    // a resolver that quietly returned the collision box would pass the check
    // above on every block whose shapes happen to match.
    int differ = 0;
    int targetableWithoutCollision = 0;
    for (int i = 0; i < test::kCollisionCaseCount; ++i) {
        const test::CollisionCase& c = test::kCollisionCases[i];
        AABB boxes[kMaxCollisionBoxes];
        const int count = collisionBoxes(BlockId(c.id), c.metadata, boxes, kMaxCollisionBoxes);
        const AABB selection = block::selectionBox(BlockId(c.id), c.metadata);
        if (count == 0 || !same(selection, c.boxes[0])) {
            ++differ;
        }
        if (count == 0 && block::isTargetable(BlockId(c.id))) {
            ++targetableWithoutCollision;
        }
    }
    CHECK(differ > 0);
    // Torches, plants, rails, snow: aimable, not walk-into-able. This is the
    // whole reason the two tables exist.
    CHECK(targetableWithoutCollision > 0);
}

TEST(only_water_lava_and_fire_are_invisible_to_a_ray)
{
    int blind = 0;
    for (int i = 0; i < test::kCollisionCaseCount; ++i) {
        const test::CollisionCase& c = test::kCollisionCases[i];
        const bool want = test::kTargetable[i];
        CHECK_EQ(block::isTargetable(BlockId(c.id)), want);
        if (!want) {
            ++blind;
        }
    }
    // Five blocks x sixteen metadata values: still water, moving water, still
    // lava, moving lava, fire.
    CHECK_EQ(blind, 5 * 16);
    CHECK_EQ(test::kCollisionCaseCount - blind, test::kTargetableCases);
}

TEST(the_item_render_shape_matches_the_jar_for_every_block)
{
    // `Block.setBlockBoundsForItemRender`, which is the shape the hand, an
    // inventory slot and a dropped stack draw -- and is neither of the two
    // tables above. One box per id: nothing on that path has a metadata to
    // give it.
    for (int id = 0; id < 256; ++id) {
        const test::CollisionBox& want = test::kItemRenderBoxes[id];
        const AABB got = block::itemRenderBox(BlockId(id));
        if (!same(got, want)) {
            CHECK(same(got, want));
            return;
        }
    }
}

TEST(the_button_and_the_plates_are_drawn_as_items_unlike_how_they_sit_in_the_world)
{
    // The negative control, and the bug that put this table here: a reader that
    // ignored `setBlockBoundsForItemRender` and used `selectionBox(id, 0)`
    // instead put a **full stone cube** in the hand and in the inventory slot
    // where a1.1.2 draws a button, because a button's world bounds are written
    // in `setBlockBoundsBasedOnState` and metadata 0 is not one of the four
    // values it writes for.
    //
    // Three blocks moved, which is what the jar sweep counted.
    CHECK_EQ(test::kItemRenderOverrides, 3);

    const AABB unitCube{0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
    const BlockId button = BlockId(mcver::Block::StoneButton);
    CHECK(same(block::selectionBox(button, 0), test::CollisionBox{0.0, 0.0, 0.0, 1.0, 1.0, 1.0}));
    const AABB held = block::itemRenderBox(button);
    CHECK(!same(held, test::CollisionBox{unitCube.minX, unitCube.minY, unitCube.minZ,
                                         unitCube.maxX, unitCube.maxY, unitCube.maxZ}));
    // Six texels wide, four tall, four deep, centred in the cell -- `hu.e()V`.
    CHECK(same(held, test::CollisionBox{0.3125, 0.375, 0.375, 0.6875, 0.625, 0.625}));

    // A plate in the hand is full width and four texels thick in the middle of
    // the cell, not the wafer on the floor that it is in the world.
    for (mcver::Block plate : {mcver::Block::StonePressurePlate,
                               mcver::Block::WoodenPressurePlate}) {
        const BlockId id = BlockId(plate);
        CHECK(same(block::itemRenderBox(id),
                   test::CollisionBox{0.0, 0.375, 0.0, 1.0, 0.625, 1.0}));
        CHECK(!same(block::selectionBox(id, 0),
                    test::CollisionBox{0.0, 0.375, 0.0, 1.0, 0.625, 1.0}));
    }
}
