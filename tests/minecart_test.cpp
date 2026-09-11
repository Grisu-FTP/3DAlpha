// The minecart -- `oc` (EntityMinecart) and `jo.a(...)` (ItemMinecart).
//
// "Boats and minecarts don't work (not even placeable)" was two bugs for the
// cart. The `spawns` bug is one; the other is that **`ItemMinecart.onItemUse`
// tests the clicked block against `Block.rails` and returns false for anything
// else** -- so a cart put on the ground places nothing, correctly, and half the
// report is a description of the game.
//
// The rail physics is the interesting part and it is exactly checkable, because
// none of it is random: a cart is snapped onto the track's centreline every
// tick, its speed is re-pointed rather than re-computed, and its height is
// looked up rather than integrated.

#include "core/block/registry.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/minecart_mesh.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Minecart;
using mc::entity::MinecartSystem;
using mc::entity::MinecartType;
using mc::entity::VehicleRider;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

item::ItemId minecartItem(int variant)
{
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Minecart
            && int(def.spawnVariant) == variant) {
            return item::ItemId(id);
        }
    }
    return 0;
}

// A straight track along z at y = 64, on a stone bed.
struct Track {
    SceneWorld scene{0, 0};
    MinecartSystem carts{2024};

    Track()
    {
        for (i32 z = -40; z <= 55; ++z) {
            for (i32 x = -2; x <= 2; ++x) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
            // Metadata 0 is flat along z -- the shapes are core/tick/rail.hpp's.
            scene.place(0, 64, z, bid(mcver::Block::Rail), 0);
        }
    }

    tick::TickWorld& w() { return scene.w(); }

    void run(int ticks, const VehicleRider& rider = VehicleRider{})
    {
        for (int i = 0; i < ticks; ++i) {
            carts.tick(w(), rider);
        }
    }
};

}  // namespace

TEST(three_items_spawn_minecarts_and_each_names_its_own_type)
{
    // The plain cart, the chest cart and the furnace cart, told apart by
    // `ItemMinecart`'s own `a` field rather than by their names.
    int found = 0;
    bool seen[3] = {false, false, false};
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.spawns == item::SpawnsEntity::Minecart) {
            ++found;
            CHECK(int(def.spawnVariant) < 3);
            seen[def.spawnVariant] = true;
        }
    }
    CHECK_EQ(found, 3);
    CHECK(seen[0] && seen[1] && seen[2]);
}

TEST(a_minecart_goes_on_rails_and_nowhere_else)
{
    Track track;
    // On the rail: yes.
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    CHECK_EQ(track.carts.count(), 1);
    // On the stone beside it: no, and this is the game's own rule.
    CHECK(!track.carts.place(track.w(), 1, 63, 0, MinecartType::Rideable));
    // In mid-air: no.
    CHECK(!track.carts.place(track.w(), 0, 70, 0, MinecartType::Rideable));
    CHECK_EQ(track.carts.count(), 1);
}

TEST(a_cart_pushed_along_a_straight_track_stays_on_it)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));

    // A shove along the track.
    const_cast<Minecart&>(track.carts[0]).motionZ = 0.3;
    track.run(60);

    const Minecart& c = track.carts[0];
    // **Never leaves the centreline**, because the position is assigned from
    // the rail rather than steered towards it.
    CHECK(std::fabs(c.x - 0.5) < 1e-9);
    CHECK(c.z > 2.0);
    CHECK(c.onRail);
}

TEST(an_empty_cart_stops_and_an_occupied_one_keeps_going)
{
    // 0.96 against 0.997 a tick. Over sixty ticks that is a factor of about
    // eleven in the speed left, and it is the difference between a cart that
    // coasts across a map and one that stops in a few blocks.
    Track empty;
    CHECK(empty.carts.place(empty.w(), 0, 64, 0, MinecartType::Rideable));
    const_cast<Minecart&>(empty.carts[0]).motionZ = 0.3;
    empty.run(60);

    Track ridden;
    CHECK(ridden.carts.place(ridden.w(), 0, 64, 0, MinecartType::Rideable));
    CHECK(ridden.carts.mount(0));
    const_cast<Minecart&>(ridden.carts[0]).motionZ = 0.3;
    VehicleRider rider;
    rider.present = true;
    ridden.run(60, rider);

    CHECK(std::fabs(ridden.carts[0].motionZ) > std::fabs(empty.carts[0].motionZ) * 3.0);
}

TEST(a_moving_cart_pushes_the_cart_on_the_next_rail)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    CHECK(track.carts.place(track.w(), 0, 64, 1, MinecartType::Rideable));

    const_cast<Minecart&>(track.carts[0]).motionZ = 0.3;
    track.carts.tick(track.w(), VehicleRider{});

    // A cart immediately behind another is inside EntityMinecart's expanded
    // collision query.  The rear cart gives up speed and the front one gains
    // it, which is the rail-to-rail shove players use to start a train.
    CHECK(track.carts[0].motionZ < 0.3);
    CHECK(track.carts[1].motionZ > 0.0);
}

TEST(a_stack_of_carts_stays_finite_however_long_it_runs)
{
    // "Stacking too many minecarts" turned them flat or made them vanish, sent
    // a rider to a world that neither drew nor collided, and left a save that
    // would not open. Every collision leaves a pair with 1.2 times its motion,
    // so carts kept in contact compound it until the double overflows and the
    // position goes NaN. A loop keeps the stack together for as long as it
    // runs. Carts placed on one rail share a point, which the collision skips;
    // one of them moving is what sets the rest off.
    SceneWorld scene{0, 0};
    const i32 lo = -10;
    const i32 hi = 10;
    for (i32 z = lo - 1; z <= hi + 1; ++z) {
        for (i32 x = lo - 1; x <= hi + 1; ++x) {
            scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
        }
    }
    for (i32 v = lo + 1; v < hi; ++v) {
        scene.place(v, 64, lo, bid(mcver::Block::Rail), 1);
        scene.place(v, 64, hi, bid(mcver::Block::Rail), 1);
        scene.place(lo, 64, v, bid(mcver::Block::Rail), 0);
        scene.place(hi, 64, v, bid(mcver::Block::Rail), 0);
    }
    scene.place(lo, 64, lo, bid(mcver::Block::Rail), 6);
    scene.place(hi, 64, lo, bid(mcver::Block::Rail), 7);
    scene.place(hi, 64, hi, bid(mcver::Block::Rail), 8);
    scene.place(lo, 64, hi, bid(mcver::Block::Rail), 9);

    MinecartSystem carts{2024};
    for (int i = 0; i < 8; ++i) {
        CHECK(carts.place(scene.w(), lo, 64, 0, MinecartType::Rideable));
    }
    CHECK(carts.mount(0));
    const_cast<Minecart&>(carts[0]).motionZ = 0.3;
    VehicleRider rider;
    rider.present = true;

    bool finite = true;
    double largest = 0.0;
    for (int t = 0; t < 4000 && finite; ++t) {
        carts.tick(scene.w(), rider);
        for (int i = 0; i < carts.count(); ++i) {
            const Minecart& c = carts[i];
            finite = finite && std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z)
                     && std::isfinite(c.motionX) && std::isfinite(c.motionZ)
                     && std::isfinite(c.yaw);
            largest = std::fmax(largest, std::fmax(std::fabs(c.motionX), std::fabs(c.motionZ)));
        }
    }
    CHECK(finite);
    // Bounded, not pinned to the limit: the rail re-points a pair's combined
    // speed onto one axis, which can put it back over by up to a root two
    // until the next collision clamps it again.
    CHECK(largest < 2.0 * entity::kMinecartMotionLimit);
    // The rider's seat is the cart's position, so it is the rider's too.
    CHECK(std::isfinite(carts.seat().x) && std::isfinite(carts.seat().z));
    // Still going round: the bound stops the overflow, not the ride.
    CHECK(carts[0].onRail);
}

TEST(a_player_body_pushes_an_unridden_cart)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    const AABB player{0.2, 64.0, -0.1, 0.8, 65.8, 0.5};
    double motionX = 0.0;
    double motionZ = 0.0;

    track.carts.collideWithPlayer(player, 0.5, 0.2, &motionX, &motionZ);

    CHECK(motionZ < 0.0);
    CHECK(track.carts[0].motionZ > 0.0);
}

TEST(a_cart_takes_a_corner_without_losing_its_speed)
{
    // The speed is re-pointed, not re-computed: the magnitude of the motion is
    // kept and only its direction is replaced. A corner that slowed a cart down
    // would mean that step had been read as a projection.
    SceneWorld scene{0, 0};
    for (i32 x = -6; x <= 6; ++x) {
        for (i32 z = -6; z <= 6; ++z) {
            scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
        }
    }
    // A track running along z that turns towards +x at the origin.
    for (i32 z = -6; z < 0; ++z) {
        scene.place(0, 64, z, bid(mcver::Block::Rail), 0);
    }
    // Shape 9 joins +x and -z.
    scene.place(0, 64, 0, bid(mcver::Block::Rail), 9);
    for (i32 x = 1; x <= 6; ++x) {
        scene.place(x, 64, 0, bid(mcver::Block::Rail), 1);
    }

    MinecartSystem carts{11};
    CHECK(carts.place(scene.w(), 0, 64, -5, MinecartType::Rideable));
    const_cast<Minecart&>(carts[0]).motionZ = 0.3;
    const double before =
        std::sqrt(carts[0].motionX * carts[0].motionX + carts[0].motionZ * carts[0].motionZ);

    for (int i = 0; i < 40 && carts[0].x < 2.0; ++i) {
        carts.tick(scene.w(), VehicleRider{});
    }

    // It got round the corner...
    CHECK(carts[0].x > 1.0);
    // ...and the drag is the only thing that took anything off it. Sixty ticks
    // of 0.96 would be a factor of ten; forty ticks leaves well over a tenth.
    const double after =
        std::sqrt(carts[0].motionX * carts[0].motionX + carts[0].motionZ * carts[0].motionZ);
    CHECK(after > before * 0.1);
}

TEST(a_cart_left_alone_on_a_slope_rolls_down_it)
{
    SceneWorld scene{0, 0};
    for (i32 x = -8; x <= 8; ++x) {
        for (i32 z = -8; z <= 8; ++z) {
            for (int y = 60; y <= 68; ++y) {
                scene.place(x, y, z, bid(mcver::Block::Stone), 0);
            }
        }
    }
    // A staircase of ascending rails along x: shape 2 ascends towards +x.
    for (int step = 0; step < 6; ++step) {
        const i32 x = i32(step);
        const int y = 64 + step;
        scene.place(x, y, 0, bid(mcver::Block::Air), 0);
        scene.place(x, y, 0, bid(mcver::Block::Rail), 2);
        // Clear the air above so the cart has room.
        for (int above = 1; above <= 3; ++above) {
            scene.place(x, y + above, 0, bid(mcver::Block::Air), 0);
        }
    }

    MinecartSystem carts{5};
    CHECK(carts.place(scene.w(), 3, 67, 0, MinecartType::Rideable));
    const double startX = carts[0].x;
    for (int i = 0; i < 80; ++i) {
        carts.tick(scene.w(), VehicleRider{});
    }
    // The slope push is constant and points down the hill -- towards -x for a
    // rail that ascends towards +x.
    CHECK(carts[0].x < startX);
}

TEST(only_the_plain_cart_can_be_ridden)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Chest));
    CHECK(track.carts.place(track.w(), 0, 64, 4, MinecartType::Furnace));
    CHECK(track.carts.place(track.w(), 0, 64, 8, MinecartType::Rideable));

    // `interact` mounts for type 0, opens a chest for 1 and takes coal for 2 --
    // and the last two have nowhere to go in this build.
    CHECK(!track.carts.mount(0));
    CHECK(!track.carts.mount(1));
    CHECK(track.carts.mount(2));
    CHECK_EQ(track.carts.riddenIndex(), 2);
}

TEST(the_seat_is_under_the_cart_by_three_tenths)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    CHECK(!track.carts.seat().valid);
    CHECK(track.carts.mount(0));

    const entity::RiderSeat seat = track.carts.seat();
    CHECK(seat.valid);
    CHECK(std::fabs(seat.y - (track.carts[0].y + entity::kMinecartMountedYOffset)) < 1e-12);
}

TEST(the_rail_point_is_the_middle_of_a_flat_rail)
{
    Track track;
    // A flat rail along z: the track's centreline runs down x = 0.5 and the
    // surface is the block's own y.
    const entity::RailPoint p =
        MinecartSystem::railPointAt(track.w(), 0.7, 64.3, 3.4);
    CHECK(p.valid);
    CHECK(std::fabs(p.x - 0.5) < 1e-9);
    CHECK(std::fabs(p.z - 3.4) < 1e-9);
    CHECK(std::fabs(p.y - 64.5) < 1e-9);

    // Off the track there is no point at all, which is the original's null.
    CHECK(!MinecartSystem::railPointAt(track.w(), 8.5, 64.3, 3.4).valid);
}

TEST(a_rail_one_block_below_still_counts)
{
    // The rule that lets a cart run off the top of a slope onto the flat rail
    // beyond without ever being airborne.
    Track track;
    CHECK(MinecartSystem::railPointAt(track.w(), 0.5, 65.1, 0.5).valid);
}

TEST(there_is_no_minecart_limit)
{
    // Every placement on the track succeeds, where the fixed pool refused past
    // thirty-two; and they still tick, pushing each other along.
    Track track;
    const int many = MinecartSystem::kInitialCapacity * 2;
    int placed = 0;
    for (int i = 0; i < many; ++i) {
        placed += track.carts.place(track.w(), 0, 64, i32(i), MinecartType::Rideable) ? 1 : 0;
    }
    CHECK_EQ(int(track.carts.refused()), 0);
    CHECK_EQ(track.carts.count(), placed);
    CHECK(placed > MinecartSystem::kInitialCapacity);
    VehicleRider none;
    for (int t = 0; t < 20; ++t) {
        track.carts.tick(track.w(), none);
    }
    CHECK_EQ(track.carts.count(), placed);
}

TEST(a_right_click_with_a_minecart_reaches_the_pool)
{
    Track track;
    entity::RayHit hit;
    hit.hit = true;
    hit.x = 0;
    hit.y = 64;
    hit.z = 0;
    hit.face = mesh::Face(1);

    item::Effects none;
    CHECK(!item::rightClick(track.w(), minecartItem(0), hit, kNoPlayer, 0.0f, none));
    CHECK_EQ(track.carts.count(), 0);

    item::Effects effects;
    effects.entities.minecarts = &track.carts;
    CHECK(item::rightClick(track.w(), minecartItem(0), hit, kNoPlayer, 0.0f, effects));
    CHECK_EQ(track.carts.count(), 1);
    CHECK(track.carts[0].type == MinecartType::Rideable);

    // ...and a furnace cart item makes a furnace cart, which is the
    // `spawnVariant` column doing its job.
    entity::RayHit other = hit;
    other.z = 6;
    CHECK(item::rightClick(track.w(), minecartItem(2), other, kNoPlayer, 0.0f, effects));
    CHECK(track.carts[1].type == MinecartType::Furnace);
}

TEST(a_right_click_off_the_rails_places_nothing)
{
    Track track;
    entity::RayHit hit;
    hit.hit = true;
    hit.x = 1;
    hit.y = 63;  // the stone bed beside the track
    hit.z = 0;
    hit.face = mesh::Face(1);

    item::Effects effects;
    effects.entities.minecarts = &track.carts;
    CHECK(!item::rightClick(track.w(), minecartItem(0), hit, kNoPlayer, 0.0f, effects));
    CHECK_EQ(track.carts.count(), 0);
}

TEST(the_minecart_meshes_into_six_boxes)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));

    mesh::DetailVertex verts[render::kMinecartMaxVertices];
    const int written = render::buildMinecarts(track.carts, track.w(), 0.0, 64.0, 0.0, 0.0f,
                                               verts, render::kMinecartMaxVertices);
    CHECK_EQ(written, render::kMinecartVerticesEach);

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
    // A cart is about a block and a quarter long and a block wide.
    CHECK(hi[0] - lo[0] > 0.8);
    CHECK(hi[0] - lo[0] < 2.0);
    CHECK(hi[1] - lo[1] < 1.0);
}

TEST(every_minecart_uv_lands_inside_the_cart_page)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));

    mesh::DetailVertex verts[render::kMinecartMaxVertices];
    const int written = render::buildMinecarts(track.carts, track.w(), 0.0, 64.0, 0.0, 0.0f,
                                               verts, render::kMinecartMaxVertices);
    CHECK(written > 0);

    // **This is the model that needs the clamp.** The cart's underside plate is
    // 18 x 14 x 1 at texture offset (44, 10), and its unwrap wants 38 texels of
    // width from u 44 -- eighteen past the edge of a 64-wide page. The original
    // wraps those onto another part of `cart.png` and never shows the faces
    // anyway; here the pages share a sheet, so `buildBox` clamps rather than
    // letting them bleed into the arrow's page. See core/render/box_model.cpp.
    int ox = 0, oy = 0;
    texture::skinOrigin(texture::EntitySkin::Minecart, &ox, &oy);
    for (int i = 0; i < written; ++i) {
        const double u = double(verts[i].u) * texture::kEntitySheetWidth
                         / double(mesh::kUvUnitsPerAtlas);
        const double v = double(verts[i].v) * texture::kEntitySheetHeight
                         / double(mesh::kUvUnitsPerAtlas);
        CHECK(u >= double(ox) - 0.01);
        CHECK(u <= double(ox) + double(texture::kSkinPageWidth) + 0.01);
        CHECK(v >= double(oy) - 0.01);
        CHECK(v <= double(oy) + double(texture::kSkinPageHeight) + 0.01);
    }
}

// **The link the two tests above skip: the crosshair has to find the rail.**
//
// `a_right_click_with_a_minecart_reaches_the_pool` hands `item::rightClick` a
// `RayHit` built by hand, so it proves the item path and says nothing about
// whether the ray that feeds it in the game ever lands on a rail. A rail's
// selection box is an eighth of a block tall and sits on the cell floor -- by
// some distance the thinnest thing in a1.1.2's selection table -- so it is
// exactly the shape a ray walk can step over. This is the whole chain the
// shoulder button runs: look at a rail, trace, place what the trace found.
TEST(the_crosshair_finds_a_rail_and_the_cart_goes_on_it)
{
    Track track;

    // Standing on the stone beside the track, eye height above the rail, aimed
    // down and across at the rail two blocks away -- an ordinary placing shot.
    const double eyeX = 2.5;
    const double eyeY = 64.0 + 1.62;
    const double eyeZ = 0.5;
    double dirX = 0.5 - eyeX;
    double dirY = 64.0625 - eyeY;
    double dirZ = 0.0;
    const double length = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
    dirX /= length;
    dirY /= length;
    dirZ /= length;

    const entity::RayHit hit =
        entity::rayTrace(track.w(), eyeX, eyeY, eyeZ, dirX, dirY, dirZ);
    CHECK(hit.hit);
    CHECK_EQ(int(hit.x), 0);
    CHECK_EQ(int(hit.y), 64);
    CHECK_EQ(int(hit.z), 0);
    CHECK_EQ(int(track.w().blockAt(hit.x, hit.y, hit.z)), int(bid(mcver::Block::Rail)));

    item::Effects effects;
    effects.entities.minecarts = &track.carts;
    CHECK(item::rightClick(track.w(), minecartItem(0), hit, kNoPlayer, 0.0f, effects));
    CHECK_EQ(track.carts.count(), 1);
}

// ---------------------------------------------------------------------------
// Under the crosshair -- `getMouseOver`'s entity half
// ---------------------------------------------------------------------------

TEST(a_cart_is_picked_through_a_tenth_of_a_border_and_no_more)
{
    // `boundingBox.expand(0.1F)` in `iq.a(F)`. The crosshair used to borrow the
    // arrow's 0.3, which made a cart grab rays that passed well clear of it.
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    item::EntityPools pools;
    pools.minecarts = &track.carts;
    const AABB box = track.carts[0].box;
    const double eyeX = box.minX - 2.0;
    const double z = (box.minZ + box.maxZ) / 2.0;
    const entity::RayHit noBlock{};

    // Level shots along +x, two blocks short of the box.
    const item::EntityTarget through =
        item::pickEntity(pools, eyeX, box.maxY - 0.1, z, 1.0, 0.0, 0.0, noBlock);
    CHECK(through.kind == item::EntityTarget::Kind::Minecart);
    CHECK_EQ(through.index, 0);
    // The distance is to the grown face, through a float square root.
    CHECK(std::fabs(through.distance - (2.0 - entity::kEntityPickBorder)) < 1e-6);

    CHECK(item::pickEntity(pools, eyeX, box.maxY + 0.05, z, 1.0, 0.0, 0.0, noBlock).found());
    CHECK(!item::pickEntity(pools, eyeX, box.maxY + 0.2, z, 1.0, 0.0, 0.0, noBlock).found());
}

TEST(a_cart_is_under_the_crosshair_only_within_three_blocks)
{
    // `if (d1 > 3.0D) d1 = 3.0D`: the block ray reaches four, the entity pass
    // three.
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    item::EntityPools pools;
    pools.minecarts = &track.carts;
    const AABB box = track.carts[0].box;
    const double face = box.minX - entity::kEntityPickBorder;
    const double y = box.maxY - 0.1;
    const double z = (box.minZ + box.maxZ) / 2.0;
    const entity::RayHit noBlock{};

    CHECK(item::pickEntity(pools, face - 2.95, y, z, 1.0, 0.0, 0.0, noBlock).found());
    CHECK(!item::pickEntity(pools, face - 3.05, y, z, 1.0, 0.0, 0.0, noBlock).found());
    CHECK(entity::entityPickReach(face - 3.05, y, z, noBlock) == entity::kEntityReach);
}

TEST(a_cart_behind_the_block_the_crosshair_is_on_is_not_picked)
{
    // The entity segment ends at the block hit, so a wall between the eye and
    // the cart keeps the crosshair -- and the outline -- on the wall.
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    item::EntityPools pools;
    pools.minecarts = &track.carts;
    const AABB box = track.carts[0].box;
    const double eyeX = -2.5;
    const double eyeY = (box.minY + box.maxY) / 2.0;
    const double eyeZ = (box.minZ + box.maxZ) / 2.0;
    CHECK(eyeY > 64.125 && eyeY < 65.0);

    track.scene.place(-1, 64, 0, bid(mcver::Block::Stone), 0);
    const entity::RayHit wall = entity::rayTrace(track.w(), eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0);
    CHECK(wall.hit);
    CHECK_EQ(int(wall.x), -1);
    CHECK(!item::pickEntity(pools, eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0, wall).found());

    track.scene.place(-1, 64, 0, bid(mcver::Block::Air), 0);
    const entity::RayHit open = entity::rayTrace(track.w(), eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0);
    CHECK(!open.hit);
    CHECK(item::pickEntity(pools, eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0, open).found());
}

TEST(a_rail_goes_back_under_a_cart_that_has_lost_its_track)
{
    // The report: "impossible to place a rail below a minecart". A cart whose
    // rail is gone sits on the stone, filling the cell a new rail goes into.
    // Its top face is covered by the cart in a1.1.2 too, but the top of the
    // neighbour's side face is not -- the grown box stops 0.2 short of it.
    // Under the arrow's 0.3 it reached the top of the cell and there was no
    // shot left that did not climb into the cart.
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    track.scene.place(0, 64, 0, bid(mcver::Block::Air), 0);
    track.run(40);
    const AABB box = track.carts[0].box;
    CHECK(std::fabs(box.minY - 64.0) < 1e-6);
    CHECK(box.maxY + entity::kEntityPickBorder < 64.85);

    track.scene.place(1, 64, 0, bid(mcver::Block::Stone), 0);

    // Standing on the far side, eye height above the stone, aimed at the top of
    // the neighbour's -x face.
    const double eyeX = -1.5;
    const double eyeY = 64.0 + 1.62;
    const double eyeZ = 0.5;
    double dirX = 1.0 - eyeX;
    double dirY = 64.9 - eyeY;
    const double length = std::sqrt(dirX * dirX + dirY * dirY);
    dirX /= length;
    dirY /= length;

    const entity::RayHit hit = entity::rayTrace(track.w(), eyeX, eyeY, eyeZ, dirX, dirY, 0.0);
    CHECK(hit.hit);
    CHECK_EQ(int(hit.x), 1);
    CHECK_EQ(int(hit.face), int(mesh::kFaceNegX));

    item::Effects effects;
    effects.entities.minecarts = &track.carts;
    CHECK(!item::pickEntity(effects.entities, eyeX, eyeY, eyeZ, dirX, dirY, 0.0, hit).found());
    CHECK(item::rightClick(track.w(), item::ItemId(bid(mcver::Block::Rail)), hit, kNoPlayer,
                           0.0f, effects));
    CHECK_EQ(int(track.w().blockAt(0, 64, 0)), int(bid(mcver::Block::Rail)));
}

TEST(the_nearest_entity_wins_whichever_pool_it_is_in)
{
    // `getMouseOver` compares every candidate's distance. The pick used to ask
    // the boats before the carts and take the first, so a boat behind a cart
    // took the click.
    SceneWorld scene{0, 0};
    for (i32 x = -4; x <= 3; ++x) {
        scene.place(x, 62, 0, bid(mcver::Block::Stone), 0);
    }
    scene.place(0, 63, 0, bid(mcver::Block::Water), 0);
    scene.place(1, 63, 0, bid(mcver::Block::Water), 0);
    scene.place(-2, 63, 0, bid(mcver::Block::Stone), 0);
    scene.place(-2, 64, 0, bid(mcver::Block::Rail), 1);

    MinecartSystem carts{3};
    entity::BoatSystem boats{4};
    CHECK(carts.place(scene.w(), -2, 64, 0, MinecartType::Rideable));
    CHECK(boats.place(scene.w(), 0, 63, 0));
    item::EntityPools pools;
    pools.minecarts = &carts;
    pools.boats = &boats;

    // On the line through both centres, a block short of the cart's.
    const AABB c = carts[0].box;
    const AABB b = boats[0].box;
    const double cx = (c.minX + c.maxX) / 2.0, cy = (c.minY + c.maxY) / 2.0;
    const double bx = (b.minX + b.maxX) / 2.0, by = (b.minY + b.maxY) / 2.0;
    double dx = bx - cx;
    double dy = by - cy;
    const double length = std::sqrt(dx * dx + dy * dy);
    dx /= length;
    dy /= length;
    const double eyeX = cx - dx;
    const double eyeY = cy - dy;
    const double eyeZ = 0.5;

    const item::EntityTarget target =
        item::pickEntity(pools, eyeX, eyeY, eyeZ, dx, dy, 0.0, entity::RayHit{});
    CHECK(target.kind == item::EntityTarget::Kind::Minecart);

    // With the cart gone the boat behind it is the answer.
    pools.minecarts = nullptr;
    CHECK(item::pickEntity(pools, eyeX, eyeY, eyeZ, dx, dy, 0.0, entity::RayHit{}).kind
          == item::EntityTarget::Kind::Boat);
}

TEST(a_right_click_on_a_cart_mounts_it_and_a_chest_cart_refuses)
{
    Track track;
    CHECK(track.carts.place(track.w(), 0, 64, 0, MinecartType::Rideable));
    CHECK(track.carts.place(track.w(), 0, 64, 5, MinecartType::Chest));
    item::EntityPools pools;
    pools.minecarts = &track.carts;

    item::EntityTarget target;
    target.kind = item::EntityTarget::Kind::Minecart;
    target.index = 0;
    CHECK(item::interactWithEntity(target, pools));
    CHECK_EQ(track.carts.riddenIndex(), 0);
    target.index = 1;
    CHECK(!item::interactWithEntity(target, pools));
    CHECK_EQ(track.carts.riddenIndex(), 0);
}

// ---------------------------------------------------------------------------
// Breaking one
// ---------------------------------------------------------------------------

TEST(five_bare_handed_hits_break_a_cart_and_it_leaves_a_minecart)
{
    // `oc.a(Lkh;I)Z`, the same four lines the boat's has with a different tail.
    Track track;
    mc::test::DropCatcher caught;
    caught.watch(track.w());
    CHECK(track.carts.place(track.w(), 0, 64, 0, entity::MinecartType::Rideable));

    for (int hit = 0; hit < 4; ++hit) {
        CHECK(track.carts.attack(track.w(), 0, 1));
        CHECK_EQ(track.carts.count(), 1);
    }
    CHECK_EQ(int(caught.drops.size()), 0);
    CHECK(track.carts.attack(track.w(), 0, 1));
    CHECK_EQ(track.carts.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Minecart)), 1);
}

TEST(a_container_cart_leaves_the_container_as_well)
{
    // **The plain minecart plus the block it was carrying**, not items 342 and
    // 343 -- which is the one place a cart's type reaches the ground, and the
    // reason `MinecartType` is read here at all.
    struct Case {
        entity::MinecartType type;
        int extra;
    };
    const Case cases[] = {
        {entity::MinecartType::Rideable, 0},
        {entity::MinecartType::Chest, int(mcver::Block::Chest)},
        {entity::MinecartType::Furnace, int(mcver::Block::Furnace)},
    };

    for (const Case& c : cases) {
        Track track;
        mc::test::DropCatcher caught;
        caught.watch(track.w());
        CHECK(track.carts.place(track.w(), 0, 64, 0, c.type));
        CHECK(track.carts.attack(track.w(), 0, 5));
        CHECK_EQ(track.carts.count(), 0);
        CHECK_EQ(caught.countOf(u16(mcver::Item::Minecart)), 1);
        if (c.extra != 0) {
            CHECK_EQ(caught.countOf(u16(c.extra)), 1);
            CHECK_EQ(int(caught.drops.size()), 2);
        } else {
            CHECK_EQ(int(caught.drops.size()), 1);
        }
    }
}

TEST(an_arrow_alone_does_not_break_a_fresh_cart)
{
    // `attackEntityFrom(source, 4)` is 40, and the threshold is *past* 40. Two
    // arrows do it; one does not.
    Track track;
    mc::test::DropCatcher caught;
    caught.watch(track.w());
    CHECK(track.carts.place(track.w(), 0, 64, 0, entity::MinecartType::Rideable));
    CHECK(track.carts.hitByArrow(track.w(), 0));
    CHECK_EQ(track.carts.count(), 1);
    CHECK(track.carts.hitByArrow(track.w(), 0));
    CHECK_EQ(track.carts.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Item::Minecart)), 1);
}
