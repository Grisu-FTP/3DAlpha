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
#include "core/entity/player_body.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/arrow_mesh.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "low_heap.hpp"
#include "items.hpp"  // generated; see tools/configure.py
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

// A place to count `attackEntityFrom` against the player, which is all the
// arrow can do with it: this build routes the damage out through a function
// pointer because there is no player health in core.
namespace {
struct Shot {
    int hits = 0;
    static void sink(void* ctx, int amount, mc::entity::DamageSource, double, double)
    {
        Shot& self = *static_cast<Shot*>(ctx);
        self.hits += amount;
    }
};

// The player's own box, 0.6 x 1.8 with the feet on the floor, and the eye at
// `posY + 1.62` the way the game holds it.
AABB standing(double x, double z)
{
    return AABB{x - 0.3, 64.0, z - 0.3, x + 0.3, 65.8, z + 0.3};
}
}  // namespace

TEST(an_arrow_does_not_shoot_the_player_who_fired_it)
{
    // Reported as "a shot arrow hits the player who shot it", and it is the
    // same `entity != shootingEntity || ticksInAir >= 5` a skeleton needs:
    // the muzzle offset backs the arrow up 0.16 against a half-width of 0.3,
    // so it leaves from *inside* the shooter's box -- and the sweep grows a
    // target by another 0.3 before it asks. Without the grace the first tick's
    // segment starts inside the player and the shot lands on the shooter.
    Range range;
    Shot shot;
    entity::ArrowTargets hits;
    hits.playerPresent = true;
    hits.playerBox = standing(0.5, 0.5);
    hits.hurtPlayer = &Shot::sink;
    hits.hurtPlayerCtx = &shot;

    CHECK(range.arrows.shoot(range.w(), 0.5, 64.0 + 1.62, 0.5, 0.0f, 0.0f));
    for (int t = 0; t < 30 && range.arrows.count() > 0; ++t) {
        range.arrows.tick(range.w(), hits);
    }
    CHECK_EQ(shot.hits, 0);
}

TEST(an_arrow_that_comes_back_down_hits_the_player_who_fired_it)
{
    // The grace is five ticks, not an exemption: `ticksInAir >= 5` and the
    // player is a target like any other. Fired straight up, the arrow spends
    // its grace on the way out and is an ordinary hazard on the way back --
    // which is how an arrow fired at the sky lands on the archer.
    Range range;
    Shot shot;
    entity::ArrowTargets hits;
    hits.playerPresent = true;
    hits.playerBox = standing(0.5, 0.5);
    hits.hurtPlayer = &Shot::sink;
    hits.hurtPlayerCtx = &shot;

    // Pitch is negative for up: `motionY = -sin(pitch)`.
    CHECK(range.arrows.shoot(range.w(), 0.5, 64.0 + 1.62, 0.5, 0.0f, -90.0f));
    for (int t = 0; t < 400 && range.arrows.count() > 0; ++t) {
        range.arrows.tick(range.w(), hits);
    }
    CHECK_EQ(shot.hits, entity::kArrowDamage);
    CHECK_EQ(range.arrows.count(), 0);
}

TEST(a_skeletons_arrow_hits_the_player_at_once_and_owes_no_grace)
{
    // The grace belongs to whoever fired, and only to them: `entity !=
    // shootingEntity` is a reference comparison in the jar, so a skeleton's
    // arrow is an immediate hazard to the player it was aimed at. This is the
    // half that stops "the shooter is skipped" from becoming "the player is
    // skipped", which is what an exclusion written without the shooter's
    // identity would have been.
    Range range;
    Shot shot;
    entity::ArrowTargets hits;
    hits.playerPresent = true;
    hits.playerBox = standing(0.5, 0.5);
    hits.hurtPlayer = &Shot::sink;
    hits.hurtPlayerCtx = &shot;

    // Loosed from three blocks away, at the skeleton's own velocity, along +Z.
    CHECK(range.arrows.shootFrom(range.w(), 0.5, 64.0 + 1.0, -3.0, 0.0, 0.0, 1.0, 0.6f, 0.0f,
                                 entity::ArrowShooter::Skeleton));
    for (int t = 0; t < 10 && range.arrows.count() > 0; ++t) {
        range.arrows.tick(range.w(), hits);
    }
    CHECK_EQ(shot.hits, entity::kArrowDamage);
    CHECK_EQ(range.arrows.count(), 0);
}

TEST(an_arrow_fired_while_flying_does_not_die_on_the_player_who_fired_it)
{
    // Reported as "an arrow should hit paintings and cause them to pop off",
    // and the painting was never the problem: in Creative the arrow never got
    // there.
    //
    // The self-grace excluded the shooter **by place** -- any candidate still
    // standing over `(shooterX, shooterZ)` -- which holds for as long as the
    // shooter has not left its own footprint. A walking player has not. A
    // flying one has: `entity::kFlightSpeed` is 0.6 blocks a tick against a
    // half-width of 0.3, so one tick of flight carries the box clear of the
    // place the shot was recorded at. The exclusion then missed the player,
    // whose box the arrow is still inside on its first tick -- the muzzle is
    // 0.16 back and the sweep grows a target by 0.3 -- so the shot was spent on
    // its own archer at a distance of zero, every time, before it had gone
    // anywhere.
    //
    // Flying *forward*, which is what shooting something across a room while
    // flying towards it looks like.
    Range range;
    Shot shot;
    entity::ArrowTargets hits;
    hits.playerPresent = true;
    hits.hurtPlayer = &Shot::sink;
    hits.hurtPlayerCtx = &shot;

    const double fired = 0.5;
    hits.playerBox = standing(0.5, fired);
    CHECK(range.arrows.shoot(range.w(), 0.5, 64.0 + 1.62, fired, 0.0f, 0.0f));
    for (int t = 0; t < 30 && range.arrows.count() > 0; ++t) {
        // A tick of Creative flight along the shot, applied before the arrow
        // moves, as `runGame` does: the body is ticked above the arrows.
        hits.playerBox = standing(0.5, fired + entity::kFlightSpeed * double(t + 1));
        range.arrows.tick(range.w(), hits);
    }
    CHECK_EQ(shot.hits, 0);
}

TEST(an_arrow_shot_by_a_flying_player_still_knocks_the_painting_off_the_wall)
{
    // The report, end to end and in `runGame`'s own order: a painting on a
    // wall, a player flying towards it, the bow fired through `item::useItem`
    // -- and the painting on the floor as an item afterwards.
    //
    // Every piece of this worked on its own. What did not was the combination:
    // the arrow was spent on its own archer on the tick it was fired, so the
    // pool it would have swept was never reached.
    SceneWorld scene{0, 0};
    mc::test::DropCatcher caught;
    caught.watch(scene.w());
    for (i32 x = -4; x <= 4; ++x) {
        for (i32 z = -12; z <= 1; ++z) {
            scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
        }
    }
    for (i32 x = -4; x <= 4; ++x) {
        for (int y = 64; y <= 68; ++y) {
            scene.place(x, y, 0, bid(mcver::Block::Stone), 0);
        }
    }
    entity::PaintingSystem paintings{5};
    CHECK(paintings.place(scene.w(), 0, 66, 0, 2));
    ArrowSystem arrows{4242};

    entity::ArrowTargets hits;
    hits.paintings = &paintings;
    hits.playerPresent = true;
    hits.hurtPlayer = &Shot::sink;
    Shot shot;
    hits.hurtPlayerCtx = &shot;

    // Flying level with the painting, six blocks out, closing at 0.6 a tick.
    double z = -6.0;
    const double eyeY = 66.0 + 1.62;
    hits.playerBox = AABB{0.5 - 0.3, eyeY - 1.62, z - 0.3, 0.5 + 0.3, eyeY - 1.62 + 1.8, z + 0.3};

    item::Effects effects;
    effects.entities.arrows = &arrows;
    const double dy = paintings[0].y - eyeY;
    const double flat = paintings[0].z - z;
    const double length = std::sqrt(dy * dy + flat * flat);
    const item::ItemUse used = item::useItem(scene.w(), bowItem(), 0.5, eyeY, z, 0.0,
                                             dy / length, flat / length, effects);
    CHECK(used.changed);
    CHECK_EQ(arrows.count(), 1);

    for (int t = 0; t < 20 && paintings.count() > 0; ++t) {
        z += entity::kFlightSpeed;
        hits.playerBox =
            AABB{0.5 - 0.3, eyeY - 1.62, z - 0.3, 0.5 + 0.3, eyeY - 1.62 + 1.8, z + 0.3};
        paintings.tick(scene.w());
        arrows.tick(scene.w(), hits);
    }
    CHECK_EQ(shot.hits, 0);
    CHECK_EQ(paintings.count(), 0);
    CHECK_EQ(arrows.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Painting)), 1);
}

namespace {

// Shot straight down from above the floor, so it sticks where it can be
// reached, and ticked until it has. False if it was never fired or never
// landed.
bool landAtFeet(Range& range, bool bySkeleton = false)
{
    const bool fired =
        bySkeleton ? range.arrows.shootFrom(range.w(), 0.5, 66.0, 0.5, 0.0, -1.0, 0.0, 0.6f,
                                            0.0f, entity::ArrowShooter::Skeleton)
                   : range.arrows.shoot(range.w(), 0.5, 66.0, 0.5, 0.0f, 90.0f);
    if (!fired) {
        return false;
    }
    for (int t = 0; t < 40 && !range.arrows[0].inGround; ++t) {
        range.arrows.tick(range.w());
    }
    return range.arrows.count() == 1 && range.arrows[0].inGround;
}

AABB reachAround(const Arrow& a)
{
    return AABB{a.x - 1.3, a.y - 0.5, a.z - 1.3, a.x + 1.3, a.y + 1.3, a.z + 1.3};
}

int arrowsHeld(const item::Inventory& inventory)
{
    int n = 0;
    for (const auto& slot : inventory.main) {
        if (int(slot.id) == int(mcver::Item::Arrow)) {
            n += int(slot.count);
        }
    }
    return n;
}

}  // namespace

TEST(the_player_takes_back_their_own_arrow_once_it_stops_shaking)
{
    // `kg.b(dm)`: in the ground, fired by this player, `arrowShake <= 0`, and
    // one arrow into the inventory -- and then the arrow is gone.
    Range range;
    CHECK(landAtFeet(range));
    const Arrow& a = range.arrows[0];
    const AABB reach = reachAround(a);
    item::Inventory inventory;
    inventory.clear();

    // Still quivering from the impact: not yet.
    CHECK(a.shake > 0);
    CHECK_EQ(range.arrows.collect(reach, inventory), 0);

    range.run(entity::kArrowShake);
    CHECK_EQ(range.arrows[0].shake, 0);
    CHECK_EQ(range.arrows.collect(reach, inventory), 1);
    CHECK_EQ(range.arrows.count(), 0);
    CHECK_EQ(arrowsHeld(inventory), 1);
}

TEST(nobody_takes_an_arrow_they_did_not_fire)
{
    // A skeleton's arrow has a shooter, and it is not the player. An arrow
    // read back off the card has none at all -- `kg` does not save it -- which
    // is the same answer: `shooterPlayer` starts at nobody.
    Range range;
    CHECK(landAtFeet(range, true));
    range.run(entity::kArrowShake + 1);
    item::Inventory inventory;
    inventory.clear();
    CHECK_EQ(range.arrows.collect(reachAround(range.arrows[0]), inventory), 0);
    CHECK_EQ(range.arrows.count(), 1);
    CHECK_EQ(Arrow{}.shooterPlayer, 0);
}

TEST(a_full_inventory_leaves_the_arrow_where_it_is)
{
    Range range;
    CHECK(landAtFeet(range));
    range.run(entity::kArrowShake + 1);
    item::Inventory inventory;
    inventory.clear();
    for (int slot = 0; slot < int(sizeof(inventory.main) / sizeof(inventory.main[0])); ++slot) {
        inventory.set(slot, item::ItemId(mcver::Block::Stone), 64);
    }
    CHECK_EQ(range.arrows.collect(reachAround(range.arrows[0]), inventory), 0);
    CHECK_EQ(range.arrows.count(), 1);
}

namespace {

struct RemoteHits {
    std::vector<i32> victims;
    static void hurt(void* ctx, i32 victim, Arrow& arrow)
    {
        (void)arrow;
        static_cast<RemoteHits*>(ctx)->victims.push_back(victim);
    }
};

}  // namespace

TEST(a_hosts_arrow_strikes_a_guest_and_spares_the_guest_who_fired_it)
{
    // On a host, the guests are targets as the local player is -- struck by
    // the nearest box along the segment, and the one who fired it spared for
    // five ticks by identity. The blow is handed back, never resolved here.
    Range range;
    constexpr i32 kShooter = 2;
    constexpr i32 kVictim = 3;
    entity::RemoteTarget guests[2];
    guests[0].entityId = kShooter;
    guests[0].box = AABB{0.2, 64.0, 0.2, 0.8, 65.8, 0.8};   // the archer, at the muzzle
    guests[1].entityId = kVictim;
    guests[1].box = AABB{0.2, 64.0, 5.2, 0.8, 65.8, 5.8};   // five blocks down range
    RemoteHits hits;
    entity::ArrowTargets targets;
    targets.remotePlayers = guests;
    targets.remotePlayerCount = 2;
    targets.hurtRemote = &RemoteHits::hurt;
    targets.hurtRemoteCtx = &hits;

    CHECK(range.arrows.shoot(range.w(), 0.5, 65.62, 0.5, 0.0f, 0.0f, kShooter));
    for (int t = 0; t < 10 && range.arrows.count() > 0; ++t) {
        range.arrows.tick(range.w(), targets);
    }
    CHECK_EQ(int(hits.victims.size()), 1);
    CHECK_EQ(hits.victims[0], kVictim);
    CHECK_EQ(range.arrows.count(), 0);
}
