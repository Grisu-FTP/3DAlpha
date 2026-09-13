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
#include "core/util/java_random.hpp"
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
        CHECK(mc::item::attackEntity(scene.w(), aim(), kEmptyHand, effects));
        CHECK_EQ(boats.count(), 1);
    }
    CHECK(mc::item::attackEntity(scene.w(), aim(), kEmptyHand, effects));
    CHECK_EQ(boats.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);

    // And with the boat gone the same click finds nothing, which is what tells
    // the caller to break the block behind it instead.
    CHECK(!aim().found());
    CHECK(!mc::item::attackEntity(scene.w(), aim(), kEmptyHand, effects));
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
    CHECK(!mc::item::attackEntity(scene.w(), target, kEmptyHand, mc::item::Effects{}));
    CHECK(!mc::item::interactWithEntity(scene.w(), target, mc::item::EntityPools{}, 0).taken);
}

// ---------------------------------------------------------------------------
// The hoe
// ---------------------------------------------------------------------------

namespace {

// The five hoes, found by the generated column rather than written down --
// `ItemDef::tills` is `instanceof ItemHoe`, asked of the jar. See item_def.hpp.
item::ItemId firstHoe()
{
    for (item::ItemId id = 0; id < item::ItemId(mcver::kItemTableSize); ++id) {
        if (item::def(id).known && item::def(id).tills) {
            return id;
        }
    }
    return 0;
}

int hoeCount()
{
    int n = 0;
    for (item::ItemId id = 0; id < item::ItemId(mcver::kItemTableSize); ++id) {
        if (item::def(id).known && item::def(id).tills) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST(a_hoe_turns_grass_and_dirt_into_farmland)
{
    // All five of them, and each on both grounds: the behaviour is the class's
    // and none of the five overrides any part of it.
    CHECK_EQ(hoeCount(), 5);
    for (item::ItemId hoe = 0; hoe < item::ItemId(mcver::kItemTableSize); ++hoe) {
        if (!item::def(hoe).known || !item::def(hoe).tills) {
            continue;
        }
        Ground g;
        g.scene.place(0, 64, 0, bid(mcver::Block::Grass), 0);
        g.scene.place(2, 64, 0, bid(mcver::Block::Dirt), 0);
        CHECK(g.click(hoe, 0, 64, 0, mesh::kFacePosY));
        CHECK(g.click(hoe, 2, 64, 0, mesh::kFacePosY));
        CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::Farmland));
        CHECK_EQ((long long) g.id(2, 64, 0), (long long) int(mcver::Block::Farmland));
    }
}

TEST(a_hoe_refuses_anything_that_is_not_grass_or_dirt)
{
    Ground g;
    const item::ItemId hoe = firstHoe();
    // The stone floor itself, and sand -- which is neither of the two ids the
    // method names, however much it looks like ground.
    g.scene.place(2, 64, 0, bid(mcver::Block::Sand), 0);
    CHECK(!g.click(hoe, 0, 63, 0, mesh::kFacePosY));
    CHECK(!g.click(hoe, 2, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(0, 63, 0), (long long) int(mcver::Block::Stone));
    CHECK_EQ((long long) g.id(2, 64, 0), (long long) int(mcver::Block::Sand));
}

TEST(a_solid_block_stops_grass_from_being_tilled_and_not_dirt)
{
    // `(above.isSolid() || id != grass) && id != dirt` -- the cover is only
    // ever asked about on the grass branch, so **dirt under stone still
    // tills** and grass under stone does not. Getting the precedence wrong
    // reads the same on grass and differs here.
    Ground g;
    const item::ItemId hoe = firstHoe();
    g.scene.place(0, 64, 0, bid(mcver::Block::Grass), 0);
    g.scene.place(0, 65, 0, bid(mcver::Block::Stone), 0);
    g.scene.place(2, 64, 0, bid(mcver::Block::Dirt), 0);
    g.scene.place(2, 65, 0, bid(mcver::Block::Stone), 0);

    CHECK(!g.click(hoe, 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::Grass));
    CHECK(g.click(hoe, 2, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(2, 64, 0), (long long) int(mcver::Block::Farmland));

    // And a torch overhead is not solid, so it stops nothing.
    g.scene.place(4, 64, 0, bid(mcver::Block::Grass), 0);
    g.scene.place(4, 65, 0, bid(mcver::Block::Torch), 5);
    CHECK(g.click(hoe, 4, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(4, 64, 0), (long long) int(mcver::Block::Farmland));
}

TEST(a_hoe_tills_the_cell_it_struck_from_any_face)
{
    // The face parameter is never read: the struck cell is what changes, so a
    // click on the underside or the side of a dirt block tills *that* block
    // and not the cell the face points into. This is the difference between
    // the hoe and every ItemBlock, and it is why `places` cannot describe it.
    const item::ItemId hoe = firstHoe();
    for (mesh::Face face : {mesh::kFaceNegY, mesh::kFaceNegX, mesh::kFacePosX,
                            mesh::kFaceNegZ, mesh::kFacePosZ, mesh::kFacePosY}) {
        Ground g;
        g.scene.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
        CHECK(g.click(hoe, 0, 64, 0, face));
        CHECK_EQ((long long) g.id(0, 64, 0), (long long) int(mcver::Block::Farmland));
        // Nothing landed in the cell the face points into.
        CHECK_EQ((long long) g.id(0, 65, 0), 0LL);
        CHECK_EQ((long long) g.id(0, 63, 0), (long long) int(mcver::Block::Stone));
    }
}

TEST(hoeing_grass_drops_a_seed_one_time_in_eight)
{
    // `if (world.rand.nextInt(8) != 0) return true;` and then one seed. The
    // roll is driven rather than sampled: the world's own generator is the one
    // the method draws from, so seeding it decides the outcome.
    Ground g;
    mc::test::DropCatcher caught;
    caught.watch(g.w());
    const item::ItemId hoe = firstHoe();

    // Find a seed whose first draw is the winning one, and one whose is not.
    // **The seeds are spread rather than counted up**: `setSeed` only xors, so
    // consecutive small seeds share their whole high word and `nextInt(8)` --
    // which for a power of two is the top three bits -- answers the same
    // number for every one of them. A stride large enough to move the high
    // bits is what makes this a search instead of a loop over one answer.
    constexpr i64 kStride = 2654435761LL;
    JavaRandom probe{0};
    i64 winner = -1;
    i64 loser = -1;
    for (i64 n = 1; n < 64 && (winner < 0 || loser < 0); ++n) {
        const i64 s = n * kStride;
        probe.setSeed(s);
        if (probe.nextInt(8) == 0) {
            if (winner < 0) {
                winner = s;
            }
        } else if (loser < 0) {
            loser = s;
        }
    }
    CHECK(winner >= 0);
    CHECK(loser >= 0);

    g.scene.place(0, 64, 0, bid(mcver::Block::Grass), 0);
    g.w().random().setSeed(loser);
    CHECK(g.click(hoe, 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ(caught.total(), 0);

    g.scene.place(2, 64, 0, bid(mcver::Block::Grass), 0);
    g.w().random().setSeed(winner);
    CHECK(g.click(hoe, 2, 64, 0, mesh::kFacePosY));
    CHECK_EQ(caught.countOf(u16(mcver::Item::Seeds)), 1);

    // `float f2 = 1.2F` is a constant and not a draw, so the seed rises to a
    // fixed height above the cell while x and z scatter over the middle 70 %.
    const mc::test::Drop& d = caught.drops.back();
    CHECK_EQ(d.y, double(float(64) + 1.2f));
    CHECK(d.x > 2.0 && d.x < 3.0);
    CHECK(d.z > 0.0 && d.z < 1.0);
}

TEST(hoeing_dirt_costs_the_roll_and_drops_nothing)
{
    // The `nextInt(8)` happens before the block is compared against grass, so
    // tilling dirt spends a draw on a seed it can never get. A build that
    // skipped it would leave the world's random one step behind, which every
    // later random tick would read.
    Ground g;
    mc::test::DropCatcher caught;
    caught.watch(g.w());
    const item::ItemId hoe = firstHoe();

    g.scene.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    g.w().random().setSeed(4321);
    CHECK(g.click(hoe, 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ(caught.total(), 0);

    JavaRandom expected{0};
    expected.setSeed(4321);
    expected.nextInt(8);
    CHECK_EQ(g.w().random().nextInt(1 << 20), expected.nextInt(1 << 20));
}

// ---------------------------------------------------------------------------
// What is in the hand decides how hard the click lands
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The seed, which is the one placement in a1.1.2 that asks nothing
// ---------------------------------------------------------------------------

TEST(a_seed_plants_a_crop_on_the_farmland_it_struck)
{
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Farmland), 0);
    CHECK(g.click(item::ItemId(mcver::Item::Seeds), 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(0, 65, 0), (long long) int(mcver::Block::Wheat));

    // `ItemSeeds.onItemUse` is the top face and the top face only, and it is
    // the *struck* block that has to be farmland -- not the one under it.
    Ground side;
    side.scene.place(0, 64, 0, bid(mcver::Block::Farmland), 0);
    CHECK(!side.click(item::ItemId(mcver::Item::Seeds), 0, 64, 0, mesh::kFacePosX));
    CHECK(!side.click(item::ItemId(mcver::Item::Seeds), 0, 63, 0, mesh::kFacePosY));
    CHECK_EQ((long long) side.id(0, 65, 0), 0LL);
}

TEST(a_seed_planted_under_bedrock_breaks_the_bedrock)
{
    // **a1.1.2's own bug, kept.** `ItemSeeds.onItemUse` writes the crop into
    // the cell above the farmland with no air test, no `canPlaceBlockAt` and
    // no `canBlockBePlacedAt` -- the only placement in the game that asks
    // nothing at all. Farmland is fifteen sixteenths tall, so its top face is
    // still clickable through the gap under a block sitting on it, and the
    // crop replaces whatever that block is. Bedrock is the one worth naming:
    // nothing else in the game removes it.
    Ground g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Farmland), 0);
    g.scene.place(0, 65, 0, bid(mcver::Block::Bedrock), 0);
    CHECK(g.click(item::ItemId(mcver::Item::Seeds), 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) g.id(0, 65, 0), (long long) int(mcver::Block::Wheat));

    // It is not bedrock-specific, and it is not a hole in the block table: any
    // block over farmland goes the same way.
    Ground stone;
    stone.scene.place(0, 64, 0, bid(mcver::Block::Farmland), 0);
    stone.scene.place(0, 65, 0, bid(mcver::Block::Stone), 0);
    CHECK(stone.click(item::ItemId(mcver::Item::Seeds), 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) stone.id(0, 65, 0), (long long) int(mcver::Block::Wheat));

    // The ordinary placement path still refuses what it always refused: the
    // hole is in the seed, not in `canBePlacedAt`.
    Ground blocked;
    blocked.scene.place(0, 64, 0, bid(mcver::Block::Farmland), 0);
    blocked.scene.place(0, 65, 0, bid(mcver::Block::Bedrock), 0);
    CHECK(!blocked.click(kStoneItem, 0, 64, 0, mesh::kFacePosY));
    CHECK_EQ((long long) blocked.id(0, 65, 0), (long long) int(mcver::Block::Bedrock));
}

TEST(a_sword_hits_harder_than_a_fist)
{
    // `getDamageVsEntity`, straight out of the jar and into the table: an
    // empty hand and anything ordinary answer 1, `ItemTool` answers
    // material + kind, `ItemSword` answers 4 + material * 2. **Gold sits
    // beside wood**, which is the row a name-based guess gets wrong.
    CHECK_EQ(int(item::def(kEmptyHand).damageVsEntity), 1);
    CHECK_EQ(int(item::def(kStoneItem).damageVsEntity), 1);

    const struct {
        mcver::Item item;
        int damage;
    } kRows[] = {
        {mcver::Item::WoodenSword, 4},   {mcver::Item::GoldenSword, 4},
        {mcver::Item::StoneSword, 6},    {mcver::Item::IronSword, 8},
        {mcver::Item::DiamondSword, 10}, {mcver::Item::WoodenShovel, 1},
        {mcver::Item::DiamondAxe, 6},    {mcver::Item::WoodenHoe, 1},
    };
    for (const auto& row : kRows) {
        CHECK_EQ(int(item::def(item::ItemId(row.item)).damageVsEntity), row.damage);
    }
}

TEST(a_boat_goes_down_in_one_hit_from_a_diamond_sword)
{
    // Five punches or one sword: the boat's health is the same, and the only
    // thing that changed is the number `attackEntity` passes it. Compare with
    // `a_punched_boat_breaks_and_leaves_planks` above, which is the same scene
    // with a bare hand.
    SceneWorld scene{0, 0};
    mc::test::DropCatcher caught;
    caught.watch(scene.w());
    entity::BoatSystem boats{1234};
    CHECK(boats.place(scene.w(), 0.5, 64.0, 0.5));

    mc::item::Effects effects;
    effects.entities.boats = &boats;

    const double eyeX = -3.0;
    const double eyeY = boats[0].y;
    const double eyeZ = boats[0].z;
    const entity::RayHit block =
        entity::rayTrace(scene.w(), eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0);
    const mc::item::EntityTarget target =
        mc::item::pickEntity(effects.entities, eyeX, eyeY, eyeZ, 1.0, 0.0, 0.0, block);
    CHECK(target.kind == mc::item::EntityTarget::Kind::Boat);

    CHECK(mc::item::attackEntity(scene.w(), target,
                                 item::ItemId(mcver::Item::DiamondSword), effects));
    CHECK_EQ(boats.count(), 0);
    CHECK_EQ(caught.countOf(u16(mcver::Block::Planks)), entity::kBoatPlanksDropped);
}
