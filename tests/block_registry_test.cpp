#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/world/chunk.hpp"

using namespace mc;
using block::RenderType;

TEST(the_generated_table_covers_every_id_a_file_can_hold)
{
    // Pre-Anvil block ids are one byte on disk and on the wire, so every value
    // a world can contain must be in bounds. A hole here is an out-of-range
    // read in the mesher on a modded world.
    CHECK_EQ(mcver::kBlockTableSize, 256);
    for (int id = 0; id < 512; ++id) {
        const block::BlockDef& def = block::def(block::BlockId(id));
        CHECK(def.name != nullptr);
    }
}

TEST(air_is_nothing_and_stone_is_something)
{
    CHECK(block::isAir(0));
    CHECK(!block::isAir(1));

    const block::BlockDef& air = block::def(0);
    CHECK_EQ(std::string(air.name), std::string("air"));
    CHECK(air.render == RenderType::None);
    CHECK(!air.opaque);
    CHECK_EQ(air.opacity, u8(0));

    const block::BlockDef& stone = block::def(u16(mcver::Block::Stone));
    CHECK_EQ(std::string(stone.name), std::string("stone"));
    CHECK(stone.render == RenderType::Cube);
    CHECK(stone.opaque);
    CHECK_EQ(stone.opacity, u8(255));
    CHECK_EQ(stone.hardness, 1.5f);
}

TEST(unknown_ids_come_back_visible_rather_than_missing)
{
    // 21 is lapis ore, which arrives in Beta. An a1.1.2 build must survive
    // meeting one without indexing past the table -- and it should be a solid
    // block you can see, because an invisible wrong block is a mystery.
    for (block::BlockId id : {block::BlockId(21), block::BlockId(30),
                              block::BlockId(255), block::BlockId(4096)}) {
        const block::BlockDef& def = block::def(id);
        CHECK(!def.known);
        CHECK(def.opaque);
        CHECK(def.render == RenderType::Cube);
        CHECK_EQ(std::string(def.name), std::string("unknown"));
    }

    CHECK(block::def(u16(mcver::Block::Stone)).known);
}

TEST(light_emitters_match_the_original)
{
    // These values were recovered from the client jar as floats and scaled by
    // 15; they are the load-bearing half of the lightmap the renderer feeds.
    CHECK_EQ(block::def(u16(mcver::Block::Torch)).light, u8(14));
    CHECK_EQ(block::def(u16(mcver::Block::Lava)).light, u8(15));
    CHECK_EQ(block::def(u16(mcver::Block::Fire)).light, u8(15));
    CHECK_EQ(block::def(u16(mcver::Block::LitFurnace)).light, u8(13));
    CHECK_EQ(block::def(u16(mcver::Block::LitRedstoneOre)).light, u8(9));
    CHECK_EQ(block::def(u16(mcver::Block::RedstoneTorch)).light, u8(7));
    CHECK_EQ(block::def(u16(mcver::Block::BrownMushroom)).light, u8(1));

    CHECK_EQ(block::def(u16(mcver::Block::Stone)).light, u8(0));
    CHECK_EQ(block::def(u16(mcver::Block::Furnace)).light, u8(0));
}

TEST(opacity_distinguishes_see_through_from_full_cube)
{
    // Glass and leaves are full cubes that do not block light or cull their
    // neighbours' faces. Conflating the two flags would either make glass
    // solid or make stone transparent.
    const block::BlockDef& glass = block::def(u16(mcver::Block::Glass));
    CHECK(!glass.opaque);
    CHECK(glass.fullCube);

    const block::BlockDef& leaves = block::def(u16(mcver::Block::Leaves));
    CHECK(!leaves.opaque);
    CHECK(leaves.fullCube);
    CHECK_EQ(leaves.opacity, u8(1));  // dims light by one level per block

    const block::BlockDef& stairs = block::def(u16(mcver::Block::WoodenStairs));
    CHECK(!stairs.opaque);
    CHECK(!stairs.fullCube);
    CHECK(stairs.render == RenderType::Stairs);

    // The slab's isOpaqueCube branches on its own id, so the extractor cannot
    // see it and it is set by hand. Pinning it here is what stops a future
    // regeneration from quietly reverting it to the inherited `true`.
    const block::BlockDef& slab = block::def(u16(mcver::Block::Slab));
    CHECK(!slab.opaque);
    CHECK_EQ(slab.opacity, u8(0));
    CHECK(block::def(u16(mcver::Block::DoubleSlab)).opaque);
}

TEST(translucency_is_three_blocks_and_lines_up_with_nothing_else)
{
    // getRenderBlockPass == 1, recovered from the jar. The whole of it:
    CHECK(block::def(u16(mcver::Block::Water)).translucent);
    CHECK(block::def(u16(mcver::Block::FlowingWater)).translucent);
    CHECK(block::def(u16(mcver::Block::Ice)).translucent);

    // Lava is the trap. It shares BlockFluid with water and renders the same
    // way, and it is drawn in the *opaque* pass -- the two are told apart only
    // by their material, which is why this column could not be read as a
    // constant and had to be evaluated.
    CHECK(!block::def(u16(mcver::Block::Lava)).translucent);
    CHECK(!block::def(u16(mcver::Block::FlowingLava)).translucent);

    // Nor is it "you can see through it": glass and leaves are cut out by the
    // alpha test inside the opaque pass.
    CHECK(!block::def(u16(mcver::Block::Glass)).translucent);
    CHECK(!block::def(u16(mcver::Block::Leaves)).translucent);
    CHECK(!block::def(u16(mcver::Block::Stone)).translucent);

    // An id this version does not define must not land in the sorted pass: it
    // is drawn as a solid opaque cube on purpose.
    CHECK(!block::def(60000).translucent);
}

TEST(render_types_are_what_the_mesher_will_switch_on)
{
    CHECK(block::renderOf(u16(mcver::Block::Stone)) == RenderType::Cube);
    CHECK(block::renderOf(u16(mcver::Block::Dandelion)) == RenderType::Cross);
    CHECK(block::renderOf(u16(mcver::Block::SugarCane)) == RenderType::Cross);
    CHECK(block::renderOf(u16(mcver::Block::Torch)) == RenderType::Torch);
    CHECK(block::renderOf(u16(mcver::Block::Water)) == RenderType::Fluid);
    CHECK(block::renderOf(u16(mcver::Block::Ladder)) == RenderType::Ladder);
    CHECK(block::renderOf(u16(mcver::Block::Fence)) == RenderType::Fence);
    CHECK(block::renderOf(u16(mcver::Block::Cactus)) == RenderType::Cactus);

    // Signs are drawn by the tile-entity pass, not by the block mesher.
    CHECK(block::renderOf(u16(mcver::Block::SignPost)) == RenderType::None);
    CHECK(block::renderOf(u16(mcver::Block::WallSign)) == RenderType::None);

    CHECK_EQ(std::string(block::renderTypeName(RenderType::Fluid)),
             std::string("fluid"));
}

TEST(per_face_textures_are_what_the_original_hands_the_renderer)
{
    // These six numbers per block were interpreted out of the jar's
    // getBlockTexture rather than read, because every one of them is a branch
    // on the face index. Face order is mc::mesh::Face: 0 -Y, 1 +Y, 2 -Z,
    // 3 +Z, 4 -X, 5 +X.
    const u16 grass[6] = {2, 0, 3, 3, 3, 3};       // dirt below, grass above
    const u16 log[6] = {21, 21, 20, 20, 20, 20};   // rings on the cut ends
    const u16 crafting[6] = {4, 43, 60, 59, 60, 59};
    const u16 slab[6] = {6, 6, 5, 5, 5, 5};

    struct Case {
        mcver::Block block;
        const u16* faces;
    } cases[] = {
        {mcver::Block::Grass, grass},
        {mcver::Block::Log, log},
        {mcver::Block::CraftingTable, crafting},
        {mcver::Block::Slab, slab},
        {mcver::Block::DoubleSlab, slab},
    };

    for (const Case& item : cases) {
        const block::BlockDef& def = block::def(u16(item.block));
        for (int face = 0; face < 6; ++face) {
            CHECK_EQ(def.faces[face], item.faces[face]);
        }
    }
}

TEST(a_block_with_one_texture_repeats_it_on_every_face)
{
    // The mesher indexes `faces` unconditionally, so the uniform case has to be
    // spelled out rather than left to fall back on `texture`.
    for (mcver::Block block : {mcver::Block::Stone, mcver::Block::Dirt,
                               mcver::Block::Cobblestone, mcver::Block::Sand,
                               mcver::Block::Glass, mcver::Block::Leaves}) {
        const block::BlockDef& def = block::def(u16(block));
        for (int face = 0; face < 6; ++face) {
            CHECK_EQ(def.faces[face], def.texture);
        }
    }

    // Air and the unknown-id filler are indexed by the same loop and must be
    // in bounds, whatever they point at.
    for (block::BlockId id : {block::BlockId(0), block::BlockId(21)}) {
        for (int face = 0; face < 6; ++face) {
            CHECK(block::def(id).faces[face] < 256);
        }
    }
}

TEST(every_face_texture_is_inside_the_atlas)
{
    // A tile index past 255 would sample outside the 16x16 terrain atlas. The
    // mesher clamps, so a bad table would show up as a wrong texture rather
    // than a crash -- which is exactly the kind of thing that survives to a
    // screenshot. Checking the whole table catches it at build time instead.
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = block::def(block::BlockId(id));
        CHECK(def.texture < 256);
        for (int face = 0; face < 6; ++face) {
            CHECK(def.faces[face] < 256);
        }
    }
}

TEST(the_registry_reads_blocks_straight_out_of_a_column)
{
    // The join the mesher will make: a block id from storage, a definition
    // from the table, with no lookup table in between.
    world::ChunkColumn column;
    column.setBlock(1, 10, 1, u16(mcver::Block::Glass));
    column.setBlock(1, 11, 1, u16(mcver::Block::Torch));

    CHECK(!block::isOpaque(column.block(1, 10, 1)));
    CHECK_EQ(block::def(column.block(1, 11, 1)).light, u8(14));
    CHECK(block::isAir(column.block(1, 12, 1)));
}

// Solidity is the original's `Material.isSolid()`, and the reason it is its own
// column rather than something derived is that it lines up with nothing else in
// the table. These four are the counterexamples to every plausible shortcut:
// "solid means opaque", "solid means a full cube", "solid means it renders as a
// cube". Each of them is wrong here.
TEST(material_solidity_is_not_any_of_the_other_columns)
{
    const block::BlockDef& glass = block::def(u16(mcver::Block::Glass));
    CHECK(glass.solid);
    CHECK(!glass.opaque);  // solid, and light passes through it

    const block::BlockDef& stairs = block::def(u16(mcver::Block::WoodenStairs));
    CHECK(stairs.solid);
    CHECK(!stairs.fullCube);  // solid, and not a cube

    const block::BlockDef& button = block::def(u16(mcver::Block::StoneButton));
    CHECK(!button.solid);
    CHECK_EQ(int(button.render), int(block::RenderType::Cube));  // a cube, and not solid

    const block::BlockDef& snow = block::def(u16(mcver::Block::SnowLayer));
    CHECK(!snow.solid);
    CHECK_EQ(int(snow.render), int(block::RenderType::Cube));
}

// How a fluid recognises its own kind. The original compares materials, and so
// does the table: flowing and still water share one, lava shares another, and
// the two never match -- all without a block id appearing anywhere.
TEST(a_fluids_flowing_and_still_forms_share_a_material)
{
    const block::BlockDef& flowingWater = block::def(u16(mcver::Block::FlowingWater));
    const block::BlockDef& water = block::def(u16(mcver::Block::Water));
    const block::BlockDef& flowingLava = block::def(u16(mcver::Block::FlowingLava));
    const block::BlockDef& lava = block::def(u16(mcver::Block::Lava));

    CHECK_EQ(flowingWater.material, water.material);
    CHECK_EQ(flowingLava.material, lava.material);
    CHECK(water.material != lava.material);

    // Air is index 0, which no constructed block ever takes, so "same material
    // as air" can never accidentally be true.
    CHECK_EQ(block::def(block::kAir).material, u8(0));
    CHECK(water.material != 0);
}

TEST(every_known_block_has_a_material_and_unknown_ids_do_not)
{
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = block::def(block::BlockId(id));
        if (def.known && id != 0) {
            CHECK(def.material != 0);
        }
    }
    // An id this version never defined falls back to the unknown entry, which
    // is deliberately solid: a visible wrong block is a bug report.
    CHECK(!block::def(block::BlockId(200)).known);
}
