// The bow and the arrow -- `jg.a(...)` and `kg` (EntityArrow).
//
// "The bow doesn't work" was the same bug paintings had: `ItemBow.onItemRightClick`
// runs to completion, plays its sound, builds an entity and hands it to the
// world -- and with nowhere to put an entity the click did every step and
// produced nothing.
//
// The arrow is the cheapest moving entity in the game to check, because it does
// not use `moveEntity`: it ray-traces from where it is to where it would be and
// otherwise just adds its motion. So its whole flight is checkable exactly.

#include "core/block/registry.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/painting.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/arrow_mesh.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "low_heap.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Arrow;
using mc::entity::ArrowSystem;
using mc::entity::MinecartSystem;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

item::ItemId bowItem()
{
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Arrow) {
            return item::ItemId(id);
        }
    }
    return 0;
}

// A stone floor at y = 63 under a wide open sky, so an arrow fired flat has
// somewhere to land and an arrow fired up has nothing to hit.
struct Range {
    SceneWorld scene{0, 0};
    ArrowSystem arrows{4242};

    // **The floor covers the whole resident area**, which matters more than it
    // looks: an arrow leaves the bow at 1.5 blocks a tick, which is thirty
    // blocks a second, and a flat shot travels about thirty before it has
    // fallen the seven blocks to the ground. A short range would let it fly out
    // of the loaded columns and stop ticking, which reads as an arrow that
    // never lands.
    Range()
    {
        for (i32 x = -48; x <= 63; ++x) {
            for (i32 z = -48; z <= 63; ++z) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
    }

    tick::TickWorld& w() { return scene.w(); }

    void run(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            arrows.tick(w());
        }
    }
};

}  // namespace

TEST(the_bow_is_the_item_the_table_says_spawns_an_arrow)
{
    int found = 0;
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Arrow) {
            ++found;
        }
    }
    CHECK_EQ(found, 1);
    CHECK(bowItem() != 0);
}

TEST(firing_puts_an_arrow_in_the_air_moving_where_it_was_aimed)
{
    Range range;
    // Straight along +Z from above the floor.
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    CHECK_EQ(range.arrows.count(), 1);

    const Arrow& a = range.arrows[0];
    // The scatter is a gaussian at 0.0075 per axis against a velocity of 1.5,
    // so the heading is the aim to well within a tenth.
    CHECK(a.motionZ > 1.4);
    CHECK(std::fabs(a.motionX) < 0.1);
    CHECK(std::fabs(a.motionY) < 0.1);
    CHECK(!a.inGround);
}

TEST(an_arrow_leaves_beside_the_bow_and_not_out_of_the_players_face)
{
    // The muzzle offset is 0.16 *perpendicular* to the heading and a tenth
    // down -- which is what puts the arrow beside the bow. An offset applied
    // along the heading instead would be a visible difference and is the easy
    // way to read the constructor wrong.
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    const Arrow& a = range.arrows[0];

    CHECK(std::fabs(a.y - (70.0 - entity::kArrowMuzzleDrop)) < 1e-9);
    // Aiming along +Z at yaw 0 offsets on x, not on z.
    CHECK(std::fabs(a.x) > 0.1);
    CHECK(std::fabs(a.z) < 1e-9);
}

TEST(an_arrow_fired_flat_drops_and_sticks_in_the_ground)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));

    range.run(200);
    CHECK_EQ(range.arrows.count(), 1);
    const Arrow& a = range.arrows[0];
    CHECK(a.inGround);
    // It stuck in the floor, not below it.
    CHECK(a.y > 63.0);
    CHECK(a.y < 65.0);
    // ...and it stopped.
    CHECK_EQ(a.tileY, 63);
}

TEST(gravity_is_three_hundredths_and_not_a_twentieth)
{
    // 0.05 is later Minecraft and is the number this is most likely to be
    // ported with. One tick of a horizontal shot is the cleanest place to see
    // it: motionY starts at ~0, is multiplied by the 0.99 drag, then has
    // gravity taken off.
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    const double before = range.arrows[0].motionY;
    range.run(1);
    const double after = range.arrows[0].motionY;

    const double fall = before * double(entity::kArrowAirDrag) - after;
    CHECK(std::fabs(fall - double(entity::kArrowGravity)) < 1e-6);
}

TEST(an_arrow_that_sticks_stops_moving_entirely)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    range.run(200);
    CHECK(range.arrows[0].inGround);

    const double x = range.arrows[0].x;
    const double y = range.arrows[0].y;
    const double z = range.arrows[0].z;
    range.run(50);
    // A stuck arrow's tick returns before it touches its position -- no
    // gravity, no drag, no ray.
    CHECK_EQ(range.arrows[0].x, x);
    CHECK_EQ(range.arrows[0].y, y);
    CHECK_EQ(range.arrows[0].z, z);
}

TEST(an_arrow_comes_loose_when_the_block_it_stuck_in_is_mined)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    range.run(200);
    CHECK(range.arrows[0].inGround);

    const i32 tx = range.arrows[0].tileX;
    const int ty = range.arrows[0].tileY;
    const i32 tz = range.arrows[0].tileZ;
    range.scene.place(tx, ty, tz, bid(mcver::Block::Air), 0);

    range.run(1);
    CHECK(!range.arrows[0].inGround);
    // ...and then it falls, rather than hanging where it was.
    const double y = range.arrows[0].y;
    range.run(10);
    CHECK(range.arrows.count() == 0 || range.arrows[0].y < y);
}

TEST(a_released_arrow_keeps_its_current_heading_and_interpolates_from_the_previous_one)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    range.run(200);
    CHECK(range.arrows[0].inGround);
    const i32 tx = range.arrows[0].tileX;
    const int ty = range.arrows[0].tileY;
    const i32 tz = range.arrows[0].tileZ;
    range.scene.place(tx, ty, tz, bid(mcver::Block::Air), 0);

    range.run(1);
    const Arrow& a = range.arrows[0];
    // Rotation is assigned after moving, before this tick's drag and gravity.
    const double mx = a.motionX / double(entity::kArrowAirDrag);
    const double my = (a.motionY + double(entity::kArrowGravity)) / double(entity::kArrowAirDrag);
    const double mz = a.motionZ / double(entity::kArrowAirDrag);
    const float horizontal = float(std::sqrt(mx * mx + mz * mz));
    const float heading = float(std::atan2(my, double(horizontal)) * 180.0
                                / 3.1415927410125732);
    CHECK(std::fabs(a.pitch - heading) < 1e-5f);
    CHECK(std::fabs(a.prevPitch - a.pitch) > 1e-5f);
}

TEST(a_stuck_arrow_dies_after_a_minute)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));
    range.run(200);
    CHECK(range.arrows[0].inGround);
    CHECK_EQ(range.arrows.count(), 1);

    // 1200 ticks in the ground, which is a minute. The counter only runs while
    // it is stuck, so the flight time does not count towards it.
    range.run(entity::kArrowMaxStuck + 10);
    CHECK_EQ(range.arrows.count(), 0);
}

TEST(an_arrow_that_hits_nothing_falls_out_of_the_world)
{
    // No floor at all: it falls past the void floor and is removed, which is
    // `Entity.onEntityUpdate`'s last line and the only thing that stops the
    // pool filling with arrows fired off the edge of the world.
    SceneWorld scene{0, 0};
    // One block well clear of the fall line, only so the column the arrow is
    // in counts as resident -- an arrow outside a loaded column does not tick
    // at all, which is the rule every pool here follows.
    scene.place(20, 5, 20, bid(mcver::Block::Stone), 0);
    ArrowSystem arrows{7};
    // Straight down. `motionY` is `-sin(pitch)`, so a positive pitch is
    // downwards -- the original's convention, not this test's.
    CHECK(arrows.shoot(scene.w(), 0.5, 70.0, 0.5, 0.0f, 90.0f));
    for (int i = 0; i < 400; ++i) {
        arrows.tick(scene.w());
    }
    CHECK_EQ(arrows.count(), 0);
}

TEST(there_is_no_arrow_limit)
{
    // a1.1.2 has none: `spawnEntityInWorld` adds to a list. The old fixed pool
    // stopped the bow at 128.
    Range range;
    const int many = ArrowSystem::kInitialCapacity * 3;
    for (int i = 0; i < many; ++i) {
        CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, -45.0f));
    }
    CHECK_EQ(range.arrows.count(), many);
    CHECK_EQ(int(range.arrows.refused()), 0);
    range.arrows.tick(range.w());
    CHECK_EQ(range.arrows.count(), many);
}

TEST(a_heap_that_will_not_hold_another_arrow_refuses_the_shot)
{
    // What stops the pool is the heap, and it stops it with a refusal the bow
    // already handles -- nothing fired -- rather than an allocation failure.
    Range range;
    for (int i = 0; i < ArrowSystem::kInitialCapacity; ++i) {
        CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, -45.0f));
    }
    test::LowHeap low;
    CHECK(!range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, -45.0f));
    CHECK_EQ(range.arrows.count(), ArrowSystem::kInitialCapacity);
    CHECK_EQ(int(range.arrows.refused()), 1);
}

TEST(an_arrow_hits_a_minecart_before_the_block_behind_it)
{
    SceneWorld scene{0, 0};
    // The rail makes placement valid; the stone behind the cart ensures this
    // also checks that entity hits win over a later block ray hit.
    scene.place(0, 63, 0, bid(mcver::Block::Stone), 0);
    scene.place(0, 64, 0, bid(mcver::Block::Rail), 0);
    scene.place(0, 64, 2, bid(mcver::Block::Stone), 0);
    MinecartSystem carts{11};
    CHECK(carts.place(scene.w(), 0, 64, 0, entity::MinecartType::Rideable));

    ArrowSystem arrows{4242};
    CHECK(arrows.shoot(scene.w(), 0.5, 64.8, -1.5, 0.0f, 0.0f));
    arrows.tick(scene.w(), entity::ArrowTargets{nullptr, nullptr, &carts});

    CHECK_EQ(arrows.count(), 0);
    CHECK_EQ(carts.count(), 1);
    CHECK_EQ(carts[0].damage, 40);
    CHECK_EQ(carts[0].timeSinceHit, 10);
}

TEST(two_arrow_hits_break_a_minecart_like_alpha)
{
    SceneWorld scene{0, 0};
    scene.place(0, 63, 0, bid(mcver::Block::Stone), 0);
    scene.place(0, 64, 0, bid(mcver::Block::Rail), 0);
    MinecartSystem carts{11};
    CHECK(carts.place(scene.w(), 0, 64, 0, entity::MinecartType::Rideable));

    CHECK(carts.hitByArrow(scene.w(), 0));
    CHECK_EQ(carts.count(), 1);
    CHECK(carts.hitByArrow(scene.w(), 0));
    CHECK_EQ(carts.count(), 0);
}

// Aims an arrow from `(ex, ey, ez)` at a point, as a player would line one up:
// yaw 0 is +z in this codebase's convention, so the target is straight ahead
// along z and only the pitch is solved for.
bool shootAlongZAt(ArrowSystem& arrows, tick::TickWorld& world, double ex, double ey, double ez,
                   double tx, double ty, double tz)
{
    const double dy = ty - ey;
    const double flat = std::sqrt((tx - ex) * (tx - ex) + (tz - ez) * (tz - ez));
    const float pitch = float(-std::atan2(dy, flat) * 180.0 / 3.141592653589793);
    return arrows.shoot(world, ex, ey, ez, 0.0f, pitch);
}

TEST(two_arrows_from_a_standing_player_break_a_minecart_and_it_leaves_one)
{
    // Reported as "minecarts drop nothing when killed by arrows". The whole
    // chain, as `runGame` ticks it: carts first, then the arrows against them,
    // with the world's drop sink catching what the break leaves.
    SceneWorld scene{0, 0};
    mc::test::DropCatcher caught;
    caught.watch(scene.w());
    for (i32 z = -8; z <= 4; ++z) {
        scene.place(0, 63, z, bid(mcver::Block::Stone), 0);
        scene.place(0, 64, z, bid(mcver::Block::Rail), 0);
    }
    MinecartSystem carts{11};
    CHECK(carts.place(scene.w(), 0, 64, 0, entity::MinecartType::Rideable));
    ArrowSystem arrows{4242};
    const entity::ArrowTargets targets{nullptr, nullptr, &carts};

    for (int t = 0; t < 40 && carts.count() > 0; ++t) {
        if (t == 0 || t == 10) {
            CHECK(shootAlongZAt(arrows, scene.w(), 0.5, 64.0 + 1.62, -6.0, carts[0].x,
                                carts[0].y + 0.3, carts[0].z));
        }
        carts.tick(scene.w(), entity::VehicleRider{});
        arrows.tick(scene.w(), targets);
    }
    CHECK_EQ(carts.count(), 0);
    CHECK_EQ(arrows.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Minecart)), 1);
}

TEST(an_arrow_hits_a_boat_and_a_second_breaks_it_for_planks_and_sticks)
{
    // `dc.c_()` answers `!isDead`, so a boat is in the arrow's sweep exactly as
    // a cart is, and `dc.a(Lkh;I)Z` takes the same 4.
    SceneWorld scene{0, 0};
    mc::test::DropCatcher caught;
    caught.watch(scene.w());
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -8; z <= 4; ++z) {
            scene.place(x, 62, z, bid(mcver::Block::Stone), 0);
            scene.place(x, 63, z, bid(mcver::Block::Water), 0);
        }
    }
    entity::BoatSystem boats{99};
    CHECK(boats.place(scene.w(), 0, 63, 0));
    ArrowSystem arrows{4242};
    const entity::ArrowTargets targets{nullptr, &boats, nullptr};

    CHECK(shootAlongZAt(arrows, scene.w(), 0.5, 64.0 + 1.62, -6.0, boats[0].x,
                        boats[0].y + 0.3, boats[0].z));
    for (int t = 0; t < 10 && arrows.count() > 0; ++t) {
        arrows.tick(scene.w(), targets);
    }
    CHECK_EQ(arrows.count(), 0);
    CHECK_EQ(boats.count(), 1);
    CHECK_EQ(boats[0].damage, 4 * 10);
    CHECK_EQ(boats[0].timeSinceHit, 10);
    CHECK_EQ(int(caught.drops.size()), 0);

    CHECK(shootAlongZAt(arrows, scene.w(), 0.5, 64.0 + 1.62, -6.0, boats[0].x,
                        boats[0].y + 0.3, boats[0].z));
    for (int t = 0; t < 10 && arrows.count() > 0; ++t) {
        arrows.tick(scene.w(), targets);
    }
    CHECK_EQ(boats.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Stick)), entity::kBoatSticksDropped);
}

TEST(an_arrow_knocks_a_painting_off_its_wall_in_one_hit)
{
    // `jc.c_()` is a bare `return true` and its `attackEntityFrom` ignores the
    // amount, so one arrow is enough -- and it strikes the canvas, not the
    // wall behind it.
    SceneWorld scene{0, 0};
    mc::test::DropCatcher caught;
    caught.watch(scene.w());
    scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    entity::PaintingSystem paintings{5};
    CHECK(paintings.place(scene.w(), 0, 64, 0, 2));
    ArrowSystem arrows{4242};
    const entity::ArrowTargets targets{&paintings, nullptr, nullptr};

    CHECK(shootAlongZAt(arrows, scene.w(), 0.5, 64.5, -6.0, paintings[0].x, paintings[0].y,
                        paintings[0].z));
    for (int t = 0; t < 10 && arrows.count() > 0; ++t) {
        arrows.tick(scene.w(), targets);
    }
    CHECK_EQ(arrows.count(), 0);
    CHECK_EQ(paintings.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Painting)), 1);
}

TEST(an_arrow_strikes_the_nearer_of_two_targets_in_line)
{
    // `if (d < best || best == 0.0D)` across every pool at once: a boat in
    // front of a cart takes the arrow and the cart is untouched.
    SceneWorld scene{0, 0};
    for (i32 z = -8; z <= 6; ++z) {
        scene.place(0, 62, z, bid(mcver::Block::Stone), 0);
    }
    scene.place(0, 63, 0, bid(mcver::Block::Water), 0);
    scene.place(0, 63, 3, bid(mcver::Block::Stone), 0);
    scene.place(0, 64, 3, bid(mcver::Block::Rail), 0);
    entity::BoatSystem boats{99};
    CHECK(boats.place(scene.w(), 0, 63, 0));
    MinecartSystem carts{11};
    CHECK(carts.place(scene.w(), 0, 64, 3, entity::MinecartType::Rideable));
    ArrowSystem arrows{4242};

    CHECK(arrows.shoot(scene.w(), 0.5, boats[0].y + 0.3, -3.0, 0.0f, 0.0f));
    arrows.tick(scene.w(), entity::ArrowTargets{nullptr, &boats, &carts});
    arrows.tick(scene.w(), entity::ArrowTargets{nullptr, &boats, &carts});
    CHECK_EQ(arrows.count(), 0);
    CHECK_EQ(boats[0].damage, 40);
    CHECK_EQ(carts[0].damage, 0);
}

TEST(a_right_click_with_a_bow_reaches_the_pool)
{
    // The seam: the bow is an `onItemRightClick`, so it goes through
    // `item::useItem` and not through the block path -- and without the pool
    // the click does nothing at all, which is the reported bug.
    Range range;

    item::Effects none;
    item::ItemUse used = item::useItem(range.w(), bowItem(), 0.0, 70.0, 0.0, 0.0, 0.0, 1.0,
                                       none);
    CHECK(!used.changed);
    CHECK_EQ(range.arrows.count(), 0);

    item::Effects effects;
    effects.entities.arrows = &range.arrows;
    used = item::useItem(range.w(), bowItem(), 0.0, 70.0, 0.0, 0.0, 0.0, 1.0, effects);
    CHECK(used.changed);
    CHECK_EQ(range.arrows.count(), 1);
    // The bow is not spent and does not become anything else.
    CHECK_EQ(int(used.becomes), int(bowItem()));
    // ...and it flew where it was aimed.
    CHECK(range.arrows[0].motionZ > 1.4);
}

TEST(the_arrow_meshes_into_six_quads)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));

    mesh::DetailVertex verts[render::kArrowMaxVertices];
    const int written = render::buildArrows(range.arrows, 0.0, 70.0, 0.0, 0.0f, verts,
                                            render::kArrowMaxVertices);
    CHECK_EQ(written, render::kArrowVerticesEach);

    // The whole arrow fits inside a block: the fins span 16 model units at
    // 0.05625, which is 0.9 of a block.
    double lo[3] = {1e30, 1e30, 1e30};
    double hi[3] = {-1e30, -1e30, -1e30};
    for (int i = 0; i < written; ++i) {
        const double p[3] = {double(verts[i].x) / double(mesh::kDetailUnitsPerBlock),
                             double(verts[i].y) / double(mesh::kDetailUnitsPerBlock),
                             double(verts[i].z) / double(mesh::kDetailUnitsPerBlock)};
        for (int a = 0; a < 3; ++a) {
            lo[a] = p[a] < lo[a] ? p[a] : lo[a];
            hi[a] = p[a] > hi[a] ? p[a] : hi[a];
        }
    }
    for (int a = 0; a < 3; ++a) {
        CHECK(hi[a] - lo[a] <= 1.0);
    }
    // ...and it is longest along the axis it is flying down, which is +Z here.
    CHECK(hi[2] - lo[2] > hi[0] - lo[0]);
    CHECK(hi[2] - lo[2] > hi[1] - lo[1]);
}

TEST(every_arrow_uv_lands_inside_the_arrow_page)
{
    Range range;
    CHECK(range.arrows.shoot(range.w(), 0.0, 70.0, 0.0, 0.0f, 0.0f));

    mesh::DetailVertex verts[render::kArrowMaxVertices];
    const int written = render::buildArrows(range.arrows, 0.0, 70.0, 0.0, 0.0f, verts,
                                            render::kArrowMaxVertices);
    CHECK(written > 0);

    int ox = 0, oy = 0;
    texture::skinOrigin(texture::EntitySkin::Arrow, &ox, &oy);
    for (int i = 0; i < written; ++i) {
        const double u = double(verts[i].u) * texture::kEntitySheetWidth
                         / double(mesh::kUvUnitsPerAtlas);
        const double v = double(verts[i].v) * texture::kEntitySheetHeight
                         / double(mesh::kUvUnitsPerAtlas);
        // The arrow's page is 32 x 32, not the 64 x 32 a box model uses.
        CHECK(u >= double(ox) - 0.01);
        CHECK(u <= double(ox) + 32.0 + 0.01);
        CHECK(v >= double(oy) - 0.01);
        CHECK(v <= double(oy) + 32.0 + 0.01);
    }
}
