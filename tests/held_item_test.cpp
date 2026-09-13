// The item in your hand.
//
// Two things are worth asserting and they are not the same kind of thing.
//
// The **animation** has an oracle: `ItemRenderer.updateEquippedItem` and
// `EntityPlayer.updateArmSwingProgress` are short enough to predict tick by
// tick, so the counters are checked against the numbers the class file
// produces -- 0.4 a tick, eight ticks of swing, and the wrap at the end of one.
//
// The **geometry** has none: no oracle can say where a corner of a held pickaxe
// belongs. What it can say is where the item as a whole must end up, and that
// is the property this feature exists for -- **right of the eye, below it, and
// in front of it**, which is the bottom right corner of the screen. The rest
// are structural: one sheet per item, a full 66-quad sprite or whole boxes, no
// writing past the end of the buffer, and an empty hand drawing nothing.

#include "core/block/registry.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"
#include "core/render/box_model.hpp"
#include "core/render/held_item.hpp"
#include "core/texture/entity_skins.hpp"
#include "framework.hpp"

#include <vector>

using namespace mc;
using mc::item::IconSheet;
using mc::item::ItemId;
using mc::render::buildHeldItem;
using mc::render::HeldSheet;
using mc::render::HeldItemMesh;
using mc::render::HeldItemState;

namespace {

// 400 x 240, the top screen, which is what the shift in `buildHeldItem` is for.
constexpr float kTopScreenAspect = 400.0f / 240.0f;

ItemId firstBlockItem()
{
    for (int i = 0; i < item::paletteSize(); ++i) {
        const ItemId id = item::paletteItem(i);
        const item::ItemDef& def = item::def(id);
        if (def.sheet == IconSheet::Terrain && def.places != 0
            && block::def(block::BlockId(def.places)).render == block::RenderType::Cube) {
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

struct Built {
    std::vector<mesh::DetailVertex> verts;
    HeldItemMesh mesh;
};

Built build(ItemId item, float equipped, float swing, float aspect = kTopScreenAspect)
{
    Built built;
    built.verts.resize(usize(render::kMaxHeldItemVertices));
    built.mesh = buildHeldItem(item, equipped, swing, aspect, 0xF0, built.verts.data(),
                               render::kMaxHeldItemVertices);
    built.verts.resize(usize(built.mesh.vertices));
    return built;
}

// The mean corner, back in blocks. Camera space: +x right, +y up, -z forward.
void centroid(const std::vector<mesh::DetailVertex>& verts, double* x, double* y, double* z)
{
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (const mesh::DetailVertex& v : verts) {
        sx += double(v.x);
        sy += double(v.y);
        sz += double(v.z);
    }
    const double n = double(verts.size()) * double(mesh::kDetailUnitsPerBlock);
    *x = sx / n;
    *y = sy / n;
    *z = sz / n;
}

// Ticks the state until the item it is drawing is the one asked for and fully
// raised, and says how many ticks that took.
int raise(HeldItemState& state, ItemId item)
{
    for (int i = 0; i < 32; ++i) {
        state.tick(item);
        if (state.item() == item && state.equippedProgress(1.0f) >= 1.0f) {
            return i + 1;
        }
    }
    return -1;
}

}  // namespace

TEST(an_empty_hand_draws_the_arm)
{
    const Built built = build(0, 1.0f, 0.0f);
    // One box off the player skin -- `bipedRightArm.render(0.0625F)` and
    // nothing else. Six faces, four corners.
    CHECK_EQ(built.mesh.vertices, render::kBoxVertices);
    CHECK(built.mesh.sheet == HeldSheet::PlayerSkin);
}

TEST(the_arm_samples_the_player_page_and_no_other)
{
    const Built built = build(0, 1.0f, 0.0f);
    CHECK(built.mesh.vertices > 0);
    int originU = 0;
    int originV = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &originU, &originV);
    // **The page and not the sheet.** Every other page shares this texture, so
    // a UV that wandered would sample a boat rather than an arm -- and
    // `buildBox` clamps into the page precisely so that cannot happen.
    for (const mesh::DetailVertex& v : built.verts) {
        const double u = double(v.u) * texture::kEntitySheetWidth
                         / double(mesh::kUvUnitsPerAtlas);
        const double w = double(v.v) * texture::kEntitySheetHeight
                         / double(mesh::kUvUnitsPerAtlas);
        CHECK(u >= double(originU) - 0.01);
        CHECK(u <= double(originU + texture::kSkinPageWidth) + 0.01);
        CHECK(w >= double(originV) - 0.01);
        CHECK(w <= double(originV + texture::kSkinPageHeight) + 0.01);
    }
}

TEST(an_item_this_build_does_not_know_draws_neither_it_nor_the_arm)
{
    // The hand is not empty -- this build just cannot say what is in it, and a
    // bare arm would claim the opposite.
    std::vector<mesh::DetailVertex> verts(usize(render::kMaxHeldItemVertices));
    const HeldItemMesh mesh = buildHeldItem(item::ItemId(2000), 1.0f, 0.0f, kTopScreenAspect,
                                            0, verts.data(), render::kMaxHeldItemVertices);
    CHECK_EQ(mesh.vertices, 0);
}

TEST(a_block_in_the_hand_draws_as_whole_boxes_off_the_terrain_sheet)
{
    const ItemId block = firstBlockItem();
    CHECK(block != 0);
    const Built built = build(block, 1.0f, 0.0f);
    CHECK(built.mesh.sheet == HeldSheet::Terrain);
    // Six faces of four corners, and never a partial box: a cube is one box.
    CHECK_EQ(built.mesh.vertices, 6 * 4);
}

TEST(everything_else_draws_as_the_extruded_sprite)
{
    const ItemId sprite = firstSpriteItem();
    CHECK(sprite != 0);
    const Built built = build(sprite, 1.0f, 0.0f);
    CHECK(built.mesh.sheet == HeldSheet::Items);
    // A front face, a back face and four runs of sixteen edge strips.
    CHECK_EQ(built.mesh.vertices, render::kHeldSpriteQuads * 4);
    CHECK_EQ(render::kHeldSpriteQuads, 66);
}

TEST(the_held_item_sits_right_of_the_eye_below_it_and_in_front_of_it)
{
    // The arm too: an empty hand is the third branch and belongs in the same
    // corner as the other two.
    for (const ItemId item : {firstBlockItem(), firstSpriteItem(), ItemId(0)}) {
        const Built built = build(item, 1.0f, 0.0f);
        CHECK(built.mesh.vertices > 0);
        double x, y, z;
        centroid(built.verts, &x, &y, &z);
        // The bottom right corner, and in camera space that is all three signs
        // at once. -Z is forward, so an item behind the eye would be z > 0.
        CHECK(x > 0.0);
        CHECK(y < 0.0);
        CHECK(z < 0.0);
        // **And in front of the near plane rather than through it.** a1.1.2
        // sets its own up at 0.05 and so does `Renderer::drawHeldItem`; the
        // world's is 0.2 and the nearest corner of a held sword reaches 0.193,
        // which is why that pass does not share it.
        for (const mesh::DetailVertex& v : built.verts) {
            CHECK(double(v.z) / double(mesh::kDetailUnitsPerBlock) < -0.05);
        }
    }
}

TEST(a_wider_screen_moves_the_item_further_out_and_nowhere_else)
{
    const ItemId item = firstSpriteItem();
    CHECK(item != 0);
    const Built narrow = build(item, 1.0f, 0.0f, render::kOriginalAspect);
    const Built wide = build(item, 1.0f, 0.0f, kTopScreenAspect);
    CHECK_EQ(narrow.mesh.vertices, wide.mesh.vertices);

    double nx, ny, nz, wx, wy, wz;
    centroid(narrow.verts, &nx, &ny, &nz);
    centroid(wide.verts, &wx, &wy, &wz);
    CHECK(wx > nx);
    // A shift and not a scale: the item does not change shape, so y and z are
    // untouched and every corner moves by the same amount.
    CHECK(ny == wy);
    CHECK(nz == wz);
    // 0.7 * 0.8 * (5/3 / (4/3) - 1) = 0.14 of a block, which is 143.36 units --
    // so a corner lands on 143 or 144 depending on which side of a unit it
    // started, and the check is that they all land within one of the same
    // number rather than on it exactly.
    const int expected = int(0.14 * double(mesh::kDetailUnitsPerBlock) + 0.5);
    for (usize i = 0; i < narrow.verts.size(); ++i) {
        const int moved = int(wide.verts[i].x) - int(narrow.verts[i].x);
        CHECK(moved >= expected - 1);
        CHECK(moved <= expected + 1);
    }
}

TEST(a_lowered_item_is_drawn_lower)
{
    const ItemId item = firstBlockItem();
    CHECK(item != 0);
    const Built up = build(item, 1.0f, 0.0f);
    const Built down = build(item, 0.0f, 0.0f);
    double ux, uy, uz, dx, dy, dz;
    centroid(up.verts, &ux, &uy, &uz);
    centroid(down.verts, &dx, &dy, &dz);
    // `-(1 - equippedProgress) * 0.6` is the whole of it, so a fully lowered
    // item is six tenths of a block further down and nothing else moves.
    CHECK(dy < uy);
    CHECK(dx == ux);
    CHECK(dz == uz);
}

TEST(the_buffer_it_was_given_is_the_buffer_it_writes)
{
    const ItemId item = firstSpriteItem();
    CHECK(item != 0);
    std::vector<mesh::DetailVertex> verts(usize(render::kMaxHeldItemVertices));
    // One vertex short of what the sprite needs. Nothing at all is the right
    // answer: half an extruded icon is worse than none.
    const HeldItemMesh mesh = buildHeldItem(item, 1.0f, 0.0f, kTopScreenAspect, 0, verts.data(),
                                            render::kHeldSpriteQuads * 4 - 1);
    CHECK_EQ(mesh.vertices, 0);
}

TEST(the_light_byte_reaches_every_vertex)
{
    const Built built = build(firstBlockItem(), 1.0f, 0.0f);
    CHECK(built.mesh.vertices > 0);
    for (const mesh::DetailVertex& v : built.verts) {
        CHECK_EQ(int(v.light), 0xF0);
    }
}

TEST(switching_slots_lowers_one_item_and_raises_the_next)
{
    const ItemId first = firstBlockItem();
    const ItemId second = firstSpriteItem();
    CHECK(first != 0);
    CHECK(second != 0);

    HeldItemState state;
    // **The first tick adopts rather than raises.** equippedProgress starts at
    // zero, which is below the 0.1 threshold, so `itemToRender` is taken on the
    // spot and only then does it climb at 0.4 a tick: 0.4, 0.8, 1.0.
    CHECK_EQ(raise(state, first), 4);
    CHECK(state.item() == first);

    // Now select something else. The drawn item stays put while it sinks.
    state.tick(second);
    CHECK(state.item() == first);
    CHECK(state.equippedProgress(1.0f) > 0.55f);
    CHECK(state.equippedProgress(1.0f) < 0.65f);
    state.tick(second);
    CHECK(state.item() == first);
    state.tick(second);
    // 1.0 -> 0.6 -> 0.2 -> below 0.1, which is where the swap happens.
    CHECK(state.item() == second);
    CHECK(state.equippedProgress(1.0f) < 0.1f);
}

TEST(a_swing_runs_eight_ticks_and_does_not_run_backwards)
{
    HeldItemState state;
    state.tick(0);
    CHECK_EQ(double(state.swingProgress(1.0f)), 0.0);

    state.swing();
    // Eight ticks of 0, 1/8 .. 7/8, in order and never going backwards.
    float previous = -1.0f;
    for (int i = 0; i < 8; ++i) {
        state.tick(0);
        const float here = state.swingProgress(1.0f);
        CHECK(here >= previous);
        previous = here;
    }
    CHECK_EQ(double(previous), 7.0 / 8.0);

    // **The ninth tick ends it, and it ends at one rather than at zero.** The
    // counter is back on zero, but `getSwingProgress` adds the wrap -- and 1.0
    // is the same arm position as 0.0, because every term the transform builds
    // out of it is a sine of a multiple of pi.
    state.tick(0);
    CHECK_EQ(double(state.swingProgress(1.0f)), 1.0);

    // And the tick after that is idle.
    state.tick(0);
    CHECK_EQ(double(state.swingProgress(1.0f)), 0.0);
}

TEST(the_last_eighth_of_a_swing_interpolates_forwards)
{
    HeldItemState state;
    state.tick(0);
    state.swing();
    for (int i = 0; i < 9; ++i) {
        state.tick(0);
    }
    // swingProgress went 7/8 -> 0 on that last tick. Half way through the frame
    // the arm must read 15/16 and not 7/16.
    CHECK(state.swingProgress(0.5f) > 0.9f);
    CHECK(state.swingProgress(0.5f) < 1.0f);
}

TEST(a_swing_bends_the_item_away_from_where_it_rests)
{
    const ItemId item = firstBlockItem();
    CHECK(item != 0);
    const Built rest = build(item, 1.0f, 0.0f);
    const Built mid = build(item, 1.0f, 0.5f);
    double rx, ry, rz, mx, my, mz;
    centroid(rest.verts, &rx, &ry, &rz);
    centroid(mid.verts, &mx, &my, &mz);
    // Half way through a swing the item is somewhere else entirely; which way
    // is the transform's business, that it moves at all is this test's.
    CHECK(mx != rx || my != ry || mz != rz);
    // And it is still on the right-hand side of the screen while it swings.
    CHECK(mx > 0.0);
}

TEST(a_button_in_the_hand_is_smaller_than_the_stone_it_is_made_of)
{
    // The other half of "the stone button is a normal stone block in the
    // inventory and hand". The hand draws `block::itemRenderBoxes`, which for a
    // button is `setBlockBoundsForItemRender`'s six-by-four-by-four box and not
    // the full cube its world bounds hold at metadata 0.
    const Built button = build(ItemId(mcver::Block::StoneButton), 1.0f, 0.0f);
    const Built stone = build(ItemId(mcver::Block::Stone), 1.0f, 0.0f);
    CHECK(button.mesh.sheet == HeldSheet::Terrain);
    CHECK_EQ(button.mesh.vertices, stone.mesh.vertices);

    const auto widest = [](const std::vector<mesh::DetailVertex>& verts) {
        i16 lo = verts.front().x;
        i16 hi = verts.front().x;
        for (const mesh::DetailVertex& v : verts) {
            lo = v.x < lo ? v.x : lo;
            hi = v.x > hi ? v.x : hi;
        }
        return int(hi) - int(lo);
    };
    CHECK(widest(button.verts) > 0);
    CHECK(widest(button.verts) < widest(stone.verts));
}
