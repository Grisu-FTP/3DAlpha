// The furnace's tick, the chest's spill, and the three blocks that open a
// screen -- the tick side of a1.1.2's containers.

#include "core/block/registry.hpp"
#include "core/item/item_stack.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/furnace.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/tile_entity.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

#include <vector>

using namespace mc;
using mc::item::ItemStack;
using mc::test::SceneWorld;
using mc::tick::TickWorld;
using mcver::Block;
using mcver::Item;

namespace {

constexpr i32 kX = 8;
constexpr int kY = 64;
constexpr i32 kZ = 8;

ItemStack stack(i16 id, int count)
{
    ItemStack s;
    s.id = id;
    s.count = i8(count);
    return s;
}

world::TileEntity& furnaceAt(SceneWorld& scene)
{
    scene.place(kX, kY, kZ, block::BlockId(Block::Furnace), 2);
    std::vector<world::TileEntity>* list = scene.w().tileEntitiesAt(kX, kZ);
    return world::putTileEntity(*list, kX, kY, kZ, world::TileEntityKind::Furnace);
}

ItemStack slotOf(SceneWorld& scene, int slot)
{
    ItemStack slots[tick::kFurnaceSlotCount];
    const world::TileEntity* tile =
        world::findTileEntity(*scene.w().tileEntitiesAt(kX, kZ), kX, kY, kZ);
    if (tile == nullptr) {
        return ItemStack{};
    }
    tick::tileItems(*tile, slots, tick::kFurnaceSlotCount);
    return slots[slot];
}

}  // namespace

TEST(the_furnace_tables_are_the_jars)
{
    CHECK_EQ(tick::smeltingResult(item::ItemId(Block::Cobblestone)), int(Block::Stone));
    CHECK_EQ(tick::smeltingResult(item::ItemId(Block::IronOre)), int(Item::IronIngot));
    CHECK_EQ(tick::smeltingResult(item::ItemId(Block::Dirt)), -1);
    CHECK_EQ(tick::fuelTicks(item::ItemId(Item::Coal)), 1600);
    CHECK_EQ(tick::fuelTicks(item::ItemId(Block::Planks)), 300);
    CHECK_EQ(tick::fuelTicks(item::ItemId(Item::Stick)), 100);
    CHECK_EQ(tick::fuelTicks(item::ItemId(Item::LavaBucket)), 20000);
    CHECK_EQ(tick::fuelTicks(item::ItemId(Block::Stone)), 0);
}

TEST(a_lit_furnace_cooks_one_item_every_two_hundred_ticks)
{
    SceneWorld scene(0, 0);
    world::TileEntity& tile = furnaceAt(scene);
    const ItemStack in[3] = {stack(i16(Block::Cobblestone), 2), stack(i16(Item::Coal), 1), {}};
    tick::setTileItems(tile, in, 3);

    CHECK(tick::furnaceTickAt(scene.w(), kX, kY, kZ));
    // It took its fuel and lit, and the swap kept the metadata and the entry.
    CHECK_EQ(int(scene.w().blockAt(kX, kY, kZ)), int(Block::LitFurnace));
    CHECK(slotOf(scene, tick::kFurnaceFuelSlot).empty());
    for (int t = 1; t < 199; ++t) {
        tick::furnaceTickAt(scene.w(), kX, kY, kZ);
    }
    CHECK(slotOf(scene, tick::kFurnaceOutputSlot).empty());
    CHECK(tick::furnaceTickAt(scene.w(), kX, kY, kZ));  // tick 200
    CHECK_EQ(int(slotOf(scene, tick::kFurnaceOutputSlot).id), int(Block::Stone));
    CHECK_EQ(int(slotOf(scene, tick::kFurnaceOutputSlot).count), 1);
    CHECK_EQ(int(slotOf(scene, tick::kFurnaceInputSlot).count), 1);
    const world::TileEntity* after =
        world::findTileEntity(*scene.w().tileEntitiesAt(kX, kZ), kX, kY, kZ);
    CHECK(after != nullptr);
    // Set to 1600 on the first tick, and one spent on each of the other 199.
    CHECK_EQ(int(after->burnTime), 1600 - 199);
}

TEST(a_furnace_with_nothing_to_burn_does_nothing)
{
    SceneWorld scene(0, 0);
    world::TileEntity& tile = furnaceAt(scene);
    const ItemStack in[3] = {stack(i16(Block::Cobblestone), 2), {}, {}};
    tick::setTileItems(tile, in, 3);
    for (int t = 0; t < 10; ++t) {
        CHECK(!tick::furnaceTickAt(scene.w(), kX, kY, kZ));
    }
    CHECK_EQ(int(scene.w().blockAt(kX, kY, kZ)), int(Block::Furnace));
}

TEST(a_lava_bucket_burns_bucket_and_all)
{
    SceneWorld scene(0, 0);
    world::TileEntity& tile = furnaceAt(scene);
    const ItemStack in[3] = {stack(i16(Block::Sand), 1), stack(i16(Item::LavaBucket), 1), {}};
    tick::setTileItems(tile, in, 3);
    tick::furnaceTickAt(scene.w(), kX, kY, kZ);
    CHECK(slotOf(scene, tick::kFurnaceFuelSlot).empty());
}

TEST(the_output_slot_holding_something_else_stops_the_furnace)
{
    SceneWorld scene(0, 0);
    world::TileEntity& tile = furnaceAt(scene);
    const ItemStack in[3] = {stack(i16(Block::Cobblestone), 2), stack(i16(Item::Coal), 1),
                             stack(i16(Block::Dirt), 1)};
    tick::setTileItems(tile, in, 3);
    CHECK(!tick::furnaceTickAt(scene.w(), kX, kY, kZ));
    CHECK_EQ(int(slotOf(scene, tick::kFurnaceFuelSlot).count), 1);
}

namespace {

struct Spill {
    int cobble = 0;
    int torches = 0;
    int clumps = 0;
    static void sink(void* ctx, double, double, double, u16 item, int count, i16, double, double,
                     double)
    {
        Spill& self = *static_cast<Spill*>(ctx);
        ++self.clumps;
        if (item == u16(Block::Cobblestone)) {
            self.cobble += count;
        }
        if (item == u16(Block::Torch)) {
            self.torches += count;
        }
    }
};

struct Opened {
    int count = 0;
    TickWorld::ContainerKind kind = TickWorld::ContainerKind::Chest;
    static void sink(void* ctx, TickWorld::ContainerKind kind, i32, int, i32)
    {
        Opened& self = *static_cast<Opened*>(ctx);
        ++self.count;
        self.kind = kind;
    }
};

}  // namespace

TEST(a_broken_chest_spills_everything_in_clumps_and_forgets_its_contents)
{
    Spill spill;
    SceneWorld scene(0, 0);
    scene.w().setStackSink(&Spill::sink, &spill);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    std::vector<world::TileEntity>* list = scene.w().tileEntitiesAt(kX, kZ);
    world::TileEntity& chest =
        world::putTileEntity(*list, kX, kY, kZ, world::TileEntityKind::Chest);
    ItemStack cobble = stack(i16(Block::Cobblestone), 64);
    cobble.slot = 0;
    ItemStack torch = stack(i16(Block::Torch), 3);
    torch.slot = 5;
    chest.items = {cobble, torch};

    scene.w().setBlockWithNotify(kX, kY, kZ, block::kAir);
    CHECK_EQ(spill.cobble, 64);
    CHECK_EQ(spill.torches, 3);
    // 64 in clumps of 10 to 30 is at least three; three torches is one.
    CHECK(spill.clumps >= 4);
    CHECK(world::findTileEntity(*list, kX, kY, kZ) == nullptr);
}

TEST(a_chest_with_a_block_on_it_takes_the_click_and_stays_shut)
{
    Opened opened;
    SceneWorld scene(0, 0);
    scene.w().setContainerSink(&Opened::sink, &opened);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    scene.place(kX, kY + 1, kZ, block::BlockId(Block::Stone), 0);
    CHECK(tick::blockActivated(scene.w(), kX, kY, kZ));
    CHECK_EQ(opened.count, 0);

    scene.place(kX, kY + 1, kZ, block::kAir, 0);
    CHECK(tick::blockActivated(scene.w(), kX, kY, kZ));
    CHECK_EQ(opened.count, 1);
    CHECK(opened.kind == TickWorld::ContainerKind::Chest);
}

TEST(a_workbench_and_a_furnace_open_their_screens)
{
    Opened opened;
    SceneWorld scene(0, 0);
    scene.w().setContainerSink(&Opened::sink, &opened);
    scene.place(kX, kY, kZ, block::BlockId(Block::CraftingTable), 0);
    CHECK(tick::blockActivated(scene.w(), kX, kY, kZ));
    CHECK(opened.kind == TickWorld::ContainerKind::Workbench);
    scene.place(kX + 2, kY, kZ, block::BlockId(Block::Furnace), 2);
    CHECK(tick::blockActivated(scene.w(), kX + 2, kY, kZ));
    CHECK(opened.kind == TickWorld::ContainerKind::Furnace);
}

TEST(a_double_chest_puts_the_lower_coordinate_half_first)
{
    SceneWorld scene(0, 0);
    scene.place(kX, kY, kZ, block::BlockId(Block::Chest), 0);
    scene.place(kX + 1, kY, kZ, block::BlockId(Block::Chest), 0);
    tick::ChestPart parts[tick::kMaxChestParts];
    CHECK_EQ(tick::chestInventoryParts(scene.w(), kX, kY, kZ, parts), 2);
    CHECK_EQ(int(parts[0].x), kX);
    CHECK_EQ(int(parts[1].x), kX + 1);
    CHECK_EQ(tick::chestInventoryParts(scene.w(), kX + 1, kY, kZ, parts), 2);
    CHECK_EQ(int(parts[0].x), kX);
    CHECK_EQ(int(parts[1].x), kX + 1);
}
