// Turning a dropped item into quads.
//
// The interesting claims are structural rather than numeric: that a block on
// the ground is drawn as a *block* and everything else as a sprite, that the
// two sheets between them cover the pool exactly once, that a bigger stack
// draws as more copies, and that the whole thing stays inside the buffer it was
// given. The bob and the spin are checked by watching them move rather than by
// comparing against numbers no oracle can produce.

#include "core/entity/item_entity.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"
#include "core/render/item_entity_mesh.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <vector>

using namespace mc;
using mc::entity::ItemEntitySystem;
using mc::item::IconSheet;
using mc::item::ItemId;
using mc::test::SceneWorld;

namespace {

// A world with nothing in it: the mesh cares about the entities, not the
// terrain, and a spawn only reads the light where it lands.
struct Bare {
    SceneWorld world{0, 0};
    ItemEntitySystem items{99};
};

// The first palette item that draws as a block, and the first that does not.
ItemId firstBlockItem()
{
    for (int i = 0; i < item::paletteSize(); ++i) {
        const ItemId id = item::paletteItem(i);
        if (item::def(id).sheet == IconSheet::Terrain
            && block::def(block::BlockId(item::def(id).places)).render
                   == block::RenderType::Cube) {
            return id;
        }
    }
    return 0;
}

ItemId firstSpriteItem()
{
    for (int i = 0; i < item::paletteSize(); ++i) {
        const ItemId id = item::paletteItem(i);
        if (item::def(id).sheet == IconSheet::Items) {
            return id;
        }
    }
    return 0;
}

int build(const ItemEntitySystem& items, IconSheet sheet, std::vector<mesh::DetailVertex>& out,
          float partial = 0.0f, float yaw = 0.0f)
{
    out.assign(4096, mesh::DetailVertex{});
    return render::buildItemEntities(items, yaw, 0.0, 0.0, 0.0, partial, sheet, out.data(),
                                     int(out.size()));
}

}  // namespace

TEST(a_block_on_the_ground_is_drawn_as_a_block_from_the_terrain_sheet)
{
    const ItemId id = firstBlockItem();
    CHECK(id != 0);

    Bare b;
    CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 1, 0));

    std::vector<mesh::DetailVertex> verts;
    // Six faces, four vertices each, one copy for a stack of one.
    CHECK_EQ(build(b.items, IconSheet::Terrain, verts), 24);
    // And nothing at all on the item sheet, so the two passes do not double it.
    CHECK_EQ(build(b.items, IconSheet::Items, verts), 0);
}

TEST(an_item_that_is_not_a_block_is_drawn_as_one_sprite_from_its_own_sheet)
{
    const ItemId id = firstSpriteItem();
    CHECK(id != 0);

    Bare b;
    CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 1, 0));

    std::vector<mesh::DetailVertex> verts;
    CHECK_EQ(build(b.items, IconSheet::Items, verts), 4);
    CHECK_EQ(build(b.items, IconSheet::Terrain, verts), 0);
}

TEST(a_bigger_stack_is_drawn_as_more_copies)
{
    // `RenderItem`'s three thresholds, and the geometry has to follow them.
    CHECK_EQ(render::itemCopies(1), 1);
    CHECK_EQ(render::itemCopies(2), 2);
    CHECK_EQ(render::itemCopies(5), 2);
    CHECK_EQ(render::itemCopies(6), 3);
    CHECK_EQ(render::itemCopies(20), 3);
    CHECK_EQ(render::itemCopies(21), 4);

    const ItemId id = firstBlockItem();
    std::vector<mesh::DetailVertex> verts;
    for (int stack : {1, 2, 6, 21}) {
        Bare b;
        CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, stack, 0));
        CHECK_EQ(build(b.items, IconSheet::Terrain, verts),
                 24 * render::itemCopies(stack));
    }
}

TEST(the_copies_of_one_pile_are_the_same_every_frame)
{
    // `random.setSeed(187L)` per entity is what stops a pile shimmering, and it
    // is the one literal seed in this project that is the point rather than an
    // accident.
    const ItemId id = firstBlockItem();
    Bare b;
    CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 21, 0));

    std::vector<mesh::DetailVertex> first;
    std::vector<mesh::DetailVertex> second;
    const int a = build(b.items, IconSheet::Terrain, first);
    const int c = build(b.items, IconSheet::Terrain, second);
    CHECK_EQ(a, c);
    for (int i = 0; i < a; ++i) {
        CHECK_EQ(int(first[usize(i)].x), int(second[usize(i)].x));
        CHECK_EQ(int(first[usize(i)].y), int(second[usize(i)].y));
        CHECK_EQ(int(first[usize(i)].z), int(second[usize(i)].z));
    }
}

TEST(an_item_bobs_and_spins_as_it_ages)
{
    const ItemId id = firstBlockItem();
    Bare b;
    CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 1, 0));

    std::vector<mesh::DetailVertex> atZero;
    std::vector<mesh::DetailVertex> atHalf;
    const int n = build(b.items, IconSheet::Terrain, atZero, 0.0f);
    CHECK_EQ(build(b.items, IconSheet::Terrain, atHalf, 5.0f), n);

    // Five ticks on is a different height and a different turn -- the partial
    // is what makes both smooth between ticks rather than stepping at 20 Hz.
    bool moved = false;
    for (int i = 0; i < n; ++i) {
        moved = moved || atZero[usize(i)].y != atHalf[usize(i)].y
                || atZero[usize(i)].x != atHalf[usize(i)].x;
    }
    CHECK(moved);
}

TEST(a_sprite_turns_to_face_the_camera_about_the_vertical_axis_only)
{
    const ItemId id = firstSpriteItem();
    Bare b;
    CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 1, 0));

    std::vector<mesh::DetailVertex> north;
    std::vector<mesh::DetailVertex> east;
    CHECK_EQ(build(b.items, IconSheet::Items, north, 0.0f, 0.0f), 4);
    CHECK_EQ(build(b.items, IconSheet::Items, east, 0.0f, 90.0f), 4);

    bool turned = false;
    for (int i = 0; i < 4; ++i) {
        turned = turned || north[usize(i)].x != east[usize(i)].x
                 || north[usize(i)].z != east[usize(i)].z;
    }
    CHECK(turned);

    // The heights are the same whichever way it is turned, which is what "no
    // pitch term" means: a sprite stays upright.
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(int(north[usize(i)].y), int(east[usize(i)].y));
    }
}

TEST(the_builder_stops_at_the_buffer_it_was_given)
{
    const ItemId id = firstBlockItem();
    Bare b;
    for (int i = 0; i < 64; ++i) {
        CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 21, 0));
    }

    // A buffer far too small for four copies of sixty-four blocks, all in one
    // pile. Nothing may be written past it, what does fit has to be whole
    // quads -- and a pile bigger than the whole budget still draws the part
    // of itself that fits rather than vanishing.
    mesh::DetailVertex small[400];
    const int written = render::buildItemEntities(b.items, 0.0f, 0.0, 0.0, 0.0, 0.0f,
                                                  IconSheet::Terrain, small, 400);
    CHECK(written <= 400);
    CHECK_EQ(written % 4, 0);
    CHECK(written > 0);
}

TEST(past_the_buffer_the_nearest_items_are_drawn)
{
    // The pool has no cap and the buffer has one, so when there are more items
    // in range than it holds the nearest are drawn and the far ones go -- not
    // whichever happen to sit early in the pool. The far ones are spawned
    // first, so a pool-order build would draw only them.
    const ItemId id = firstBlockItem();
    Bare b;
    for (int i = 0; i < 40; ++i) {
        CHECK(b.items.spawn(b.world.w(), 20.5, 4.0, 0.5, id, 1, 0));
    }
    for (int i = 0; i < 10; ++i) {
        CHECK(b.items.spawn(b.world.w(), 1.5, 4.0, 0.5, id, 1, 0));
    }
    std::vector<mesh::DetailVertex> verts(24 * 12);
    const int written =
        render::buildItemEntities(b.items, 0.0f, 0.0, 0.0, 0.0, 0.0f, IconSheet::Terrain,
                                  verts.data(), int(verts.size()));
    CHECK(written >= 24 * 10);
    int nearVertices = 0;
    for (int v = 0; v < written; ++v) {
        // Detail units are 1/1024 block; the near pile is within five blocks.
        if (verts[usize(v)].x < 5 * 1024) {
            ++nearVertices;
        }
    }
    CHECK_EQ(nearVertices, 24 * 10);
}

TEST(an_item_too_far_from_the_origin_is_skipped_rather_than_clamped)
{
    // The same rule the particles follow: a detail position is a signed short
    // of 1/1024 blocks and reaches about 32, and an item stuck to the edge of
    // that reach is worse than a missing one.
    const ItemId id = firstBlockItem();
    Bare b;
    CHECK(b.items.spawn(b.world.w(), 0.5, 4.0, 0.5, id, 1, 0));

    std::vector<mesh::DetailVertex> verts;
    verts.assign(4096, mesh::DetailVertex{});
    CHECK_EQ(render::buildItemEntities(b.items, 0.0f, 0.0, 0.0, 0.0, 0.0f,
                                       IconSheet::Terrain, verts.data(), int(verts.size())),
             24);
    CHECK_EQ(render::buildItemEntities(b.items, 0.0f, 500.0, 0.0, 0.0, 0.0f,
                                       IconSheet::Terrain, verts.data(), int(verts.size())),
             0);
}
