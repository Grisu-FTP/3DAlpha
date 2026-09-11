// Signs -- `md.a(...)` (ItemSign), `ob` (TileEntitySign) and `jk`/`in` (the
// model and its renderer).
//
// "Signs don't render and don't open the keyboard on placing" is one bug with
// three halves, and the first is why the other two were invisible: **a sign is
// render type -1 in a1.1.2.** The mesher is asked to draw block 63 and answers
// with nothing, correctly, because the whole of a sign is drawn from a tile
// entity that this build did not have. No tile entity, nothing to draw and
// nothing to type into.
//
// The keyboard itself is a console applet and cannot be tested here. Everything
// else can: which block goes down, which way it faces, that the text is kept,
// and that the geometry lands where the block is.

#include "core/block/registry.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/sign_mesh.hpp"
#include "core/texture/font.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/sign_store.hpp"
#include "framework.hpp"
#include "low_heap.hpp"
#include "scene_world.hpp"

#include <cmath>
#include <cstring>

using namespace mc;
using mc::block::BlockId;
using mc::test::SceneWorld;
using mc::block::TickBehaviour;
using mc::world::SignStore;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

const AABB kNoPlayer{1000.0, 1000.0, 1000.0, 1000.6, 1001.8, 1000.6};

// The sign item, found by what it places rather than by its id: it is the only
// thing in the table whose `places` column is a block whose tick behaviour is a
// sign post.
item::ItemId signItem()
{
    for (int id = 256; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.places != 0
            && block::def(BlockId(def.places)).tick == TickBehaviour::SignPost) {
            return item::ItemId(id);
        }
    }
    return 0;
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

// A stone pillar to hang signs on, with air around it, and the store wired to
// the world the way the frame loop wires it.
struct Post {
    SceneWorld scene{0, 0};
    SignStore signs;

    Post()
    {
        for (int y = 60; y <= 64; ++y) {
            scene.place(0, y, 0, bid(mcver::Block::Stone), 0);
        }
        w().setTileEntityRemovedSink(
            [](void* ctx, i32 x, int y, i32 z) { static_cast<SignStore*>(ctx)->erase(x, y, z); },
            &signs);
    }
    Post(const Post&) = delete;
    Post& operator=(const Post&) = delete;

    tick::TickWorld& w() { return scene.w(); }

    bool click(int face, float yaw = 0.0f)
    {
        item::Effects effects;
        effects.entities.signs = &signs;
        return item::rightClick(w(), signItem(), hitOn(0, 64, 0, face), kNoPlayer, yaw,
                                effects);
    }
};

}  // namespace

TEST(the_sign_item_is_found_by_what_it_places)
{
    CHECK(signItem() != 0);
    // Both sign blocks are render type -1, which is the whole reason the mesher
    // draws nothing for them.
    const BlockId post = BlockId(mcver::kItems[signItem()].places);
    CHECK(block::def(post).render == block::RenderType::None);
}

TEST(a_sign_on_the_top_of_a_block_is_a_post_facing_the_player)
{
    Post post;
    // Face 1 is +Y. The metadata is the heading rounded to a sixteenth.
    CHECK(post.click(1, 0.0f));

    const BlockId placed = post.w().blockAt(0, 65, 0);
    CHECK(block::def(placed).tick == TickBehaviour::SignPost);
    CHECK_EQ(post.signs.count(), 1);
    CHECK(!post.signs[0].wall);

    // `floor((0 + 180) * 16 / 360 + 0.5) & 15` is 8.
    CHECK_EQ(int(post.w().dataAt(0, 65, 0)), 8);
    CHECK_EQ(int(post.signs[0].metadata), 8);
}

TEST(the_post_metadata_is_the_heading_rounded_to_a_sixteenth)
{
    // Sixteen headings, sixteen metadata values, and the rounding is the
    // original's -- a half added before the floor, and a mask rather than a
    // modulo so a negative yaw still lands in range.
    for (int step = 0; step < 16; ++step) {
        Post post;
        const float yaw = float(step) * 22.5f;
        CHECK(post.click(1, yaw));
        const int expect =
            int(std::floor((double(yaw) + 180.0) * 16.0 / 360.0 + 0.5)) & 15;
        CHECK_EQ(int(post.w().dataAt(0, 65, 0)), expect);
    }

    // ...and a negative heading, which is where a modulo would give the wrong
    // answer.
    Post post;
    CHECK(post.click(1, -90.0f));
    const int expect = int(std::floor((-90.0 + 180.0) * 16.0 / 360.0 + 0.5)) & 15;
    CHECK_EQ(int(post.w().dataAt(0, 65, 0)), expect);
}

TEST(a_sign_on_the_side_of_a_block_is_a_wall_sign_facing_out)
{
    for (int face = 2; face <= 5; ++face) {
        Post post;
        CHECK(post.click(face));
        // The block lands on the offset cell, not on the struck one.
        const entity::RayHit hit = hitOn(0, 64, 0, face);
        const BlockId placed = post.w().blockAt(hit.placeX(), hit.placeY(), hit.placeZ());
        CHECK(block::def(placed).tick == TickBehaviour::SignWall);
        // A wall sign's metadata **is** the face it was placed on.
        CHECK_EQ(int(post.w().dataAt(hit.placeX(), hit.placeY(), hit.placeZ())), face);
        CHECK(post.signs[0].wall);
    }
}

TEST(a_sign_refuses_the_underside)
{
    Post post;
    // `if (face == 0) return false` is the method's first line -- a sign cannot
    // hang from a ceiling in this version.
    CHECK(!post.click(0));
    CHECK_EQ(post.signs.count(), 0);
}

TEST(a_sign_needs_something_solid_to_be_put_on)
{
    SceneWorld scene{0, 0};
    SignStore signs;
    item::Effects effects;
    effects.entities.signs = &signs;
    // Nothing but air: the *struck* block's material is what is tested, not the
    // target cell's, so this fails on the first line that looks at the world.
    CHECK(!item::rightClick(scene.w(), signItem(), hitOn(0, 64, 0, 1), kNoPlayer, 0.0f,
                            effects));
    CHECK_EQ(signs.count(), 0);
}

TEST(a_placement_hands_the_new_sign_back_exactly_once)
{
    // The seam that opens the keyboard. A store that reported the same sign
    // twice would put a keyboard up on the next unrelated click.
    Post post;
    CHECK(post.click(1));
    const int index = post.signs.takeJustPlaced();
    CHECK_EQ(index, 0);
    CHECK_EQ(post.signs.takeJustPlaced(), -1);
}

TEST(text_is_kept_and_truncated_to_fifteen_characters)
{
    Post post;
    CHECK(post.click(1));
    post.signs.setLine(0, 0, "hello");
    post.signs.setLine(0, 1, "0123456789abcdefghij");
    post.signs.setLine(0, 3, "last");

    CHECK(std::strcmp(post.signs[0].lines[0], "hello") == 0);
    CHECK_EQ(int(std::strlen(post.signs[0].lines[1])), world::kSignLineLength);
    CHECK(std::strcmp(post.signs[0].lines[1], "0123456789abcde") == 0);
    // An untouched line stays empty rather than picking up its neighbour.
    CHECK_EQ(int(std::strlen(post.signs[0].lines[2])), 0);
    CHECK(std::strcmp(post.signs[0].lines[3], "last") == 0);
}

TEST(breaking_the_block_forgets_the_sign)
{
    Post post;
    CHECK(post.click(1));
    post.signs.setLine(0, 0, "here");
    CHECK_EQ(post.signs.count(), 1);

    // The world write alone is enough: `blockRemoved` asks the store to forget
    // it, as `jt.b` asks `World.removeBlockTileEntity`.
    post.scene.place(0, 65, 0, bid(mcver::Block::Air), 0);
    CHECK_EQ(post.signs.count(), 0);
    // ...and a sign built in the same hole starts blank rather than inheriting.
    CHECK(post.click(1));
    CHECK_EQ(int(std::strlen(post.signs[0].lines[0])), 0);
}

TEST(a_post_whose_block_is_broken_drops_and_is_forgotten)
{
    // The reported bug: the sign block became air and the store kept it, and a
    // sign is drawn from the store, so the board stayed standing on nothing.
    Post post;
    CHECK(post.click(1));
    CHECK_EQ(post.signs.count(), 1);

    post.w().setBlockWithNotify(0, 64, 0, bid(mcver::Block::Air));
    CHECK(post.w().blockAt(0, 65, 0) == bid(mcver::Block::Air));
    CHECK_EQ(post.signs.count(), 0);
    CHECK_EQ(post.signs.find(0, 65, 0), -1);
}

TEST(a_wall_sign_whose_wall_is_broken_drops_and_is_forgotten)
{
    for (int face = 2; face <= 5; ++face) {
        Post post;
        CHECK(post.click(face));
        const entity::RayHit hit = hitOn(0, 64, 0, face);
        CHECK_EQ(post.signs.count(), 1);

        post.w().setBlockWithNotify(0, 64, 0, bid(mcver::Block::Air));
        CHECK(post.w().blockAt(hit.placeX(), hit.placeY(), hit.placeZ())
              == bid(mcver::Block::Air));
        CHECK_EQ(post.signs.count(), 0);
    }
}

TEST(a_sign_whose_neighbour_is_not_its_support_stays)
{
    // A wall sign asks only the face its metadata names; a post only the block
    // under it. Breaking the stone *above* a post changes a neighbour and
    // leaves the sign where it is, text and all.
    Post post;
    post.scene.place(0, 66, 0, bid(mcver::Block::Stone), 0);
    CHECK(post.click(1));
    post.signs.setLine(0, 0, "stays");

    post.w().setBlockWithNotify(0, 66, 0, bid(mcver::Block::Air));
    CHECK(block::def(post.w().blockAt(0, 65, 0)).tick == TickBehaviour::SignPost);
    CHECK_EQ(post.signs.count(), 1);
    CHECK(std::strcmp(post.signs[0].lines[0], "stays") == 0);
}

TEST(there_is_no_sign_limit)
{
    // A sign is a tile entity and the original keeps every one built.
    SignStore signs;
    const int many = SignStore::kInitialCapacity * 2;
    for (int i = 0; i < many; ++i) {
        CHECK_EQ(signs.put(i32(i), 64, 0, false, 0), i);
    }
    CHECK_EQ(signs.count(), many);
    CHECK_EQ(int(signs.refused()), 0);
    CHECK_EQ(signs.find(i32(many - 1), 64, 0), many - 1);
}

TEST(a_full_heap_refuses_a_sign_rather_than_overflowing)
{
    SignStore signs;
    for (int i = 0; i < SignStore::kInitialCapacity; ++i) {
        CHECK(signs.put(i32(i), 64, 0, false, 0) >= 0);
    }
    test::LowHeap low;
    CHECK_EQ(signs.put(-1, 64, 0, false, 0), -1);
    CHECK_EQ(signs.count(), SignStore::kInitialCapacity);
    CHECK_EQ(int(signs.refused()), 1);
}

TEST(a_sign_the_store_will_not_hold_leaves_no_block_behind)
{
    // The block alone is render type -1 -- invisible -- so a sign whose tile
    // entity was refused must not be placed at all.
    Post post;
    for (int i = 0; i < SignStore::kInitialCapacity; ++i) {
        CHECK(post.signs.put(i32(100 + i), 64, 0, false, 0) >= 0);
    }
    post.signs.takeJustPlaced();  // the fill's own, not the click's
    test::LowHeap low;
    CHECK(!post.click(1));
    CHECK(post.w().blockAt(0, 65, 0) == block::kAir);
    CHECK_EQ(int(post.signs.refused()), 1);
    CHECK_EQ(post.signs.takeJustPlaced(), -1);
}

TEST(a_post_draws_two_boxes_and_a_wall_sign_draws_one)
{
    Post post;
    CHECK(post.click(1));

    mesh::DetailVertex verts[4096];
    const int written =
        render::buildSignBoards(post.signs, 0.0, 64.0, 0.0, verts, 4096);
    // A post is the board and the post; a wall sign hides the post.
    CHECK_EQ(written, render::kSignVerticesEach);

    Post wall;
    CHECK(wall.click(2));
    const int wallWritten =
        render::buildSignBoards(wall.signs, 0.0, 64.0, 0.0, verts, 4096);
    CHECK_EQ(wallWritten, render::kSignVerticesEach / 2);
}

TEST(the_board_sits_at_the_block_it_belongs_to)
{
    Post post;
    CHECK(post.click(1));

    mesh::DetailVertex verts[4096];
    const int written =
        render::buildSignBoards(post.signs, 0.0, 64.0, 0.0, verts, 4096);
    CHECK(written > 0);

    double lo[3] = {1e30, 1e30, 1e30};
    double hi[3] = {-1e30, -1e30, -1e30};
    for (int i = 0; i < written; ++i) {
        const double p[3] = {double(verts[i].x) / double(mesh::kDetailUnitsPerBlock),
                             double(verts[i].y) / double(mesh::kDetailUnitsPerBlock) + 64.0,
                             double(verts[i].z) / double(mesh::kDetailUnitsPerBlock)};
        for (int a = 0; a < 3; ++a) {
            lo[a] = p[a] < lo[a] ? p[a] : lo[a];
            hi[a] = p[a] > hi[a] ? p[a] : hi[a];
        }
    }
    // The sign occupies its own block: a board a block wide, standing on a post
    // inside the cell at y = 65.
    CHECK(hi[0] - lo[0] > 0.5);
    CHECK(hi[0] - lo[0] < 1.2);
    CHECK(lo[1] > 64.5);
    CHECK(hi[1] < 66.5);
}

TEST(text_needs_a_font_and_draws_nothing_without_one)
{
    // **The one place Dev Art is less than a pack.** There is no generated font
    // -- see core/texture/font.hpp -- so a sign on Dev Art shows a blank board.
    // That is stated rather than papered over, and this is the statement.
    Post post;
    CHECK(post.click(1));
    post.signs.setLine(0, 0, "hello");

    texture::FontImage none;
    mesh::DetailVertex verts[4096];
    CHECK_EQ(render::buildSignText(post.signs, none, 0.0, 64.0, 0.0, verts, 4096), 0);
}

TEST(text_with_a_font_is_one_quad_a_glyph)
{
    Post post;
    CHECK(post.click(1));
    post.signs.setLine(0, 0, "abc");
    post.signs.setLine(0, 2, "de");

    // A stand-in font: the sheet's contents do not matter here, only that it is
    // the right size and that every glyph has a width.
    texture::FontImage font;
    font.rgba.assign(texture::kFontBytes, 0xFF);
    for (int i = 0; i < 256; ++i) {
        font.widths[i] = 6;
    }

    mesh::DetailVertex verts[4096];
    const int written =
        render::buildSignText(post.signs, font, 0.0, 64.0, 0.0, verts, 4096);
    CHECK_EQ(written, 5 * 4);

    // Every UV lands inside the font sheet.
    for (int i = 0; i < written; ++i) {
        const double u = double(verts[i].u) * texture::kFontEdge
                         / double(mesh::kUvUnitsPerAtlas);
        const double v = double(verts[i].v) * texture::kFontEdge
                         / double(mesh::kUvUnitsPerAtlas);
        CHECK(u >= -0.01);
        CHECK(u <= double(texture::kFontEdge) + 0.01);
        CHECK(v >= -0.01);
        CHECK(v <= double(texture::kFontEdge) + 0.01);
    }
}

TEST(text_is_upright_on_the_front_of_the_board)
{
    // The text hangs off the turned frame, not the model's `(f, -f, -f)`
    // scale -- `in` pops that first. Folding it into the model frame put the
    // text upside down, under the board and behind it.
    Post post;
    CHECK(post.click(1, 0.0f));
    post.signs.setLine(0, 0, "a");
    post.signs.setLine(0, 3, "b");

    texture::FontImage font;
    font.rgba.assign(texture::kFontBytes, 0xFF);
    for (int i = 0; i < 256; ++i) {
        font.widths[i] = 6;
    }

    mesh::DetailVertex board[4096];
    const int boardCount =
        render::buildSignBoards(post.signs, 0.0, 64.0, 0.0, board, 4096);
    CHECK(boardCount > 0);
    // The board is the first box written; the post after it reaches lower.
    double boardLo = 1e30;
    double boardHi = -1e30;
    for (int i = 0; i < render::kSignVerticesEach / render::kSignParts; ++i) {
        const double y = double(board[i].y) / double(mesh::kDetailUnitsPerBlock) + 64.0;
        boardLo = y < boardLo ? y : boardLo;
        boardHi = y > boardHi ? y : boardHi;
    }

    mesh::DetailVertex text[4096];
    CHECK_EQ(render::buildSignText(post.signs, font, 0.0, 64.0, 0.0, text, 4096), 2 * 4);
    const auto y = [&](int i) {
        return double(text[i].y) / double(mesh::kDetailUnitsPerBlock) + 64.0;
    };
    const auto z = [&](int i) {
        return double(text[i].z) / double(mesh::kDetailUnitsPerBlock);
    };

    // On the board, not under it.
    for (int i = 0; i < 8; ++i) {
        CHECK(y(i) >= boardLo - 1e-3);
        CHECK(y(i) <= boardHi + 1e-3);
    }
    // Upright: the first line above the last, and each glyph's top row of
    // texels (the smaller v) above its bottom.
    CHECK(y(0) > y(4));
    CHECK(text[0].v < text[3].v);
    CHECK(y(0) > y(3));
    // In front: yaw 0 looks down +z, so metadata 8 turns the board to face -z,
    // and the text stands out past the board on that side.
    for (int i = 0; i < 8; ++i) {
        CHECK(z(i) < 0.5 - 0.04);
    }
}
