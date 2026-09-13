// The chest: which tile each of its faces shows, and what may be placed beside
// it.
//
// Both halves come out of one class -- `b`, BlockChest -- and both are about
// the *neighbours*, which is what makes them different in kind from everything
// else in the block table. `getBlockTexture` reads up to six cells to decide
// where the front is and which half of a double chest's picture this cell
// draws; `canPlaceBlockAt` reads four to refuse a third chest.
//
// **And the third chest is placeable anyway**, which is the case this file
// exists to pin: `World.canBlockBePlacedAt` returns true for a cell holding
// water, lava, fire or a snow layer *before* it asks the block, so a chest
// dropped into a puddle joins a pair that the rule would have refused. The
// screen that opens is three chests deep (`tick::chestInventoryParts`) and the
// texture rule was never written for it. Both are a1.1.2's, both are kept.

#include "core/block/registry.hpp"
#include "core/block/world_texture.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/vertex.hpp"
#include "core/tick/behaviour.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

using namespace mc;
using mc::block::BlockId;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// The tiles a1.1.2's terrain.png holds for a chest, as `blockIndexInTexture`
// (26) offsets them: the lid, the plain side, the single front, then the two
// halves of a double chest's front and of its back.
constexpr int kLid = 25;
constexpr int kSide = 26;
constexpr int kFront = 27;
constexpr int kDoubleFrontLeft = 41;
constexpr int kDoubleFrontRight = 42;
constexpr int kDoubleBackLeft = 57;
constexpr int kDoubleBackRight = 58;

// A 3 x 3 patch of ids round the chest, so a test can say what is beside it
// without a world. (0, 0) is the chest itself.
struct Patch {
    BlockId at[3][3] = {};  // [dx + 1][dz + 1]

    void faces(u16 out[6]) const
    {
        const BlockId self = at[1][1];
        block::chestFaces(self, block::def(self).texture,
                          [this](int dx, int dz) { return at[dx + 1][dz + 1]; }, out);
    }
};

Patch chestPatch()
{
    Patch p;
    p.at[1][1] = bid(mcver::Block::Chest);
    return p;
}

}  // namespace

TEST(a_lone_chest_turns_its_front_away_from_a_solid_block)
{
    u16 faces[6];

    // Out in the open the front is +Z, which is the tile the item shows too.
    Patch open = chestPatch();
    open.faces(faces);
    CHECK_EQ(int(faces[0]), kLid);
    CHECK_EQ(int(faces[1]), kLid);
    CHECK_EQ(int(faces[2]), kSide);
    CHECK_EQ(int(faces[3]), kFront);
    CHECK_EQ(int(faces[4]), kSide);
    CHECK_EQ(int(faces[5]), kSide);

    // One opaque neighbour, and the front is the opposite face. The four tests
    // run -z, +z, -x, +x and the last one to fire wins, so a chest in a corner
    // faces along x -- which is the original's order and not a tidier one.
    const struct {
        int dx, dz, front;
    } cases[] = {{0, -1, 3}, {0, 1, 2}, {-1, 0, 5}, {1, 0, 4}};
    for (const auto& c : cases) {
        Patch p = chestPatch();
        p.at[c.dx + 1][c.dz + 1] = bid(mcver::Block::Stone);
        p.faces(faces);
        for (int face = 2; face < 6; ++face) {
            CHECK_EQ(int(faces[face]), face == c.front ? kFront : kSide);
        }
    }

    // Opaque on both sides of an axis cancels: neither test fires.
    Patch between = chestPatch();
    between.at[1][0] = bid(mcver::Block::Stone);
    between.at[1][2] = bid(mcver::Block::Stone);
    between.faces(faces);
    CHECK_EQ(int(faces[3]), kFront);

    // **Glass is not a solid block to this rule.** It reads
    // `opaqueCubeLookup`, not the material and not `isSolid`.
    Patch glass = chestPatch();
    glass.at[1][0] = bid(mcver::Block::Glass);
    glass.faces(faces);
    CHECK_EQ(int(faces[3]), kFront);
}

TEST(a_double_chest_draws_one_picture_across_two_cells)
{
    u16 faces[6];

    // Paired along z: the two faces that look at each other go plain, and the
    // long sides take the two halves of the double front. Nothing solid
    // anywhere, so the front stays on +X, which is the branch's default.
    Patch minus = chestPatch();
    minus.at[1][0] = bid(mcver::Block::Chest);  // partner at z - 1
    minus.faces(faces);
    CHECK_EQ(int(faces[2]), kSide);
    CHECK_EQ(int(faces[3]), kSide);
    CHECK_EQ(int(faces[5]), kDoubleFrontLeft);
    CHECK_EQ(int(faces[4]), kDoubleBackRight);

    // The other half of that same pair asks from the other cell and draws the
    // other halves, so the seam joins rather than repeating.
    Patch plus = chestPatch();
    plus.at[1][2] = bid(mcver::Block::Chest);  // partner at z + 1
    plus.faces(faces);
    CHECK_EQ(int(faces[5]), kDoubleFrontRight);
    CHECK_EQ(int(faces[4]), kDoubleBackLeft);

    // A block beside *either* cell of the pair turns the whole pair, which is
    // why the rule reads the two diagonals at all: the stone here touches the
    // partner, not this chest.
    Patch turned = chestPatch();
    turned.at[1][0] = bid(mcver::Block::Chest);
    turned.at[2][0] = bid(mcver::Block::Stone);  // +x beside the partner
    turned.faces(faces);
    CHECK_EQ(int(faces[4]), kDoubleFrontRight);
    CHECK_EQ(int(faces[5]), kDoubleBackLeft);

    // Paired along x instead: the same picture, rotated, and the joining faces
    // are now -x and +x.
    Patch along = chestPatch();
    along.at[0][1] = bid(mcver::Block::Chest);  // partner at x - 1
    along.faces(faces);
    CHECK_EQ(int(faces[4]), kSide);
    CHECK_EQ(int(faces[5]), kSide);
    CHECK_EQ(int(faces[3]), kDoubleFrontRight);
    CHECK_EQ(int(faces[2]), kDoubleBackLeft);

    // The lid is the lid whatever the neighbours do.
    CHECK_EQ(int(faces[0]), kLid);
    CHECK_EQ(int(faces[1]), kLid);
}

TEST(a_third_chest_draws_as_half_a_pair_that_is_not_there)
{
    // The middle of three in a row. The -z test fires first, so it pairs with
    // that one and the chest on its +z side is left drawing its own half of a
    // pair whose other half is already spoken for. a1.1.2 does exactly this;
    // it is what a triple chest looks like in the game.
    u16 faces[6];
    Patch middle = chestPatch();
    middle.at[1][0] = bid(mcver::Block::Chest);
    middle.at[1][2] = bid(mcver::Block::Chest);
    middle.faces(faces);
    CHECK_EQ(int(faces[5]), kDoubleFrontLeft);
    CHECK_EQ(int(faces[2]), kSide);
    CHECK_EQ(int(faces[3]), kSide);
}

TEST(every_tile_the_chest_can_show_has_a_cube_atlas_slot)
{
    // **The bug this test exists for.** The cube pass samples a 512 x 512 atlas
    // of repeat slots, handed out per tile that a cube face can show, and a
    // tile with no slot falls back to tile 0 -- which in a1.1.2 is grass. The
    // four halves of a double chest's picture appear in no block's `faces` row,
    // so they had no slot, and a double chest wore grass down its long sides.
    const BlockId chest = bid(mcver::Block::Chest);
    u16 rule[block::kMaxWorldTextureTiles];
    const int count = block::worldTextureTiles(block::def(chest).worldTexture,
                                               block::def(chest).texture, rule);
    CHECK_EQ(count, 7);
    for (int i = 0; i < count; ++i) {
        CHECK(mesh::kCubeAtlas.slotOfTile[rule[i]] != mesh::kNoCubeSlot);
    }

    // And the same claim made the other way round: every tile the rule
    // actually produces, over every arrangement of neighbours it can see, is a
    // tile `worldTextureTiles` promised. Four neighbour cells, each air, stone
    // or a chest, is 81 arrangements, and the diagonals go with them.
    const BlockId ids[3] = {block::kAir, bid(mcver::Block::Stone), chest};
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            for (int c = 0; c < 3; ++c) {
                for (int d = 0; d < 3; ++d) {
                    Patch p = chestPatch();
                    p.at[1][0] = ids[a];
                    p.at[1][2] = ids[b];
                    p.at[0][1] = ids[c];
                    p.at[2][1] = ids[d];
                    p.at[0][0] = ids[a];  // the diagonals, which a pair reads
                    p.at[2][2] = ids[b];
                    u16 faces[6];
                    p.faces(faces);
                    for (int face = 0; face < 6; ++face) {
                        CHECK(mesh::kCubeAtlas.slotOfTile[faces[face]] != mesh::kNoCubeSlot);
                        bool listed = false;
                        for (int i = 0; i < count; ++i) {
                            listed = listed || rule[i] == faces[face];
                        }
                        CHECK(listed);
                    }
                }
            }
        }
    }
}

TEST(a_blocks_world_faces_are_its_own_row_unless_it_has_a_rule)
{
    // Everything that is not a chest answers out of the generated table, so
    // the mesher can ask one question for every block.
    u16 faces[6];
    const BlockId log = bid(mcver::Block::Log);
    block::worldTextureFaces(log, 0, [](int, int) { return block::kAir; }, faces);
    for (int face = 0; face < 6; ++face) {
        CHECK_EQ(int(faces[face]), int(block::def(log).faces[face]));
    }

    // And the chest is off the mesher's fast path, because that path draws the
    // row this one cannot use.
    CHECK(!block::def(bid(mcver::Block::Chest)).unitCube);
    CHECK_EQ(int(block::def(bid(mcver::Block::Chest)).worldTexture),
             int(block::WorldTexture::Chest));
}

namespace {

// Far enough away that no placement is ever inside it.
const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

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

// A stone floor at y = 63 with air above it, so a chest clicked onto the floor
// lands at y = 64.
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

    bool putChest(i32 x, i32 z)
    {
        return item::rightClick(scene.w(), item::ItemId(mcver::Item::Chest),
                                hitOn(x, 63, z, mesh::kFacePosY), kNoPlayer, 0.0f);
    }

    BlockId at(i32 x, i32 z) { return scene.w().blockAt(x, 64, z); }
};

}  // namespace

TEST(a_chest_may_join_one_chest_and_never_two)
{
    Floor f;
    const BlockId chest = bid(mcver::Block::Chest);

    CHECK(f.putChest(0, 0));
    CHECK_EQ(int(f.at(0, 0)), int(chest));

    // The second one pairs with it.
    CHECK(f.putChest(0, 1));
    CHECK_EQ(int(f.at(0, 1)), int(chest));

    // The third is refused wherever it would make a triple: beside the pair
    // along its own axis, and beside either half across it.
    CHECK(!f.putChest(0, 2));
    CHECK_EQ(int(f.at(0, 2)), int(block::kAir));
    CHECK(!f.putChest(1, 0));
    CHECK_EQ(int(f.at(1, 0)), int(block::kAir));
    CHECK(!f.putChest(1, 1));

    // Two chests diagonally apart are not a pair, so this one is fine.
    CHECK(f.putChest(2, 2));

    // The rule is `canPlaceAt`'s, not the click's: it says the same thing.
    CHECK(!tick::canPlaceAt(f.scene.w(), chest, 0, 64, 2));
    CHECK(tick::canPlaceAt(f.scene.w(), chest, 3, 64, 3));
}

TEST(a_chest_placed_into_water_makes_a_triple_chest)
{
    // **The bug, and it is the game's.** `World.canBlockBePlacedAt` answers
    // true for a cell holding water, lava, fire or a snow layer without ever
    // asking `Block.canPlaceBlockAt`, so the chest's own rule is skipped and
    // the cluster the screen can already model becomes buildable.
    Floor f;
    const BlockId chest = bid(mcver::Block::Chest);
    CHECK(f.putChest(0, 0));
    CHECK(f.putChest(0, 1));

    // Dry land still refuses it.
    CHECK(!f.putChest(0, 2));

    // A puddle in that cell does not.
    f.scene.place(0, 64, 2, bid(mcver::Block::Water), 0);
    CHECK(f.putChest(0, 2));
    CHECK_EQ(int(f.at(0, 2)), int(chest));

    // And a fourth in the row, through lava this time, which is the same hole.
    f.scene.place(0, 64, 3, bid(mcver::Block::Lava), 0);
    CHECK(f.putChest(0, 3));

    // The screen joins the clicked chest and its two neighbours, in the order
    // `hs` nests them -- so which three of the four you get depends on which
    // one you open, and the row is deeper than a1.1.2 ever means to draw.
    tick::ChestPart parts[tick::kMaxChestParts];
    CHECK_EQ(tick::chestInventoryParts(f.scene.w(), 0, 64, 1, parts), 3);
    CHECK_EQ(int(parts[0].z), 0);
    CHECK_EQ(int(parts[1].z), 1);
    CHECK_EQ(int(parts[2].z), 2);
    CHECK_EQ(tick::chestInventoryParts(f.scene.w(), 0, 64, 2, parts), 3);
    CHECK_EQ(int(parts[0].z), 1);
    CHECK_EQ(int(parts[1].z), 2);
    CHECK_EQ(int(parts[2].z), 3);
}
