// Hanging a painting -- `jc` (EntityPainting) and `od.a(...)` (ItemPainting).
//
// "Paintings don't work" was one bug with a very specific shape: item 321 has a
// real `onItemUse` in a1.1.2, it runs to completion, and the last thing it does
// is hand an entity to the world. With nowhere to put an entity the click did
// everything and produced nothing, silently -- which is why it looked like the
// item was unimplemented rather than like the renderer was missing.
//
// What is checkable here is everything except how it looks: the wall rule, the
// art draw, the standoff, the box, and the two directions the geometry has to
// tell apart.

#include "core/block/registry.hpp"
#include "core/entity/painting.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/painting_mesh.hpp"
#include "core/tick/tick_world.hpp"
#include "core/entity/ray_trace.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Painting;
using mc::entity::PaintingSystem;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

// The painting item, found by the generated column rather than typed.
item::ItemId paintingItem()
{
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Painting) {
            return item::ItemId(id);
        }
    }
    return 0;
}

// A flat wall in the x/y plane at z = 0, `wide` blocks across and `tall` up,
// with its bottom-left corner at (0, 64, 0). The open side is -Z.
struct Wall {
    SceneWorld scene{0, 0};
    PaintingSystem paintings{1234};

    Wall(int wide, int tall)
    {
        for (i32 x = -2; x < i32(wide) + 2; ++x) {
            for (int y = 62; y < 64 + tall + 2; ++y) {
                // Only the panel itself is stone; everything around it is air,
                // so a picture that needed one more block than was asked for
                // fails rather than quietly finding support.
                const bool inPanel = x >= 0 && x < i32(wide) && y >= 64 && y < 64 + tall;
                if (inPanel) {
                    scene.place(x, y, 0, bid(mcver::Block::Stone), 0);
                }
            }
        }
    }

    tick::TickWorld& w() { return scene.w(); }

    // Click the -Z face of a wall block, which is a1.1.2's face 2.
    bool hang(i32 x, int y)
    {
        return paintings.place(w(), x, y, 0, 2);
    }
};

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST(the_painting_item_is_the_one_the_table_says_spawns_a_painting)
{
    // Nothing in this file names item 321, and nothing in the engine does
    // either -- see the `spawns` column. This is the check that the column
    // found exactly one.
    int found = 0;
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Painting) {
            ++found;
        }
    }
    CHECK_EQ(found, 1);
    CHECK(paintingItem() != 0);
}

TEST(a_one_block_gap_takes_only_a_one_block_picture)
{
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));
    CHECK_EQ(wall.paintings.count(), 1);

    const Painting& p = wall.paintings[0];
    CHECK_EQ(p.artwork().blocksWide(), 1);
    CHECK_EQ(p.artwork().blocksTall(), 1);
}

TEST(a_bare_block_with_nothing_around_it_still_takes_a_painting)
{
    // The smallest possible wall. If this fails, nothing can be hung anywhere.
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));
}

TEST(a_wall_that_is_only_air_refuses)
{
    SceneWorld scene{0, 0};
    PaintingSystem paintings{7};
    // No blocks at all: `onValidSurface` fails on the first cell it tests.
    CHECK(!paintings.place(scene.w(), 0, 64, 0, 2));
    CHECK_EQ(paintings.count(), 0);
}

TEST(the_floor_and_the_ceiling_refuse_outright)
{
    Wall wall(2, 2);
    // Faces 0 and 1 are -Y and +Y. `od.onItemUse` returns before it builds
    // anything for either, which is why a painting cannot lie on the ground.
    CHECK(!wall.paintings.place(wall.w(), 0, 64, 0, 0));
    CHECK(!wall.paintings.place(wall.w(), 0, 64, 0, 1));
    CHECK_EQ(wall.paintings.count(), 0);
}

TEST(a_bigger_wall_can_produce_a_bigger_picture)
{
    // The art is drawn at random from the ones that fit, so a single placement
    // proves nothing. Over many walls the largest picture seen on a 4 x 4 has
    // to exceed the largest possible on a 1 x 1, which is the property that
    // says the fitting rule is doing something.
    int biggestSmall = 0;
    int biggestLarge = 0;
    for (int trial = 0; trial < 40; ++trial) {
        Wall small(1, 1);
        small.paintings = PaintingSystem(trial);
        if (small.hang(0, 64)) {
            const int area = small.paintings[0].artwork().blocksWide()
                             * small.paintings[0].artwork().blocksTall();
            biggestSmall = area > biggestSmall ? area : biggestSmall;
        }

        Wall large(4, 4);
        large.paintings = PaintingSystem(trial);
        if (large.hang(0, 64)) {
            const int area = large.paintings[0].artwork().blocksWide()
                             * large.paintings[0].artwork().blocksTall();
            biggestLarge = area > biggestLarge ? area : biggestLarge;
        }
    }
    CHECK_EQ(biggestSmall, 1);
    CHECK(biggestLarge > 1);
}

TEST(a_painting_never_covers_a_block_that_is_not_there)
{
    // Whatever art comes out, every cell it covers has to be solid. This walks
    // the placed picture and checks the wall under it -- which is the whole of
    // `onValidSurface` re-derived from the outside.
    for (int trial = 0; trial < 30; ++trial) {
        Wall wall(3, 3);
        wall.paintings = PaintingSystem(trial * 31 + 5);
        if (!wall.hang(1, 65)) {
            continue;
        }
        const Painting& p = wall.paintings[0];
        const int wide = p.artwork().blocksWide();
        const int tall = p.artwork().blocksTall();

        const i32 cornerX = i32(std::floor(p.x - double(p.artwork().width) / 32.0));
        const int cornerY = int(std::floor(p.y - double(p.artwork().height) / 32.0));
        for (int i = 0; i < wide; ++i) {
            for (int j = 0; j < tall; ++j) {
                CHECK(block::def(wall.w().blockAt(cornerX + i, cornerY + j, 0)).solid);
            }
        }
    }
}

TEST(the_canvas_stands_off_the_wall_by_nine_sixteenths)
{
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));
    const Painting& p = wall.paintings[0];

    // Direction 0 hangs on a block's -Z side, so the canvas centre is the block
    // centre less 0.5625 -- half a block out of the block, plus the sixteenth
    // that is the canvas itself.
    CHECK_EQ(p.direction, 0);
    CHECK(near(p.z, 0.5 - entity::kPaintingStandoff));

    // ...and the box is a thirty-second of a block deep, less the hair the
    // original shaves off it so a flush painting does not collide with its own
    // wall.
    const double depth = p.box.maxZ - p.box.minZ;
    CHECK(depth < 2.0 * entity::kPaintingHalfDepth);
    CHECK(depth > 0.0);
}

TEST(the_four_directions_put_the_canvas_on_four_different_sides)
{
    // Faces 2, 3, 4 and 5 -- the four horizontal ones -- must give four
    // distinct facings, and each must stand the painting *outside* the block it
    // was hung on. A direction table that collapsed two of them would put two
    // paintings inside each other.
    const int faces[4] = {2, 3, 4, 5};
    double cx[4], cz[4];
    int dirs[4];
    for (int i = 0; i < 4; ++i) {
        SceneWorld scene{0, 0};
        for (i32 x = -1; x <= 1; ++x) {
            for (i32 z = -1; z <= 1; ++z) {
                for (int y = 63; y <= 65; ++y) {
                    scene.place(x, y, z, bid(mcver::Block::Stone), 0);
                }
            }
        }
        PaintingSystem pool{99};
        CHECK(pool.place(scene.w(), 0, 64, 0, faces[i]));
        cx[i] = pool[0].x;
        cz[i] = pool[0].z;
        dirs[i] = pool[0].direction;
    }

    // Four distinct directions.
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            CHECK(dirs[a] != dirs[b]);
        }
    }
    // Face 2 is -Z and face 3 is +Z: the canvas ends up on opposite sides of
    // the block's centre.
    CHECK((cz[0] - 0.5) * (cz[1] - 0.5) < 0.0);
    // Face 4 is -X and face 5 is +X.
    CHECK((cx[2] - 0.5) * (cx[3] - 0.5) < 0.0);
}

TEST(two_paintings_do_not_hang_in_the_same_place)
{
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));
    // The second finds the first already there and no art that avoids it, so
    // the fitting list comes out empty.
    CHECK(!wall.hang(0, 64));
    CHECK_EQ(wall.paintings.count(), 1);
}

TEST(a_painting_falls_off_when_its_wall_is_mined)
{
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));

    wall.scene.place(0, 64, 0, bid(mcver::Block::Air), 0);
    // The check is on a hundred-tick counter in the original, so it does not
    // vanish immediately -- and that delay is the game's, not a slow reaction
    // of ours.
    for (int i = 0; i < entity::kPaintingCheckInterval - 1; ++i) {
        wall.paintings.tick(wall.w());
    }
    CHECK_EQ(wall.paintings.count(), 1);
    wall.paintings.tick(wall.w());
    CHECK_EQ(wall.paintings.count(), 0);
}

TEST(there_is_no_painting_limit)
{
    // A long wall and more pictures than the pool first holds. A gallery is a
    // thing players build, and the original lets them.
    SceneWorld scene{0, 0};
    const i32 length = i32(PaintingSystem::kInitialCapacity) * 4;
    for (i32 x = 0; x < length; ++x) {
        scene.place(x, 64, 0, bid(mcver::Block::Stone), 0);
    }
    PaintingSystem pool{5};
    for (i32 x = 0; x < length; ++x) {
        pool.place(scene.w(), x, 64, 0, 2);
    }
    CHECK(pool.count() > PaintingSystem::kInitialCapacity);
    CHECK_EQ(int(pool.refused()), 0);
}

TEST(a_right_click_with_a_painting_reaches_the_pool)
{
    // The seam itself: `item::rightClick` has to route the painting item to
    // the pool rather than to `places`, which is 0 for it. Without the route
    // the click does nothing at all, which is the reported bug.
    Wall wall(1, 1);

    entity::RayHit hit;
    hit.hit = true;
    hit.x = 0;
    hit.y = 64;
    hit.z = 0;
    hit.face = mesh::Face(2);

    item::Effects none;
    CHECK(!item::rightClick(wall.w(), paintingItem(), hit, kNoPlayer, 0.0f, none));
    CHECK_EQ(wall.paintings.count(), 0);

    item::Effects effects;
    effects.entities.paintings = &wall.paintings;
    CHECK(item::rightClick(wall.w(), paintingItem(), hit, kNoPlayer, 0.0f, effects));
    CHECK_EQ(wall.paintings.count(), 1);
}

TEST(a_painting_meshes_into_six_faces_per_block_cell)
{
    Wall wall(4, 4);
    CHECK(wall.hang(1, 65));
    const Painting& p = wall.paintings[0];

    mesh::DetailVertex verts[render::kPaintingMaxVertices];
    // Meshed against an origin near the painting, because a detail position is
    // a signed short of 1/1024 blocks and reaches only about 31 blocks. A wall
    // at y = 65 is already outside that from the world origin, which is exactly
    // the case the next test pins.
    const double ox = 0.0, oy = 64.0, oz = 0.0;
    const int written = render::buildPaintings(wall.paintings, ox, oy, oz, verts,
                                               render::kPaintingMaxVertices);

    const int cells = p.artwork().blocksWide() * p.artwork().blocksTall();
    CHECK_EQ(written, cells * 6 * 4);

    // The canvas has to sit where the entity says it does, and it has to be one
    // model unit thick -- z from -0.5 to +0.5 before the 1/16 scale.
    //
    // **That is twice as thick as the bounding box**, and it is the original's
    // own arithmetic rather than a mismatch here: `setDirection` builds a box
    // 0.5/32 of a block deep either side of the centre while `renderPainting`
    // draws the canvas 0.5/16 either side. A painting you can see through the
    // edge of is a painting whose hitbox is thinner than its picture, which is
    // what a1.1.2 has.
    double lo = 1e30, hi = -1e30;
    for (int i = 0; i < written; ++i) {
        const double z = double(verts[i].z) / double(mesh::kDetailUnitsPerBlock) + oz;
        lo = z < lo ? z : lo;
        hi = z > hi ? z : hi;
    }
    CHECK(near(hi - lo, 1.0 / 16.0, 0.005));
    CHECK(near((lo + hi) / 2.0, p.z, 0.005));
    CHECK(hi - lo > p.box.maxZ - p.box.minZ);
}

TEST(the_picture_samples_its_own_rectangle_of_the_art_sheet)
{
    // The front faces have to land inside the art's own rectangle in kz.png,
    // and the back and edges inside the one back-of-canvas tile. A UV that
    // strayed would put somebody else's painting on this one -- and the sheet
    // is dense enough that it would still look like a painting, which is why
    // this is checked rather than looked at.
    Wall wall(2, 2);
    CHECK(wall.hang(0, 64));
    const Painting& p = wall.paintings[0];

    mesh::DetailVertex verts[render::kPaintingMaxVertices];
    const int written = render::buildPaintings(wall.paintings, 0.0, 64.0, 0.0, verts,
                                               render::kPaintingMaxVertices);
    CHECK(written > 0);

    const double edge = double(render::kArtSheetEdge);
    for (int q = 0; q * 4 < written; ++q) {
        // Six faces a cell, and the first of the six is the picture.
        const bool isFront = (q % 6) == 0;
        for (int c = 0; c < 4; ++c) {
            const mesh::DetailVertex& v = verts[q * 4 + c];
            const double u = double(v.u) * edge / double(mesh::kUvUnitsPerAtlas);
            const double t = double(v.v) * edge / double(mesh::kUvUnitsPerAtlas);
            if (isFront) {
                CHECK(u >= double(p.artwork().u) - 0.01);
                CHECK(u <= double(p.artwork().u) + double(p.artwork().width) + 0.01);
                CHECK(t >= double(p.artwork().v) - 0.01);
                CHECK(t <= double(p.artwork().v) + double(p.artwork().height) + 0.01);
            } else {
                CHECK(u >= double(render::kCanvasBackU) - 0.01);
                CHECK(u <= double(render::kCanvasBackU + render::kCanvasBackSize) + 0.01);
                CHECK(t >= double(render::kCanvasBackV) - 0.01);
                CHECK(t <= double(render::kCanvasBackV + render::kCanvasBackSize) + 0.01);
            }
        }
    }
}

TEST(a_painting_too_far_from_the_origin_is_skipped_rather_than_clamped)
{
    // The detail position is a signed short of 1/1024 blocks. A picture past
    // that range is dropped, exactly as a dropped item is: clamping it would
    // stick a painting to the edge of the world.
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));

    mesh::DetailVertex verts[render::kPaintingMaxVertices];
    CHECK(render::buildPaintings(wall.paintings, 0.0, 64.0, 0.0, verts,
                                 render::kPaintingMaxVertices)
          > 0);
    CHECK_EQ(render::buildPaintings(wall.paintings, 100000.0, 64.0, 0.0, verts,
                                    render::kPaintingMaxVertices),
             0);
}

// ---------------------------------------------------------------------------
// What breaks one, and what it leaves
// ---------------------------------------------------------------------------

TEST(a_painting_whose_wall_is_mined_leaves_a_painting_item)
{
    // `jc.e_()` -- the hundred-tick `onValidSurface` check, and the
    // `EntityItem` it spawns as it removes the painting. The removal was here
    // and the item was not, so a mined wall made a picture vanish.
    Wall wall(1, 1);
    mc::test::DropCatcher caught;
    caught.watch(wall.w());
    CHECK(wall.hang(0, 64));
    CHECK_EQ(wall.paintings.count(), 1);

    wall.w().setBlockWithNotify(0, 64, 0, block::kAir);
    // **Not before the hundredth tick.** The counter is the original's and it
    // is why a painting outlives its wall for five seconds.
    for (int i = 0; i < entity::kPaintingCheckInterval - 1; ++i) {
        wall.paintings.tick(wall.w());
    }
    CHECK_EQ(wall.paintings.count(), 1);
    CHECK_EQ(int(caught.drops.size()), 0);

    wall.paintings.tick(wall.w());
    CHECK_EQ(wall.paintings.count(), 0);
    CHECK_EQ(caught.countOf(u16(paintingItem())), 1);
}

TEST(one_hit_takes_a_painting_down_whatever_it_was_hit_with)
{
    // `jc.a(Lkh;I)Z` never reads its damage argument: there is no counter to
    // accumulate into, unlike the boat's and the cart's.
    Wall wall(1, 1);
    mc::test::DropCatcher caught;
    caught.watch(wall.w());
    CHECK(wall.hang(0, 64));

    // A ray straight at the canvas from in front of it. The painting hangs on
    // the -Z face, so this looks along +Z from a block away.
    item::EntityPools pools;
    pools.paintings = &wall.paintings;
    const int index =
        item::pickEntity(pools, 0.5, 64.5, -2.0, 0.0, 0.0, 1.0, entity::RayHit{}).index;
    CHECK(index >= 0);
    CHECK(wall.paintings.attack(wall.w(), index));
    CHECK_EQ(wall.paintings.count(), 0);
    CHECK_EQ(caught.countOf(u16(paintingItem())), 1);
}

TEST(a_ray_that_misses_the_canvas_picks_nothing)
{
    // The box grows by the same tenth every entity's does in `getMouseOver`,
    // and no more: a crosshair on the wall a little beside a picture hits the
    // wall.
    Wall wall(1, 1);
    CHECK(wall.hang(0, 64));
    item::EntityPools pools;
    pools.paintings = &wall.paintings;
    const entity::RayHit noBlock{};
    CHECK(!item::pickEntity(pools, 0.5, 64.5, -2.0, 0.0, 0.0, -1.0, noBlock).found());
    CHECK(!item::pickEntity(pools, 0.5, 70.0, -2.0, 0.0, 0.0, 1.0, noBlock).found());
    const AABB box = wall.paintings[0].box;
    CHECK(item::pickEntity(pools, 0.5, box.maxY + 0.05, -2.0, 0.0, 0.0, 1.0, noBlock).found());
    CHECK(!item::pickEntity(pools, 0.5, box.maxY + 0.15, -2.0, 0.0, 0.0, 1.0, noBlock).found());
}
