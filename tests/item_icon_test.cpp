// The slot icon: a cube seen from the corner for a plain block, a flat sprite
// for everything else, and the right sheet for each.
//
// **What this is really checking is that a door stopped being half a door.**
// Every one of the icon bugs came from the same place -- a slot asked the block
// table for a single tile, and for a door, sugar cane or anything else with a
// carried form that tile is part of the block rather than the thing in your
// hand. So the interesting assertions here are about *which* pixels get drawn
// from *which* sheet, not about the rasteriser's edges.
//
// The atlases are synthetic: every tile a solid colour derived from its index,
// which makes "did this pixel come from tile 45 of terrain or tile 43 of items"
// a comparison rather than an eyeball.

#include "core/block/registry.hpp"
#include "core/block/model.hpp"
#include "core/gui/item_icon.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/texture/atlas_image.hpp"
#include "framework.hpp"

#include <cstring>
#include <vector>

using namespace mc;
using mc::gui::IconSheets;
using mc::gui::Surface;
using mc::item::ItemId;

namespace {

constexpr int kEdge = texture::kAtlasEdge;
constexpr int kTilePixels = texture::kAtlasTilePixels;
constexpr int kTiles = texture::kAtlasTilesPerEdge;

// Tile n is (n, 200, 100) on the terrain sheet and (n, 40, 220) on the items
// one, so a pixel says both which tile and which sheet it came from. Red is the
// tile index because there are 256 tiles and 256 values.
void fillSheet(std::vector<u8>* sheet, u8 g, u8 b)
{
    sheet->assign(usize(kEdge) * kEdge * 4, 0);
    for (int y = 0; y < kEdge; ++y) {
        for (int x = 0; x < kEdge; ++x) {
            const int tile = (y / kTilePixels) * kTiles + (x / kTilePixels);
            u8* texel = sheet->data() + (usize(y) * kEdge + usize(x)) * 4;
            texel[0] = u8(tile);
            texel[1] = g;
            texel[2] = b;
            texel[3] = 255;
        }
    }
}

struct Canvas {
    static constexpr int kSize = 16;
    gui::Pixel pixels[kSize * kSize] = {};

    Surface surface()
    {
        Surface s;
        s.pixels = pixels;
        s.strideX = 1;
        s.strideY = kSize;
        s.width = kSize;
        s.height = kSize;
        return s;
    }

    gui::Pixel at(int x, int y) const { return pixels[y * kSize + x]; }

    bool blank() const
    {
        for (gui::Pixel p : pixels) {
            if (p != 0) {
                return false;
            }
        }
        return true;
    }

    // How many distinct non-zero colours were painted. A cube shows three, a
    // flat tile one.
    int distinctColours() const
    {
        gui::Pixel seen[16] = {};
        int count = 0;
        for (gui::Pixel p : pixels) {
            if (p == 0) {
                continue;
            }
            bool known = false;
            for (int i = 0; i < count; ++i) {
                known = known || seen[i] == p;
            }
            if (!known && count < 16) {
                seen[count++] = p;
            }
        }
        return count;
    }
};

u8 shaded(int value, float factor)
{
    const int scaled = int(float(value) * factor + 0.5f);
    return u8(scaled > 255 ? 255 : scaled);
}

gui::Pixel terrainColour(int tile, float factor)
{
    return gui::rgb565(int(shaded(tile & 0xFF, factor)), int(shaded(200, factor)),
                       int(shaded(100, factor)));
}

gui::Pixel itemsColour(int tile)
{
    return gui::rgb565(tile & 0xFF, 40, 220);
}

ItemId itemNamed(const char* name, item::IconSheet sheet)
{
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = item::def(ItemId(id));
        if (def.known && def.sheet == sheet && std::strcmp(def.name, name) == 0) {
            return ItemId(id);
        }
    }
    return 0;
}

}  // namespace

TEST(a_plain_block_draws_as_a_cube_with_three_brightnesses)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    const ItemId stone = itemNamed("stone", item::IconSheet::Terrain);
    CHECK(stone != 0);
    CHECK(gui::itemDrawsAsCube(stone));

    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, stone);

    // Stone is one tile on all six faces, so the only thing separating the
    // three visible ones is the shade -- which is exactly what makes a cube
    // read as a cube rather than as a flat square.
    CHECK_EQ(canvas.distinctColours(), 3);

    const block::BlockDef& def = block::def(block::BlockId(item::def(stone).places));
    // The middle of the top diamond, the middle of the left face, and the
    // middle of the right face.
    CHECK_EQ(int(canvas.at(8, 4)),
             int(terrainColour(def.faces[mesh::kFacePosY], mesh::kFaceShadeFloat[mesh::kFacePosY])));
    CHECK_EQ(int(canvas.at(4, 10)),
             int(terrainColour(def.faces[mesh::kFaceNegZ], mesh::kFaceShadeFloat[mesh::kFaceNegZ])));
    CHECK_EQ(int(canvas.at(11, 10)),
             int(terrainColour(def.faces[mesh::kFacePosX], mesh::kFaceShadeFloat[mesh::kFacePosX])));
}

TEST(the_cube_leaves_its_corners_alone)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets,
                      itemNamed("stone", item::IconSheet::Terrain));

    // The four corners of the box are outside the hexagon, which is what makes
    // the silhouette a cube instead of a square. Untouched means the slot's own
    // background shows through.
    CHECK_EQ(int(canvas.at(0, 0)), 0);
    CHECK_EQ(int(canvas.at(15, 0)), 0);
    CHECK_EQ(int(canvas.at(0, 15)), 0);
    CHECK_EQ(int(canvas.at(15, 15)), 0);
    // ...and the middle is not.
    CHECK(canvas.at(8, 8) != 0);
}

TEST(a_block_that_is_not_a_cube_draws_flat_from_terrain)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    // A torch is four quads with a stick carved out of them, not a cube, and
    // the original draws it flat in a slot for the same reason.
    const ItemId torch = itemNamed("torch", item::IconSheet::Terrain);
    CHECK(torch != 0);
    CHECK(!gui::itemDrawsAsCube(torch));

    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, torch);
    CHECK_EQ(canvas.distinctColours(), 1);
    CHECK_EQ(int(canvas.at(8, 8)), int(terrainColour(item::def(torch).icon, 1.0f)));
}

TEST(a_carried_item_draws_from_the_items_sheet)
{
    std::vector<u8> terrain;
    std::vector<u8> items;
    fillSheet(&terrain, 200, 100);
    fillSheet(&items, 40, 220);
    IconSheets sheets{terrain.data(), items.data()};

    const ItemId door = itemNamed("wooden_door", item::IconSheet::Items);
    CHECK(door != 0);
    CHECK(!gui::itemDrawsAsCube(door));

    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, door);
    // **The bug, as an assertion.** The door in the hand comes off items.png at
    // its own icon index, not off terrain.png at the door block's tile -- which
    // is the lower half of a door.
    CHECK_EQ(int(canvas.at(8, 8)), int(itemsColour(item::def(door).icon)));
    CHECK(canvas.at(8, 8) != terrainColour(block::def(block::BlockId(item::def(door).places)).texture,
                                           1.0f));
}

TEST(a_pack_with_no_items_sheet_falls_back_to_the_block)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    const ItemId door = itemNamed("wooden_door", item::IconSheet::Items);
    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, door);
    // Not nothing, and not a stone block: the tile of the block it places,
    // which is what this screen drew before there were two sheets.
    const block::BlockDef& def = block::def(block::BlockId(item::def(door).places));
    CHECK_EQ(int(canvas.at(8, 8)), int(terrainColour(def.texture, 1.0f)));
}

TEST(an_empty_slot_and_an_unknown_id_draw_nothing)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, 0);
    CHECK(canvas.blank());

    // A music disc: past the table, and drawn as an empty slot rather than as
    // whatever tile 0 happens to be. See core/item/registry.hpp.
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, ItemId(2256));
    CHECK(canvas.blank());
    // ...and an id this version simply does not define.
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets, ItemId(200));
    CHECK(canvas.blank());
}

TEST(a_null_terrain_sheet_draws_nothing_rather_than_reading_it)
{
    IconSheets sheets{};
    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), 0, 0, Canvas::kSize, sheets,
                      itemNamed("stone", item::IconSheet::Terrain));
    CHECK(canvas.blank());
}

TEST(the_icon_clips_against_the_surface)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    // Mostly off the top-left corner. Everything that lands outside is dropped
    // by Surface::at, so this is a crash test as much as a drawing one.
    Canvas canvas;
    gui::drawItemIcon(canvas.surface(), -10, -10, Canvas::kSize, sheets,
                      itemNamed("stone", item::IconSheet::Terrain));
    CHECK(!canvas.blank());
    gui::drawItemIcon(canvas.surface(), 14, 14, Canvas::kSize, sheets,
                      itemNamed("stone", item::IconSheet::Terrain));
}

TEST(a_fence_icon_is_a_post_and_not_a_solid_cube)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    // **The complaint this fixes.** A fence took the flat path and drew its
    // `texture` column, which for a fence is the plank tile -- a plain square
    // of planks. `RenderBlocks.renderItemIn3d` accepts render type 11, so the
    // original draws it in three dimensions like a block.
    const ItemId fence = itemNamed("fence", item::IconSheet::Terrain);
    CHECK(fence != 0);
    CHECK(gui::itemDrawsAsCube(fence));

    Canvas fenceCanvas;
    gui::drawItemIcon(fenceCanvas.surface(), 0, 0, Canvas::kSize, sheets, fence);
    Canvas stoneCanvas;
    gui::drawItemIcon(stoneCanvas.surface(), 0, 0, Canvas::kSize, sheets,
                      itemNamed("stone", item::IconSheet::Terrain));

    // Three brightnesses, like any block...
    CHECK_EQ(fenceCanvas.distinctColours(), 3);

    // ...but a post covers far fewer pixels than a whole cube does, which is
    // what says the silhouette followed the shape rather than filling the cell.
    int fencePixels = 0;
    int stonePixels = 0;
    for (int i = 0; i < Canvas::kSize * Canvas::kSize; ++i) {
        fencePixels += fenceCanvas.pixels[i] != 0 ? 1 : 0;
        stonePixels += stoneCanvas.pixels[i] != 0 ? 1 : 0;
    }
    CHECK(fencePixels > 0);
    CHECK(fencePixels * 2 < stonePixels);
}

TEST(a_slab_icon_is_shorter_than_a_full_block)
{
    std::vector<u8> terrain;
    fillSheet(&terrain, 200, 100);
    IconSheets sheets{terrain.data(), nullptr};

    Canvas slab;
    gui::drawItemIcon(slab.surface(), 0, 0, Canvas::kSize, sheets,
                      itemNamed("slab", item::IconSheet::Terrain));
    Canvas full;
    gui::drawItemIcon(full.surface(), 0, 0, Canvas::kSize, sheets,
                      itemNamed("double_slab", item::IconSheet::Terrain));

    // The two used to be pixel for pixel identical, which is the inventory half
    // of "a slab and a double slab both place a double slab".
    bool differ = false;
    for (int i = 0; i < Canvas::kSize * Canvas::kSize && !differ; ++i) {
        differ = slab.pixels[i] != full.pixels[i];
    }
    CHECK(differ);

    // **The top, not the bottom.** Both sit on the same floor -- the projection
    // sends y = 0 to the same row whatever the box -- so what a half-height
    // block loses is height at the top.
    int slabTop = Canvas::kSize;
    int fullTop = Canvas::kSize;
    for (int y = 0; y < Canvas::kSize; ++y) {
        for (int x = 0; x < Canvas::kSize; ++x) {
            if (slab.at(x, y) != 0 && y < slabTop) {
                slabTop = y;
            }
            if (full.at(x, y) != 0 && y < fullTop) {
                fullTop = y;
            }
        }
    }
    CHECK(fullTop < Canvas::kSize);
    CHECK(slabTop > fullTop);
}

TEST(every_three_dimensional_render_type_has_boxes_to_draw)
{
    // The two lists have to agree: `itemDrawsAsCube` says which render types go
    // down the 3D path, and `block::renderBoxes` is what that path draws. A
    // type in one and not the other is an icon that renders as nothing.
    AABB boxes[block::kMaxRenderBoxes];
    for (int id = 1; id < mcver::kBlockTableSize; ++id) {
        if (!mcver::kBlocks[id].known) {
            continue;
        }
        const int index = item::paletteIndexOf(ItemId(id));
        (void)index;
        if (!gui::itemDrawsAsCube(ItemId(id))) {
            continue;
        }
        const int count =
            block::renderBoxes(block::BlockId(id), 0, 0, boxes, block::kMaxRenderBoxes);
        CHECK(count > 0);
    }
}
