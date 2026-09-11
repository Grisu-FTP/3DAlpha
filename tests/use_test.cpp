// One right-click, from the crosshair to the world.
//
// The three things `item::rightClick` is -- `onPlayerRightClick`'s order,
// `ItemBlock.onItemUse` and `ItemDoor.onItemUse` -- and the two bugs that
// existed because the first and the third were missing: nothing could be opened
// and a door arrived as half a door, which the very next neighbour change
// deleted.
//
// The hit is built by hand rather than aimed, because what is under test is
// what happens *after* the ray -- tests/ray_trace_test.cpp is where the ray is
// checked. One case aims a real ray anyway, to keep the two ends joined.

#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/tick_world.hpp"
#include "core/entity/boat.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::entity::RayHit;
using mc::test::SceneWorld;
using mc::tick::TickWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

constexpr item::ItemId kWoodDoorItem = 324;
constexpr item::ItemId kIronDoorItem = 330;
constexpr item::ItemId kStoneItem = 1;
constexpr item::ItemId kEmptyHand = 0;
constexpr item::ItemId kFlintAndSteel = 259;

// Far enough away that no placement can ever be inside it.
const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

RayHit hitOn(i32 x, int y, i32 z, mesh::Face face)
{
    RayHit hit;
    hit.hit = true;
    hit.x = x;
    hit.y = y;
    hit.z = z;
    hit.face = face;
    return hit;
}

// A floor of stone at y = 63, so anything placed on it stands at 64.
struct Ground {
    SceneWorld scene{0, 0};

    Ground()
    {
        for (i32 x = -6; x <= 6; ++x) {
            for (i32 z = -6; z <= 6; ++z) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
    }

    TickWorld& w() { return scene.w(); }
    int id(i32 x, int y, i32 z) { return int(scene.w().blockAt(x, y, z)); }
    int md(i32 x, int y, i32 z) { return int(scene.w().dataAt(x, y, z)); }

    bool click(item::ItemId held, i32 x, int y, i32 z, mesh::Face face, float yaw = 0.0f)
    {
        return item::rightClick(scene.w(), held, hitOn(x, y, z, face), kNoPlayer, yaw);
    }

    // The item's own right-click, aimed straight down from an eye above the
    // cell. Straight down is the one direction with no rounding to argue
    // about: the ray enters the column at its centre and leaves through the
    // -Y face of whatever it stops on.
    item::ItemUse useDown(item::ItemId held, i32 x, int y, i32 z)
    {
        return item::useItem(scene.w(), held, double(x) + 0.5, double(y) + 3.5,
                             double(z) + 0.5, 0.0, -1.0, 0.0);
    }
};

// The four buckets, found by the generated column rather than written down.
// `bucket` is `ItemBucket.isFull`: 0 empty, a flowing fluid block id when full,
// -1 for milk. See core/item/item_def.hpp.
item::ItemId bucketWith(i16 fill)
{
    for (item::ItemId id = 0; id < item::ItemId(mcver::kItemTableSize); ++id) {
        const item::ItemDef& d = item::def(id);
        if (d.known && d.bucket != item::ItemDef::kNotABucket && d.bucket == fill) {
            return id;
        }
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// The door places two blocks
// ---------------------------------------------------------------------------

TEST(a_door_places_both_of_its_halves)
{
    Ground g;
    CHECK(g.click(kWoodDoorItem, 0, 63, 0, mesh::kFacePosY));

    CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::WoodenDoor));
    CHECK_EQ((long long) g.id(0, 65, 0), (long long) int(mcver::Block::WoodenDoor));
    // Bit 3 is the only difference between the two halves.
    CHECK_EQ((long long) g.md(0, 65, 0), (long long) (g.md(0, 64, 0) + 8));
}

TEST(a_door_goes_only_on_a_top_face)
{
    Ground g;
    g.scene.place(0, 64, 2, bid(mcver::Block::Stone), 0);
    for (mesh::Face face : {mesh::kFaceNegY, mesh::kFaceNegX, mesh::kFacePosX,
                            mesh::kFaceNegZ, mesh::kFacePosZ}) {
        CHECK(!g.click(kWoodDoorItem, 0, 64, 2, face));
    }
    CHECK_EQ((long long) g.id(0, 65, 2), 0LL);
}

TEST(a_doors_facing_comes_from_the_players_heading)
{
    // (yaw + 180) * 4 / 360, floored after subtracting a half, masked to two
    // bits -- and **every row below was read off a running jar**, by standing a
    // real `EntityPlayer` on a real floor at each heading and using a real
    // door item on it. Yaw 270 and yaw -90 are the same heading and the
    // original agrees they are, which is the row that catches a sign error in
    // the floor.
    const struct {
        float yaw;
        int facing;
    } cases[] = {{0.0f, 1},   {90.0f, 2},  {180.0f, 3}, {270.0f, 0},
                 {-90.0f, 0}, {45.0f, 2},  {135.0f, 3}};

    for (const auto& c : cases) {
        Ground g;
        CHECK(g.click(kWoodDoorItem, 0, 63, 0, mesh::kFacePosY, c.yaw));
        CHECK_EQ((long long) (g.md(0, 64, 0) & 3), (long long) c.facing);
        // Nothing beside it, so the hinge is not mirrored.
        CHECK_EQ((long long) (g.md(0, 64, 0) & 4), 0LL);
    }
}

TEST(a_door_beside_a_door_hangs_the_other_way)
{
    Ground g;
    CHECK(g.click(kWoodDoorItem, 0, 63, 0, mesh::kFacePosY, 0.0f));
    const int first = g.md(0, 64, 0);
    CHECK_EQ((long long) (first & 4), 0LL);

    // Facing 1 at yaw 0 runs the door along x with its hinge side at +x, so
    // the door that meets this one in the middle stands at -x. Finding a door
    // on the hinge side and none on the other is the first of the two mirror
    // conditions.
    CHECK(g.click(kWoodDoorItem, -1, 63, 0, mesh::kFacePosY, 0.0f));
    const int second = g.md(-1, 64, 0);
    CHECK_EQ((long long) (second & 4), 4LL);
    CHECK_EQ((long long) (second & 3), (long long) ((first - 1) & 3));
}

TEST(a_door_will_not_stand_without_a_floor_or_headroom)
{
    Ground g;
    // Nothing under it: the click lands on a block that is not there.
    CHECK(!g.click(kWoodDoorItem, 0, 70, 0, mesh::kFacePosY));

    // A ceiling one block up leaves no room for the upper half.
    g.scene.place(2, 65, 0, bid(mcver::Block::Stone), 0);
    CHECK(!g.click(kWoodDoorItem, 2, 63, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(2, 64, 0), 0LL);
}

// **The bug this was reported as.** A door placed as one block fails
// `onNeighborBlockChange`'s "is my other half still there" the first time
// anything beside it changes, and deletes itself.
TEST(a_placed_door_survives_a_neighbour_being_built)
{
    Ground g;
    CHECK(g.click(kWoodDoorItem, 0, 63, 0, mesh::kFacePosY));

    g.w().setBlockAndDataWithNotify(1, 64, 0, bid(mcver::Block::Stone), 0);
    g.w().setBlockAndDataWithNotify(0, 64, 1, bid(mcver::Block::Stone), 0);
    g.w().setBlockAndDataWithNotify(0, 65, 1, bid(mcver::Block::Stone), 0);

    CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::WoodenDoor));
    CHECK_EQ((long long) g.id(0, 65, 0), (long long) int(mcver::Block::WoodenDoor));
}

// ---------------------------------------------------------------------------
// The block is asked first
// ---------------------------------------------------------------------------

TEST(a_hand_opens_a_door_from_either_half)
{
    Ground g;
    CHECK(g.click(kWoodDoorItem, 0, 63, 0, mesh::kFacePosY));
    const int shut = g.md(0, 64, 0);

    CHECK(g.click(kEmptyHand, 0, 64, 0, mesh::kFacePosX));
    CHECK_EQ((long long) g.md(0, 64, 0), (long long) (shut ^ 4));
    CHECK_EQ((long long) g.md(0, 65, 0), (long long) ((shut ^ 4) + 8));

    // The upper half hands the click down, so it swings the same way.
    CHECK(g.click(kEmptyHand, 0, 65, 0, mesh::kFacePosX));
    CHECK_EQ((long long) g.md(0, 64, 0), (long long) shut);
    CHECK_EQ((long long) g.md(0, 65, 0), (long long) (shut + 8));
}

TEST(an_iron_door_eats_the_click_and_stays_shut)
{
    Ground g;
    CHECK(g.click(kIronDoorItem, 0, 63, 0, mesh::kFacePosY));
    const int shut = g.md(0, 64, 0);

    // True, because the click was consumed -- and nothing moved.
    CHECK(item::rightClick(g.w(), kStoneItem, hitOn(0, 64, 0, mesh::kFacePosX), kNoPlayer,
                           0.0f));
    CHECK_EQ((long long) g.md(0, 64, 0), (long long) shut);
    CHECK_EQ((long long) g.id(1, 64, 0), 0LL);
}

TEST(a_lever_flicks_instead_of_being_built_on)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    // Metadata 5: on the floor. Bit 3 clear, so it is off. **Which of the two
    // floor orientations it ends up on is not this test's to say** --
    // `onBlockAdded` rolls `5 + nextInt(2)` on any write, as the original does
    // -- so the flick is checked against what it actually landed on.
    g.scene.place(0, 65, 0, bid(mcver::Block::Lever), 5);
    const long long orientation = (long long) g.md(0, 65, 0) & 7;
    CHECK(orientation == 5 || orientation == 6);

    CHECK(g.click(kStoneItem, 0, 65, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.md(0, 65, 0), orientation + 8);
    // The stone in hand went nowhere: the click was spent on the lever.
    CHECK_EQ((long long) g.id(0, 66, 0), 0LL);

    CHECK(g.click(kStoneItem, 0, 65, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.md(0, 65, 0), orientation);
}

TEST(a_button_presses_and_pops_back_out)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    // Metadata 1: stuck to the wall at -x. Off.
    g.scene.place(1, 64, 0, bid(mcver::Block::StoneButton), 1);

    CHECK(g.click(kEmptyHand, 1, 64, 0, mesh::kFacePosX));
    CHECK_EQ((long long) g.md(1, 64, 0), 9LL);

    // Pressing it again is consumed and changes nothing -- no extension.
    CHECK(g.click(kEmptyHand, 1, 64, 0, mesh::kFacePosX));
    CHECK_EQ((long long) g.md(1, 64, 0), 9LL);

    // Twenty ticks later it is out again, which is `buttonTick` running off the
    // update the press scheduled.
    const TickWorld::Centre centre{0, 0};
    for (int i = 0; i < 20; ++i) {
        g.w().tick(&centre, 1, 0);
    }
    CHECK_EQ((long long) g.md(1, 64, 0), 1LL);
}

TEST(redstone_ore_lights_and_still_takes_the_block)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::RedstoneOre), 0);

    CHECK(g.click(kStoneItem, 0, 64, 0, mesh::kFacePosY));
    // Lit by the touch...
    CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::LitRedstoneOre));
    // ...and the click carried on to the hand, because that override returns
    // false where the door's returns true.
    CHECK_EQ((long long) g.id(0, 65, 0), (long long) int(mcver::Block::Stone));
}

// ---------------------------------------------------------------------------
// The ordinary placement, and the two rules it had lost
// ---------------------------------------------------------------------------

TEST(a_snow_layer_is_replaced_rather_than_built_on)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::SnowLayer), 0);

    CHECK(g.click(kStoneItem, 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::Stone));
    CHECK_EQ((long long) g.id(0, 65, 0), 0LL);
}

TEST(a_placement_is_refused_inside_the_player)
{
    Ground g;
    // Standing on the floor at (0, 64, 0), so the cell above the clicked block
    // is where the legs are.
    const AABB body{-0.3, 64.0, -0.3, 0.3, 65.8, 0.3};
    CHECK(!item::rightClick(g.w(), kStoneItem, hitOn(0, 63, 0, mesh::kFacePosY), body, 0.0f));
    CHECK_EQ((long long) g.id(0, 64, 0), 0LL);
}

TEST(a_ray_that_missed_places_nothing)
{
    Ground g;
    RayHit miss;
    CHECK(!item::rightClick(g.w(), kStoneItem, miss, kNoPlayer, 0.0f));
}

TEST(an_item_that_places_nothing_does_nothing)
{
    Ground g;
    constexpr item::ItemId kIronSword = 267;
    CHECK(!g.click(kIronSword, 0, 63, 0, mesh::kFacePosY));
    CHECK(!g.click(kEmptyHand, 0, 63, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(0, 64, 0), 0LL);
}

// ---------------------------------------------------------------------------
// Breaking
// ---------------------------------------------------------------------------

TEST(breaking_a_block_clears_it_and_leaves_a_cloud_behind)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Dirt), 0);

    entity::ParticleSystem fx(1);
    mc::item::Effects effects;
    effects.particles = &fx;

    CHECK(item::destroyBlock(g.w(), 0, 64, 0, effects));
    CHECK_EQ((long long) g.id(0, 64, 0), 0LL);

    // **Spawned from the block, before it was removed.** 64 of them, and each
    // carrying dirt's tile -- which is only possible because the order in
    // `onPlayerDestroyBlock` is particles, then removal, then sound.
    CHECK_EQ(fx.count(), 64);
    CHECK_EQ((long long) fx[0].tile,
             (long long) block::def(bid(mcver::Block::Dirt)).texture);
}

TEST(breaking_air_changes_nothing_and_spawns_nothing)
{
    Ground g;
    entity::ParticleSystem fx(1);
    mc::item::Effects effects;
    effects.particles = &fx;

    CHECK(!item::destroyBlock(g.w(), 0, 70, 0, effects));
    CHECK_EQ(fx.count(), 0);
}

TEST(breaking_works_with_no_effects_at_all)
{
    // The headless path: no particles, no sound, and the world still changes.
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    CHECK(item::destroyBlock(g.w(), 0, 64, 0));
    CHECK_EQ((long long) g.id(0, 64, 0), 0LL);
}

// The two ends joined: a real ray, aimed at the top of the floor, puts a door
// where the crosshair was.
TEST(a_door_placed_through_a_real_ray_trace)
{
    Ground g;
    const entity::RayHit hit =
        entity::rayTrace(g.w(), 0.5, 66.62, 0.5, 0.0, -1.0, 0.0);
    CHECK(hit.hit);
    CHECK(item::rightClick(g.w(), kWoodDoorItem, hit, kNoPlayer, 0.0f));
    CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::WoodenDoor));
    CHECK_EQ((long long) g.id(0, 65, 0), (long long) int(mcver::Block::WoodenDoor));
}


// ---------------------------------------------------------------------------
// The clearance test asks about the shape the block will actually have
// ---------------------------------------------------------------------------

TEST(a_ladder_goes_on_the_wall_you_are_standing_against)
{
    // **The bug this closes.** `canBlockBePlacedAt` tests the new block's
    // collision box against the player, and it used to ask for that box at
    // metadata 0 -- which for a ladder is outside the 2..5 the game writes, and
    // is the one metadata `collision.cpp` answers with a *full cube* for. A
    // full cube in the cell in front of you overlaps the body, so the click was
    // refused: a ladder could not be put on the wall you were standing against,
    // which is where a ladder goes.
    Ground g;
    for (int y = 64; y <= 67; ++y) {
        g.scene.place(0, y, 0, bid(mcver::Block::Stone), 0);
    }

    // The player standing in the cell at x = -1, feet on the floor at y = 64.
    const AABB player{-1.3, 64.0, -0.3, -0.7, 65.8, 0.3};
    const RayHit hit = hitOn(0, 65, 0, mesh::kFaceNegX);

    CHECK(item::rightClick(g.w(), 65, hit, player, 0.0f));
    CHECK_EQ(g.id(-1, 65, 0), int(mcver::Block::Ladder));
    // Metadata 4, which is the generated table's answer for the -x face: a
    // ladder hangs on the wall *behind* it, and the block that was clicked is
    // at +x from the cell the ladder landed in.
    CHECK_EQ(g.md(-1, 65, 0), 4);
}

TEST(a_full_cube_is_still_refused_inside_the_player)
{
    // The other half: the clearance test is not disabled, it is asked a better
    // question. A stone block placed into the cell the body is in is still
    // refused.
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Stone), 0);
    const AABB player{-1.3, 64.0, -0.3, -0.7, 65.8, 0.3};
    CHECK(!item::rightClick(g.w(), kStoneItem, hitOn(0, 64, 0, mesh::kFaceNegX), player,
                            0.0f));
    CHECK_EQ(g.id(-1, 64, 0), int(mcver::Block::Air));
}

// ---------------------------------------------------------------------------
// Flint and steel is not an ItemBlock
// ---------------------------------------------------------------------------

TEST(flint_and_steel_lights_the_cell_beside_the_face_it_struck)
{
    Ground g;
    for (int y = 64; y <= 66; ++y) {
        g.scene.place(0, y, 0, bid(mcver::Block::Planks), 0);
    }
    CHECK(g.click(kFlintAndSteel, 0, 65, 0, mesh::kFaceNegX));
    CHECK_EQ(g.id(-1, 65, 0), int(mcver::Block::Fire));
}

TEST(flint_and_steel_only_lights_air)
{
    // `nx.a` tests `getBlockId == 0` and nothing else -- unlike `ItemBlock`,
    // which treats water, lava and a snow layer as free space. So a puddle
    // cannot be lit, and the click is still spent.
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Planks), 0);
    g.scene.place(-1, 64, 0, bid(mcver::Block::Water), 0);

    CHECK(g.click(kFlintAndSteel, 0, 64, 0, mesh::kFaceNegX));
    CHECK(g.id(-1, 64, 0) != int(mcver::Block::Fire));
}

TEST(flint_and_steel_is_spent_even_where_nothing_catches)
{
    // It returns true whatever happens -- the original damages the tool on
    // every use -- and a fire that cannot stay is removed by `onBlockAdded` on
    // the same call rather than refused before it.
    Ground g;
    g.scene.place(0, 70, 0, bid(mcver::Block::Stone), 0);
    CHECK(g.click(kFlintAndSteel, 0, 70, 0, mesh::kFacePosY));
    // Solid ground under it, so it survives its first tick; the point is only
    // that the click was taken.
    CHECK_EQ(g.id(0, 71, 0), int(mcver::Block::Fire));

    // And in mid air with nothing to burn, the fire goes on the way in.
    CHECK(g.click(kFlintAndSteel, 0, 70, 0, mesh::kFaceNegX));
    CHECK_EQ(g.id(-1, 70, 0), int(mcver::Block::Air));
}

// ---------------------------------------------------------------------------
// The bucket
// ---------------------------------------------------------------------------
//
// `ac.a(Lev;Lcn;Ldm;)Lev;` -- ItemBucket.onItemRightClick, which is the first
// thing in this build to go down `Item.onItemRightClick` rather than
// `onItemUse`, and the first to change what is in the hand. See
// core/item/use.cpp.

TEST(an_empty_bucket_fills_from_a_source_and_takes_the_block_away)
{
    Ground g;
    const item::ItemId empty = bucketWith(0);
    CHECK(empty != 0);

    const int water = int(mcver::Block::Water);
    g.scene.place(0, 64, 0, BlockId(water), 0);

    const item::ItemUse used = g.useDown(empty, 0, 64, 0);
    CHECK(used.changed);
    CHECK_EQ(g.id(0, 64, 0), 0);
    // It became the bucket whose column names a block of the same material,
    // which is the comparison the original makes.
    const item::ItemDef& becameDef = item::def(used.becomes);
    CHECK(becameDef.bucket > 0);
    CHECK_EQ(int(block::def(BlockId(becameDef.bucket)).material),
             int(block::def(BlockId(water)).material));
}

TEST(an_empty_bucket_does_not_fill_from_flowing_water)
{
    // `canCollideCheck(meta, true)` is `hitLiquids && metadata == 0`, so the ray
    // does not even stop on a stream -- it carries on to the floor, which is
    // not something a bucket can take. This is the whole reason the targetable
    // column had to become a per-metadata mask.
    Ground g;
    const item::ItemId empty = bucketWith(0);
    g.scene.place(0, 64, 0, BlockId(int(mcver::Block::Water)), 1);

    const item::ItemUse used = g.useDown(empty, 0, 64, 0);
    CHECK(!used.changed);
    CHECK_EQ(int(used.becomes), int(empty));
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Water));
}

TEST(a_full_bucket_pours_the_flowing_block_and_comes_back_empty)
{
    Ground g;
    const item::ItemId empty = bucketWith(0);
    // Whichever fluid the table offers first; the rule is what is under test.
    item::ItemId full = 0;
    for (item::ItemId id = 0; id < item::ItemId(mcver::kItemTableSize) && full == 0; ++id) {
        const item::ItemDef& d = item::def(id);
        if (d.known && d.bucket > 0) {
            full = id;
        }
    }
    CHECK(full != 0);

    // Aimed down at the floor: the ray stops on the stone and the fluid goes
    // into the cell above it.
    const item::ItemUse used = g.useDown(full, 0, 63, 0);
    CHECK(used.changed);
    CHECK_EQ(g.id(0, 64, 0), int(item::def(full).bucket));
    CHECK_EQ(g.md(0, 64, 0), 0);
    CHECK_EQ(int(used.becomes), int(empty));
}

TEST(a_full_bucket_refuses_a_solid_cell_and_stays_full)
{
    // **Getting this branch to fire takes an eye inside a block, and that is a
    // fact about the geometry rather than a contrivance.** The pour goes into
    // the cell on the struck face -- the side the ray came *from* -- and a ray
    // can only stop on a targetable block, so in the ordinary case the cell it
    // came from is one it has just travelled through and therefore is not
    // solid. The exception is the first cell, which `rayTrace` never tests
    // (core/entity/ray_trace.hpp): a player with their head in a block is
    // aiming out of a solid cell, and that is the one way to aim at a face
    // whose near side is stone. Without the check, that click writes water into
    // the block the player is standing in.
    Ground g;
    item::ItemId full = 0;
    for (item::ItemId id = 0; id < item::ItemId(mcver::kItemTableSize) && full == 0; ++id) {
        if (item::def(id).known && item::def(id).bucket > 0) {
            full = id;
        }
    }
    CHECK(full != 0);

    const BlockId stone = BlockId(int(mcver::Block::Stone));
    g.scene.place(0, 65, 0, stone, 0);
    g.scene.place(0, 66, 0, stone, 0);

    // The eye is inside the block at y = 66, looking down at the one below it.
    const item::ItemUse used = item::useItem(g.w(), full, 0.5, 66.5, 0.5, 0.0, -1.0, 0.0);
    CHECK(!used.changed);
    CHECK_EQ(int(used.becomes), int(full));
    CHECK_EQ(g.id(0, 66, 0), int(mcver::Block::Stone));
}

TEST(milk_empties_itself_and_leaves_the_world_alone)
{
    Ground g;
    const item::ItemId milk = bucketWith(-1);
    CHECK(milk != 0);
    const item::ItemId empty = bucketWith(0);

    const item::ItemUse used = g.useDown(milk, 0, 63, 0);
    CHECK(!used.changed);
    CHECK_EQ(int(used.becomes), int(empty));
    CHECK_EQ(g.id(0, 64, 0), 0);
}

TEST(an_item_that_is_not_a_bucket_does_nothing_and_keeps_its_place)
{
    // The negative control the `useItem` seam needs: it is reached on **every**
    // right-click that the block path declined, so anything that answers it
    // with a world change would fire constantly.
    Ground g;
    const item::ItemUse used = g.useDown(kStoneItem, 0, 63, 0);
    CHECK(!used.changed);
    CHECK_EQ(int(used.becomes), int(kStoneItem));
    CHECK_EQ(g.id(0, 64, 0), 0);

    const item::ItemUse none = g.useDown(kEmptyHand, 0, 63, 0);
    CHECK(!none.changed);
}

// ---------------------------------------------------------------------------
// The left click's other half
// ---------------------------------------------------------------------------

TEST(a_boat_under_the_crosshair_is_hit_instead_of_the_block_behind_it)
{
    // `Minecraft.clickMouse`'s precedence, which was missing entirely: with no
    // entity branch on the break button there was no way to damage a boat, a
    // cart or a painting, so none of them could ever leave anything behind.
    SceneWorld scene{0, 0};
    for (i32 x = -4; x <= 4; ++x) {
        for (i32 z = -4; z <= 4; ++z) {
            scene.place(x, 62, z, bid(mcver::Block::Stone), 0);
            scene.place(x, 63, z, bid(mcver::Block::Water), 0);
        }
    }
    mc::test::DropCatcher caught;
    caught.watch(scene.w());

    entity::BoatSystem boats{7};
    CHECK(boats.place(scene.w(), 0, 63, 0));

    mc::item::Effects effects;
    effects.entities.boats = &boats;

    // Looking along +x at the boat, from three blocks back and at its height.
    const double eyeX = -3.0;
    const double eyeY = boats[0].y;
    const double eyeZ = boats[0].z;
    const auto aim = [&] {
        const entity::RayHit block =
            entity::rayTrace(scene.w(), eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0);
        return mc::item::pickEntity(effects.entities, eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0, block);
    };
    for (int hit = 0; hit < 4; ++hit) {
        CHECK(aim().kind == mc::item::EntityTarget::Kind::Boat);
        CHECK(mc::item::attackEntity(scene.w(), aim(), effects));
        CHECK_EQ(boats.count(), 1);
    }
    CHECK(mc::item::attackEntity(scene.w(), aim(), effects));
    CHECK_EQ(boats.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);

    // And with the boat gone the same click finds nothing, which is what tells
    // the caller to break the block behind it instead.
    CHECK(!aim().found());
    CHECK(!mc::item::attackEntity(scene.w(), aim(), effects));
}

TEST(a_left_click_with_no_pools_wired_hits_nothing)
{
    // Every headless caller is in this state, and it must not be a crash and
    // must not be a hit: `attackEntity` with an empty `EntityPools` is the same
    // "nowhere to put it" answer the spawn side already gives.
    SceneWorld scene{0, 0};
    scene.place(0, 63, 0, bid(mcver::Block::Stone), 0);
    const entity::RayHit block = entity::rayTrace(scene.w(), 0.5, 64.5, -3.0, 0.0, 0.0, 1.0);
    const mc::item::EntityTarget target =
        mc::item::pickEntity(mc::item::EntityPools{}, 0.5, 64.5, -3.0, 0.0, 0.0, 1.0, block);
    CHECK(!target.found());
    CHECK(!mc::item::attackEntity(scene.w(), target, mc::item::Effects{}));
    CHECK(!mc::item::interactWithEntity(target, mc::item::EntityPools{}));
}
