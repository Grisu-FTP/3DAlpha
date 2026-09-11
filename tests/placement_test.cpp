// Which way a block ends up facing when a player puts it down.
//
// Checked against a real jar for every block crossed with every face of the
// block clicked. The surprise the fixture settled is that **the player's
// heading never matters at all**: a1.1.2 has no `Block.onBlockPlacedBy`. The
// one thing in the game that does read a heading is `ItemDoor.onItemUse`, and
// it reads it in the *item*; core/item/use.cpp carries that and this does not.
//
// Nor does the struck face decide as much as the sweep makes it look. It sees
// onBlockAdded and onBlockPlaced together against one stone cube, and only six
// blocks have an onBlockPlaced at all. A furnace and a staircase turn from
// their **neighbours** in onBlockAdded, and with the stone as the only
// neighbour that reads exactly like "the face you clicked". So the table is
// checked for the six, and the whole placement path -- table, then
// `tick::blockAdded` -- is checked for everything, which is the claim that
// matters and the one that would have caught this.
//
// The fixture said "only the lever" until the sweep itself was fixed. It was
// calling `Block.onBlockClicked` after each placement -- the *left* button's
// method, not a placement's -- so every block it put down was then punched,
// which flips a lever, opens a door, presses a button and lights redstone ore.
// Four rows of the shipped table were that punch rather than the placement.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/mesh/vertex.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "placement_vectors.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::block::placementMetadata;
using mc::block::TickBehaviour;

namespace {

bool hasPlacedHook(int id)
{
    for (int i = 0; i < test::kPlacementHookCount; ++i) {
        if (test::kPlacementHooks[i] == id) {
            return true;
        }
    }
    return false;
}

// Far enough away that no placement can ever be inside it.
const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

// The sweep's own scene: a stone cube with air two blocks round it.
constexpr i32 kCx = 8;
constexpr int kCy = 70;
constexpr i32 kCz = 8;

void resetScene(test::SceneWorld& scene)
{
    for (i32 dx = -3; dx <= 3; ++dx) {
        for (int dy = -3; dy <= 3; ++dy) {
            for (i32 dz = -3; dz <= 3; ++dz) {
                scene.place(kCx + dx, kCy + dy, kCz + dz, block::kAir, 0);
            }
        }
    }
    scene.place(kCx, kCy, kCz, BlockId(mcver::Block::Stone), 0);
}

entity::RayHit hitOn(i32 x, int y, i32 z, int face)
{
    entity::RayHit hit;
    hit.hit = true;
    hit.x = x;
    hit.y = y;
    hit.z = z;
    hit.face = mesh::Face(face);
    return hit;
}

}  // namespace

TEST(the_table_is_on_block_placed_and_only_six_blocks_have_one)
{
    // Measured of the classes, not of the results: torch, ladder, lever, both
    // redstone torches and the button. The furnace and both staircases vary
    // by face in the sweep and are **not** in it.
    CHECK_EQ(test::kPlacementHookCount, 6);
    CHECK(!hasPlacedHook(int(mcver::Block::Furnace)));
    CHECK(!hasPlacedHook(int(mcver::Block::WoodenStairs)));
    CHECK(!hasPlacedHook(int(mcver::Block::CobblestoneStairs)));

    for (int i = 0; i < test::kPlacementCaseCount; ++i) {
        const test::PlacementCase& c = test::kPlacementCases[i];
        for (int face = 0; face < 6; ++face) {
            // Heading 0, which is what the shipped table holds; no heading
            // ever differs.
            const int swept = c.metadata[face][0];
            // A block without onBlockPlaced lands with metadata 0 and keeps
            // whatever onBlockAdded makes of it. -1 means the block deleted
            // itself there; the table stores 0, because a placement our code
            // refuses never reaches it.
            const int want = hasPlacedHook(c.id) && swept > 0 ? swept : 0;
            CHECK_EQ(int(placementMetadata(BlockId(c.id), face)), want);
        }
    }
}

TEST(a_placed_block_faces_the_way_the_jar_says_for_every_block_and_face)
{
    // The sweep's own sequence, in the sweep's own scene: the write with
    // notification -- which runs `tick::blockAdded` -- carrying the table's
    // metadata. The sweep calls setBlockWithNotify and onBlockPlaced
    // directly, so it skips ItemBlock's canPlaceBlockAt exactly as this does
    // (a sapling on stone is refused by `item::rightClick` and kept by the
    // jar here). What lands has to be what the jar landed, whichever of the
    // table and the tick behaviour produced it.
    test::SceneWorld scene(0, 0);
    int checked = 0;
    for (int i = 0; i < test::kPlacementCaseCount; ++i) {
        const test::PlacementCase& c = test::kPlacementCases[i];
        const BlockId placed = BlockId(c.id);
        for (int face = 0; face < 6; ++face) {
            const int want = c.metadata[face][0];
            if (want < 0) {
                // The block deleted itself in the jar. Most of those are
                // refused before the write here, which this does not model;
                // the staircase under a block is checked on its own below.
                continue;
            }
            resetScene(scene);
            const entity::RayHit hit = hitOn(kCx, kCy, kCz, face);
            const i32 x = hit.placeX();
            const int y = hit.placeY();
            const i32 z = hit.placeZ();
            scene.w().setBlockAndDataWithNotify(x, y, z, placed, placementMetadata(placed, face));
            CHECK_EQ(int(scene.w().blockAt(x, y, z)), c.id);
            const int got = int(scene.w().dataAt(x, y, z));
            if (block::def(placed).tick == TickBehaviour::Lever && (want == 5 || want == 6)) {
                // A floor lever's `5 + nextInt(2)` comes off World.rand, which
                // the sweep reseeds and this world does not. Either roll.
                CHECK(got == 5 || got == 6);
            } else {
                CHECK_EQ(got, want);
            }
            ++checked;
        }
    }
    CHECK(checked > 300);
}

TEST(no_block_orients_from_the_players_heading)
{
    // The measured claim, asserted so it cannot quietly stop being true: **no**
    // block in a1.1.2 answers differently for different player headings, across
    // sixteen of them and every face.
    CHECK_EQ(test::kPlacementYawVaryingCount, 0);
    // And a good dozen vary with the face in the sweep -- six through
    // onBlockPlaced, the rest through onBlockAdded seeing the one stone -- so
    // the fixture is not vacuous.
    CHECK(test::kPlacementSideVaryingCount >= 10);
}

TEST(a_lever_is_placed_off_and_a_door_is_placed_shut)
{
    // The four rows the sweep's stray punch used to corrupt, spelled out so a
    // regression in the generator is a failing test rather than a lever that
    // arrives already switched on.
    //
    // Bit 3 is "on" for a lever and "pressed" for a button; bit 2 is "open" for
    // a door. None of them is set by putting the thing down.
    const BlockId lever = BlockId(mcver::Block::Lever);
    for (int face = 1; face <= 5; ++face) {
        CHECK_EQ(int(placementMetadata(lever, face)) & 8, 0);
    }
    // A floor lever, from the top face: orientation 5 or 6, never a wall.
    const int floorLever = int(placementMetadata(lever, mesh::kFacePosY));
    CHECK(floorLever == 5 || floorLever == 6);

    for (int face = 0; face < 6; ++face) {
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::StoneButton), face)) & 8, 0);
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::WoodenDoor), face)), 0);
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::RedstoneOre), face)), 0);
    }
}

TEST(a_torch_takes_the_wall_it_was_clicked_onto)
{
    // The visible case, spelled out rather than left inside the sweep. The
    // metadata numbers are the game's own and come from the fixture above.
    const BlockId torch = BlockId(mcver::Block::Torch);
    CHECK_EQ(int(placementMetadata(torch, mesh::kFacePosY)), 5);   // stood on a floor
    CHECK(placementMetadata(torch, mesh::kFaceNegZ) != 5);         // hung on a wall
    CHECK(placementMetadata(torch, mesh::kFacePosX)
          != placementMetadata(torch, mesh::kFaceNegX));           // and on which wall
}

TEST(a_block_that_does_not_care_gets_metadata_zero)
{
    for (int face = 0; face < 6; ++face) {
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::Stone), face)), 0);
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::Dirt), face)), 0);
    }
    // And an id this build has never heard of does not read past the table.
    CHECK_EQ(int(placementMetadata(BlockId(mcver::kPlacementTableSize - 1), 0)), 0);
    CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::Stone), 99)), 0);
}

// ---- the furnace and the staircase: neighbours, not the face -----------
//
// Everything below is a case the one-stone sweep cannot show, because each
// has more than one neighbour. The rules are `ku.h` and `km.h`, transcribed in
// core/tick/behaviour.cpp.

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A floor of stone at y = 63, so anything placed on it stands at 64.
struct Floor {
    test::SceneWorld scene{0, 0};

    Floor()
    {
        for (i32 x = -4; x <= 4; ++x) {
            for (i32 z = -4; z <= 4; ++z) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
    }

    void wall(i32 x, int y, i32 z) { scene.place(x, y, z, bid(mcver::Block::Stone), 0); }

    // The block's own item, clicked onto a face -- the whole right-click.
    bool click(mcver::Block held, i32 x, int y, i32 z, int face)
    {
        return item::rightClick(scene.w(), item::ItemId(held), hitOn(x, y, z, face), kNoPlayer,
                                0.0f);
    }

    // Onto the floor's top face, so the block lands at (x, 64, z).
    bool putDown(mcver::Block held, i32 x, i32 z) { return click(held, x, 63, z, mesh::kFacePosY); }

    int id(i32 x, int y, i32 z) { return int(scene.w().blockAt(x, y, z)); }
    int md(i32 x, int y, i32 z) { return int(scene.w().dataAt(x, y, z)); }
};

}  // namespace

TEST(a_furnace_turns_its_mouth_away_from_a_wall)
{
    // Out in the open it faces +Z, which is metadata 3 -- the jar's default.
    {
        Floor f;
        CHECK(f.putDown(mcver::Block::Furnace, 0, 0));
        CHECK_EQ(f.md(0, 64, 0), 3);
    }
    // One wall: the mouth is on the opposite side. Metadata names the face the
    // mouth is on, in mc::mesh::Face order.
    const struct {
        i32 dx, dz;
        int facing;
    } walls[] = {{0, 1, 2}, {0, -1, 3}, {1, 0, 4}, {-1, 0, 5}};
    for (const auto& w : walls) {
        Floor f;
        f.wall(w.dx, 64, w.dz);
        CHECK(f.putDown(mcver::Block::Furnace, 0, 0));
        CHECK_EQ(f.md(0, 64, 0), w.facing);
    }
}

TEST(a_furnace_ignores_the_face_it_was_clicked_onto)
{
    // Clicked onto the -Z face of a wall, with another wall behind it. The old
    // face table said 2 (mouth on -Z, into the second wall); a1.1.2 sees walls
    // on both Z sides, lets neither rule fire, and keeps its default.
    Floor f;
    f.wall(0, 64, 1);
    f.wall(0, 64, -1);
    CHECK(f.click(mcver::Block::Furnace, 0, 64, 1, mesh::kFaceNegZ));
    CHECK_EQ(f.id(0, 64, 0), int(mcver::Block::Furnace));
    CHECK_EQ(f.md(0, 64, 0), 3);

    // In a corner the X wall is asked last and wins.
    Floor corner;
    corner.wall(0, 64, -1);
    corner.wall(-1, 64, 0);
    CHECK(corner.putDown(mcver::Block::Furnace, 0, 0));
    CHECK_EQ(corner.md(0, 64, 0), 5);
}

TEST(a_furnace_draws_its_mouth_on_the_side_its_metadata_names)
{
    // `ku.a(Lnm;IIII)I`: stone top and bottom, the mouth on side == metadata,
    // and the plain side everywhere else. The inventory's `faces` always has
    // it on +Z, which is what every furnace in the world used to show.
    const BlockId furnace = bid(mcver::Block::Furnace);
    const BlockId lit = bid(mcver::Block::LitFurnace);
    const block::BlockDef& def = block::def(furnace);
    const u16 mouth = def.faces[mesh::kFacePosZ];
    const u16 side = def.faces[mesh::kFaceNegZ];
    CHECK(mouth != side);
    for (int md = 2; md <= 5; ++md) {
        const u16* tiles = block::worldFaces(furnace, u8(md));
        const u16* litTiles = block::worldFaces(lit, u8(md));
        CHECK_EQ(int(tiles[mesh::kFaceNegY]), int(def.faces[mesh::kFaceNegY]));
        CHECK_EQ(int(tiles[mesh::kFacePosY]), int(def.faces[mesh::kFacePosY]));
        for (int face = 2; face < 6; ++face) {
            CHECK_EQ(int(tiles[face]), int(face == md ? mouth : side));
            // A lit furnace glows: its mouth in the world is `bb + 16`, the
            // tile a row below the side, and not the unlit mouth its inventory
            // form shows.
            CHECK_EQ(int(litTiles[face]), int(face == md ? side + 16 : side));
        }
    }
    // The fast cube path cannot read metadata, so a furnace must not take it.
    CHECK(!def.unitCube);
    CHECK(!block::def(lit).unitCube);
    // Nothing else changes: stone's world faces are its faces.
    CHECK(block::worldFaces(bid(mcver::Block::Stone), 7) == block::def(bid(mcver::Block::Stone)).faces);
}

TEST(a_staircase_backs_onto_a_wall_and_climbs_towards_the_next_step)
{
    // Open floor: nothing to go on, and it keeps the 0 it was written with.
    {
        Floor f;
        CHECK(f.putDown(mcver::Block::WoodenStairs, 0, 0));
        CHECK_EQ(f.md(0, 64, 0), 0);
    }
    // Metadata is the side the high half is on: 0 +X, 1 -X, 2 +Z, 3 -Z.
    const struct {
        i32 dx, dz;
        int facing;
    } walls[] = {{1, 0, 0}, {-1, 0, 1}, {0, 1, 2}, {0, -1, 3}};
    for (const auto& w : walls) {
        Floor f;
        f.wall(w.dx, 64, w.dz);
        CHECK(f.putDown(mcver::Block::CobblestoneStairs, 0, 0));
        CHECK_EQ(f.md(0, 64, 0), w.facing);
    }
    // A staircase one step up beats a wall: the first pass is asked first.
    {
        Floor f;
        f.scene.place(1, 65, 0, bid(mcver::Block::WoodenStairs), 3);
        f.wall(-1, 64, 0);
        CHECK(f.putDown(mcver::Block::WoodenStairs, 0, 0));
        CHECK_EQ(f.md(0, 64, 0), 0);
    }
}

TEST(laying_the_next_step_of_a_flight_turns_the_one_below)
{
    // The lower step backs onto a wall on +Z. The upper one goes in at +X, one
    // up, and re-shapes it: a staircase one step up on a side wins over the
    // wall. The upper step climbs away from the one below.
    Floor f;
    f.wall(0, 64, 1);
    CHECK(f.putDown(mcver::Block::WoodenStairs, 0, 0));
    CHECK_EQ(f.md(0, 64, 0), 2);

    f.scene.w().setBlockWithNotify(1, 65, 0, bid(mcver::Block::WoodenStairs));
    CHECK_EQ(f.md(0, 64, 0), 0);
    CHECK_EQ(f.md(1, 65, 0), 0);
}

TEST(a_staircase_under_something_solid_turns_into_the_block_it_is_made_of)
{
    // Putting a block on top of it.
    {
        Floor f;
        CHECK(f.putDown(mcver::Block::WoodenStairs, 0, 0));
        CHECK(f.click(mcver::Block::Stone, 0, 64, 0, mesh::kFacePosY));
        CHECK_EQ(f.id(0, 65, 0), int(mcver::Block::Stone));
        CHECK_EQ(f.id(0, 64, 0), int(mcver::Block::Planks));
    }
    // And putting it under one in the first place, which is onBlockAdded
    // asking the same question. Glass is a solid material too.
    {
        Floor f;
        f.scene.place(0, 65, 0, bid(mcver::Block::Glass), 0);
        CHECK(f.putDown(mcver::Block::CobblestoneStairs, 0, 0));
        CHECK_EQ(f.id(0, 64, 0), int(mcver::Block::Cobblestone));
    }
    CHECK_EQ(int(block::modelOf(bid(mcver::Block::WoodenStairs))), int(mcver::Block::Planks));
    CHECK_EQ(int(block::modelOf(bid(mcver::Block::Stone))), 0);
}
