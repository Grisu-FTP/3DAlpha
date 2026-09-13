#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/item/item_stack.hpp"
#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"
#include "core/world/chunk.hpp"
#include "core/world/tile_entity.hpp"
#include "impl/storage/alpha_chunkfiles/chunk_nbt.hpp"

#include <string>
#include <vector>

// The `TileEntities` list: the model, its codec, and the reconcile pass that
// keeps it agreeing with the blocks. See core/world/tile_entity.hpp.
//
// **Why the NBT here is hand-written rather than produced by encodeChunk.** The
// round trips below would pass just as happily if the decoder and the encoder
// were wrong in the same way. A file built by hand -- in the tag order a Java
// HashMap actually produces, which is not insertion order -- is the only thing
// that pins us to the format instead of to ourselves.

using namespace mc;
using world::ChunkColumn;
using world::TileEntity;
using world::TileEntityKind;

namespace {

world::BlockId bid(mcver::Block b) { return world::BlockId(b); }

// A chunk file with a `TileEntities` list the caller fills in, and nothing
// else interesting. The column arrays are the minimum a decode accepts.
struct ChunkBuilder {
    std::vector<u8> blocks{std::vector<u8>(alpha::kBlocksBytes, 0)};
    std::vector<u8> nibbles{std::vector<u8>(alpha::kNibbleBytes, 0)};
    std::vector<u8> heightMap{std::vector<u8>(alpha::kHeightMapBytes, 0)};

    void setBlock(int x, int y, int z, u8 id)
    {
        blocks[usize(y) + usize(z) * usize(ChunkColumn::kHeight) +
               usize(x) * usize(ChunkColumn::kHeight) * usize(ChunkColumn::kWidth)] = id;
    }

    template <class Fill>
    std::vector<u8> build(i32 cx, i32 cz, const Fill& fill) const
    {
        std::vector<u8> out;
        nbt::Writer w(out);
        w.beginRoot();
        w.beginCompound("Level");
        w.writeInt("xPos", cx);
        w.writeInt("zPos", cz);
        w.writeByte("TerrainPopulated", 1);
        w.writeLong("LastUpdate", 7);
        w.writeByteArray("Blocks", blocks);
        w.writeByteArray("Data", nibbles);
        w.writeByteArray("BlockLight", nibbles);
        w.writeByteArray("SkyLight", nibbles);
        w.writeByteArray("HeightMap", heightMap);
        w.beginList("TileEntities", nbt::TagType::Compound);
        fill(w);
        w.endList();
        w.endCompound();
        w.endRoot();
        return out;
    }
};

const TileEntity* tileAt(const ChunkColumn& c, i32 x, int y, i32 z)
{
    return world::findTileEntity(c.tileEntities, x, y, z);
}

// Re-reads what encodeChunk produced, which is how every round trip below
// checks that what went out can come back.
bool reencode(const ChunkColumn& in, ChunkColumn* out)
{
    std::vector<u8> bytes;
    return alpha::encodeChunk(in, &bytes) && alpha::decodeChunk(bytes, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// The registry -- `ic`'s static block
// ---------------------------------------------------------------------------

TEST(the_four_registered_ids_map_both_ways)
{
    const char* names[] = {"Furnace", "Chest", "Sign", "MobSpawner"};
    for (int i = 0; i < 4; ++i) {
        TileEntityKind kind = TileEntityKind::Unknown;
        CHECK(world::tileEntityKindFromId(names[i], &kind));
        CHECK_EQ(int(kind), i);
        CHECK_EQ(std::string(world::tileEntityId(kind)), std::string(names[i]));
    }

    TileEntityKind kind = TileEntityKind::Chest;
    CHECK(!world::tileEntityKindFromId("Cauldron", &kind));
    CHECK(!world::tileEntityKindFromId("", &kind));
    CHECK(world::tileEntityId(TileEntityKind::Unknown) == nullptr);
}

// `ly.q[]` crossed with each container's `a_()`. Five blocks, four kinds.
TEST(each_container_block_names_its_tile_entity)
{
    struct Row {
        mcver::Block block;
        TileEntityKind kind;
    };
    const Row rows[] = {
        {mcver::Block::Chest, TileEntityKind::Chest},
        {mcver::Block::Furnace, TileEntityKind::Furnace},
        {mcver::Block::LitFurnace, TileEntityKind::Furnace},
        {mcver::Block::SignPost, TileEntityKind::Sign},
        {mcver::Block::WallSign, TileEntityKind::Sign},
        {mcver::Block::MobSpawner, TileEntityKind::MobSpawner},
    };
    for (const Row& row : rows) {
        TileEntityKind kind = TileEntityKind::Unknown;
        const block::TickBehaviour behaviour = block::def(bid(row.block)).tick;
        CHECK(block::tileEntityBearing(behaviour));
        CHECK(world::tileEntityKindForBlock(behaviour, &kind));
        CHECK_EQ(int(kind), int(row.kind));
    }

    TileEntityKind kind = TileEntityKind::Unknown;
    CHECK(!world::tileEntityKindForBlock(block::def(bid(mcver::Block::Stone)).tick, &kind));
    CHECK(!block::tileEntityBearing(block::def(bid(mcver::Block::Stone)).tick));
    // A block that ticks but holds nothing.
    CHECK(!block::tileEntityBearing(block::def(bid(mcver::Block::Sand)).tick));
}

TEST(a_fresh_spawner_is_a_pig_on_a_twenty_tick_delay_and_the_rest_are_blank)
{
    const TileEntity spawner = world::makeTileEntity(1, 2, 3, TileEntityKind::MobSpawner);
    CHECK_EQ(spawner.id, std::string("MobSpawner"));
    CHECK_EQ(spawner.entityId, std::string("Pig"));
    CHECK_EQ(spawner.delay, i16(20));

    const TileEntity chest = world::makeTileEntity(1, 2, 3, TileEntityKind::Chest);
    CHECK_EQ(chest.id, std::string("Chest"));
    CHECK(chest.items.empty());
    CHECK_EQ(chest.delay, i16(0));

    const TileEntity furnace = world::makeTileEntity(1, 2, 3, TileEntityKind::Furnace);
    CHECK_EQ(furnace.burnTime, i16(0));
    CHECK_EQ(furnace.cookTime, i16(0));

    const TileEntity sign = world::makeTileEntity(1, 2, 3, TileEntityKind::Sign);
    for (int i = 0; i < world::kTileSignLines; ++i) {
        CHECK_EQ(std::string(sign.lines[i]), std::string());
    }
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

TEST(a_position_holds_at_most_one_tile_entity)
{
    std::vector<TileEntity> list;
    world::putTileEntity(list, 4, 5, 6, TileEntityKind::Chest);
    CHECK_EQ(int(list.size()), 1);

    // `Chunk.setChunkBlockTileEntity` puts into a map keyed on the position.
    world::putTileEntity(list, 4, 5, 6, TileEntityKind::Furnace);
    CHECK_EQ(int(list.size()), 1);
    CHECK_EQ(int(list[0].kind), int(TileEntityKind::Furnace));

    world::putTileEntity(list, -4, 5, -6, TileEntityKind::Sign);
    CHECK_EQ(int(list.size()), 2);
    CHECK(tileAt(ChunkColumn(), 0, 0, 0) == nullptr);
    CHECK(world::findTileEntity(list, -4, 5, -6) != nullptr);
    CHECK(world::findTileEntity(list, -4, 5, 6) == nullptr);

    CHECK(world::eraseTileEntity(list, -4, 5, -6));
    CHECK(!world::eraseTileEntity(list, -4, 5, -6));
    CHECK_EQ(int(list.size()), 1);
}

TEST(a_sign_line_is_truncated_to_fifteen_and_always_terminated)
{
    TileEntity sign = world::makeTileEntity(0, 0, 0, TileEntityKind::Sign);
    world::setTileSignLine(sign, 0, "0123456789abcdefghij");
    CHECK_EQ(std::string(sign.lines[0]), std::string("0123456789abcde"));
    world::setTileSignLine(sign, 1, "short");
    CHECK_EQ(std::string(sign.lines[1]), std::string("short"));
    world::setTileSignLine(sign, 1, "");
    CHECK_EQ(std::string(sign.lines[1]), std::string());
    // Out of range is a no-op rather than a write past the array.
    world::setTileSignLine(sign, -1, "x");
    world::setTileSignLine(sign, world::kTileSignLines, "x");
    CHECK_EQ(std::string(sign.lines[0]), std::string("0123456789abcde"));
}

// ---------------------------------------------------------------------------
// The codec
// ---------------------------------------------------------------------------

TEST(all_four_kinds_round_trip_through_a_chunk_file)
{
    ChunkBuilder builder;
    builder.setBlock(1, 40, 2, u8(bid(mcver::Block::MobSpawner)));
    builder.setBlock(3, 41, 4, u8(bid(mcver::Block::Chest)));
    builder.setBlock(5, 42, 6, u8(bid(mcver::Block::Furnace)));
    builder.setBlock(7, 43, 8, u8(bid(mcver::Block::SignPost)));

    const std::vector<u8> file = builder.build(2, -3, [](nbt::Writer& w) {
        // **Not in `ic.b`'s call order.** A Java HashMap writes its members in
        // bucket order, so the subclass tags really can precede the `id` that
        // says what class they belong to.
        w.beginListElementCompound();
        w.writeShort("Delay", 143);
        w.writeString("EntityId", "Skeleton");
        w.writeString("id", "MobSpawner");
        w.writeInt("x", 2 * 16 + 1);
        w.writeInt("y", 40);
        w.writeInt("z", -3 * 16 + 2);
        w.endCompound();

        w.beginListElementCompound();
        w.writeString("id", "Chest");
        w.writeInt("x", 2 * 16 + 3);
        w.writeInt("y", 41);
        w.writeInt("z", -3 * 16 + 4);
        w.beginList("Items", nbt::TagType::Compound);
        w.beginListElementCompound();
        w.writeByte("Slot", 0);
        w.writeShort("id", 264);
        w.writeByte("Count", 3);
        w.writeShort("Damage", 0);
        w.endCompound();
        w.beginListElementCompound();
        w.writeByte("Slot", 26);
        w.writeShort("id", 276);
        w.writeByte("Count", 1);
        w.writeShort("Damage", 7);
        w.endCompound();
        w.endList();
        w.endCompound();

        w.beginListElementCompound();
        w.writeString("id", "Furnace");
        w.writeInt("x", 2 * 16 + 5);
        w.writeInt("y", 42);
        w.writeInt("z", -3 * 16 + 6);
        w.writeShort("BurnTime", 120);
        w.writeShort("CookTime", 33);
        w.beginList("Items", nbt::TagType::Compound);
        w.beginListElementCompound();
        w.writeByte("Slot", 1);
        w.writeShort("id", 263);
        w.writeByte("Count", 8);
        w.writeShort("Damage", 0);
        w.endCompound();
        w.endList();
        w.endCompound();

        w.beginListElementCompound();
        w.writeString("id", "Sign");
        w.writeInt("x", 2 * 16 + 7);
        w.writeInt("y", 43);
        w.writeInt("z", -3 * 16 + 8);
        w.writeString("Text1", "hello");
        w.writeString("Text2", "");
        w.writeString("Text3", "third line");
        w.writeString("Text4", "");
        w.endCompound();
    });

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(file, &chunk));
    CHECK_EQ(int(chunk.tileEntities.size()), 4);
    // It is out of `preserved` now, which is the whole change.
    CHECK(chunk.preserved.find("TileEntities") == nullptr);

    // Everything must survive being written out and read back in.
    ChunkColumn again;
    CHECK(reencode(chunk, &again));

    for (const ChunkColumn* c : {&chunk, &again}) {
        const TileEntity* spawner = tileAt(*c, 33, 40, -46);
        CHECK(spawner != nullptr);
        CHECK_EQ(int(spawner->kind), int(TileEntityKind::MobSpawner));
        CHECK_EQ(spawner->entityId, std::string("Skeleton"));
        CHECK_EQ(spawner->delay, i16(143));

        const TileEntity* chest = tileAt(*c, 35, 41, -44);
        CHECK(chest != nullptr);
        CHECK_EQ(int(chest->items.size()), 2);
        CHECK_EQ(int(chest->items[0].slot), 0);
        CHECK_EQ(int(chest->items[0].id), 264);
        CHECK_EQ(int(chest->items[0].count), 3);
        CHECK_EQ(int(chest->items[1].slot), 26);
        CHECK_EQ(int(chest->items[1].id), 276);
        CHECK_EQ(int(chest->items[1].damage), 7);

        const TileEntity* furnace = tileAt(*c, 37, 42, -42);
        CHECK(furnace != nullptr);
        CHECK_EQ(furnace->burnTime, i16(120));
        CHECK_EQ(furnace->cookTime, i16(33));
        CHECK_EQ(int(furnace->items.size()), 1);
        CHECK_EQ(int(furnace->items[0].slot), 1);

        const TileEntity* sign = tileAt(*c, 39, 43, -40);
        CHECK(sign != nullptr);
        CHECK_EQ(std::string(sign->lines[0]), std::string("hello"));
        CHECK_EQ(std::string(sign->lines[1]), std::string());
        CHECK_EQ(std::string(sign->lines[2]), std::string("third line"));
    }
}

// The point of modelling all four at once: writing the list back must not cost
// a chest its contents. Encode twice and the second file is the first.
TEST(a_second_save_is_identical_to_the_first)
{
    ChunkBuilder builder;
    builder.setBlock(3, 41, 4, u8(bid(mcver::Block::Chest)));
    const std::vector<u8> file = builder.build(0, 0, [](nbt::Writer& w) {
        w.beginListElementCompound();
        w.writeString("id", "Chest");
        w.writeInt("x", 3);
        w.writeInt("y", 41);
        w.writeInt("z", 4);
        w.beginList("Items", nbt::TagType::Compound);
        w.beginListElementCompound();
        w.writeByte("Slot", 5);
        w.writeShort("id", 50);
        w.writeByte("Count", 64);
        w.writeShort("Damage", 0);
        w.endCompound();
        w.endList();
        w.endCompound();
    });

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(file, &chunk));
    std::vector<u8> first;
    CHECK(alpha::encodeChunk(chunk, &first));

    ChunkColumn reloaded;
    CHECK(alpha::decodeChunk(first, &reloaded));
    std::vector<u8> second;
    CHECK(alpha::encodeChunk(reloaded, &second));
    CHECK(first == second);

    CHECK_EQ(int(reloaded.tileEntities.size()), 1);
    CHECK_EQ(int(reloaded.tileEntities[0].items.size()), 1);
    CHECK_EQ(int(reloaded.tileEntities[0].items[0].count), 64);
}

TEST(an_unknown_id_keeps_every_tag_it_arrived_with)
{
    ChunkBuilder builder;
    const std::vector<u8> file = builder.build(0, 0, [](nbt::Writer& w) {
        w.beginListElementCompound();
        w.writeString("id", "Cauldron");
        w.writeInt("x", 1);
        w.writeInt("y", 2);
        w.writeInt("z", 3);
        // A `Delay` that is not a Short and an `Items` that is not a list --
        // claiming either off the wrong class would fail the whole chunk.
        w.writeInt("Delay", 99);
        w.writeString("Items", "not a list");
        w.writeFloat("Level", 0.5f);
        w.endCompound();
    });

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(file, &chunk));
    CHECK_EQ(int(chunk.tileEntities.size()), 1);
    const TileEntity& tile = chunk.tileEntities[0];
    CHECK_EQ(int(tile.kind), int(TileEntityKind::Unknown));
    CHECK_EQ(tile.id, std::string("Cauldron"));
    CHECK_EQ(tile.x, 1);
    CHECK_EQ(int(tile.preserved.size()), 3);
    CHECK(tile.preserved.find("Delay") != nullptr);
    CHECK(tile.preserved.find("Items") != nullptr);
    CHECK(tile.preserved.find("Level") != nullptr);

    ChunkColumn again;
    CHECK(reencode(chunk, &again));
    CHECK_EQ(int(again.tileEntities.size()), 1);
    CHECK_EQ(again.tileEntities[0].id, std::string("Cauldron"));
    CHECK_EQ(int(again.tileEntities[0].preserved.size()), 3);
}

// A tag inside a known kind that this build does not model is kept too, and
// is not written twice.
TEST(an_unmodelled_tag_on_a_known_kind_is_carried_through_once)
{
    ChunkBuilder builder;
    builder.setBlock(1, 40, 2, u8(bid(mcver::Block::MobSpawner)));
    const std::vector<u8> file = builder.build(0, 0, [](nbt::Writer& w) {
        w.beginListElementCompound();
        w.writeString("id", "MobSpawner");
        w.writeInt("x", 1);
        w.writeInt("y", 40);
        w.writeInt("z", 2);
        w.writeString("EntityId", "Zombie");
        w.writeShort("Delay", 5);
        w.writeShort("MaxNearbyEntities", 6);  // a later version's
        w.endCompound();
    });

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(file, &chunk));
    CHECK_EQ(int(chunk.tileEntities[0].preserved.size()), 1);

    std::vector<u8> bytes;
    CHECK(alpha::encodeChunk(chunk, &bytes));
    // Once in, once out -- the field and the preserved copy must not both be
    // written.
    int delays = 0;
    for (usize i = 0; i + 7 < bytes.size(); ++i) {
        if (std::string(reinterpret_cast<const char*>(&bytes[i]), 5) == "Delay") {
            ++delays;
        }
    }
    CHECK_EQ(delays, 1);
}

// `ic.c(hm)` prints "Skipping TileEntity with id" and hands back null, and the
// chunk loader never puts a null in the map -- so it is never written back
// either. The one lossy case, and it is lossy in the original too.
TEST(an_element_with_no_id_or_no_position_is_skipped)
{
    ChunkBuilder builder;
    builder.setBlock(1, 40, 2, u8(bid(mcver::Block::MobSpawner)));
    const std::vector<u8> file = builder.build(0, 0, [](nbt::Writer& w) {
        w.beginListElementCompound();  // no id
        w.writeInt("x", 5);
        w.writeInt("y", 6);
        w.writeInt("z", 7);
        w.endCompound();
        w.beginListElementCompound();  // no y
        w.writeString("id", "Chest");
        w.writeInt("x", 5);
        w.writeInt("z", 7);
        w.endCompound();
        w.beginListElementCompound();  // an id of the wrong type is no id
        w.writeInt("id", 3);
        w.writeInt("x", 5);
        w.writeInt("y", 6);
        w.writeInt("z", 7);
        w.endCompound();
        w.beginListElementCompound();  // ...and one good one
        w.writeString("id", "MobSpawner");
        w.writeInt("x", 1);
        w.writeInt("y", 40);
        w.writeInt("z", 2);
        w.writeString("EntityId", "Spider");
        w.writeShort("Delay", 1);
        w.endCompound();
    });

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(file, &chunk));
    CHECK_EQ(int(chunk.tileEntities.size()), 1);
    CHECK_EQ(chunk.tileEntities[0].entityId, std::string("Spider"));
}

TEST(a_slot_outside_the_array_is_dropped)
{
    ChunkBuilder builder;
    builder.setBlock(3, 41, 4, u8(bid(mcver::Block::Chest)));
    builder.setBlock(5, 41, 4, u8(bid(mcver::Block::Furnace)));
    const std::vector<u8> file = builder.build(0, 0, [](nbt::Writer& w) {
        w.beginListElementCompound();
        w.writeString("id", "Chest");
        w.writeInt("x", 3);
        w.writeInt("y", 41);
        w.writeInt("z", 4);
        w.beginList("Items", nbt::TagType::Compound);
        for (int slot : {-1, 0, 26, 27, 35}) {
            w.beginListElementCompound();
            w.writeByte("Slot", i8(slot));
            w.writeShort("id", 1);
            w.writeByte("Count", 1);
            w.writeShort("Damage", 0);
            w.endCompound();
        }
        w.endList();
        w.endCompound();

        w.beginListElementCompound();
        w.writeString("id", "Furnace");
        w.writeInt("x", 5);
        w.writeInt("y", 41);
        w.writeInt("z", 4);
        w.beginList("Items", nbt::TagType::Compound);
        for (int slot : {0, 2, 3}) {
            w.beginListElementCompound();
            w.writeByte("Slot", i8(slot));
            w.writeShort("id", 1);
            w.writeByte("Count", 1);
            w.writeShort("Damage", 0);
            w.endCompound();
        }
        w.endList();
        w.endCompound();
    });

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(file, &chunk));
    // 27 slots, so -1, 27 and 35 all go; `fe`'s array is 36 long until it has
    // been read once, and nothing can ever reach past `c()` = 27.
    CHECK_EQ(int(tileAt(chunk, 3, 41, 4)->items.size()), 2);
    // 3 slots, so only 0 and 2 land.
    CHECK_EQ(int(tileAt(chunk, 5, 41, 4)->items.size()), 2);
}

TEST(a_chunk_with_no_tile_entities_tag_decodes_and_gains_an_empty_one)
{
    ChunkBuilder builder;
    std::vector<u8> out;
    nbt::Writer w(out);
    w.beginRoot();
    w.beginCompound("Level");
    w.writeInt("xPos", 0);
    w.writeInt("zPos", 0);
    w.writeByteArray("Blocks", builder.blocks);
    w.writeByteArray("Data", builder.nibbles);
    w.writeByteArray("BlockLight", builder.nibbles);
    w.writeByteArray("SkyLight", builder.nibbles);
    w.endCompound();
    w.endRoot();

    ChunkColumn chunk;
    CHECK(alpha::decodeChunk(out, &chunk));
    CHECK(chunk.tileEntities.empty());

    ChunkColumn again;
    CHECK(reencode(chunk, &again));
    CHECK(again.tileEntities.empty());
}

// ---------------------------------------------------------------------------
// Reconcile -- `ga.d`'s heal and `jt.b`'s removal, in bulk
// ---------------------------------------------------------------------------

TEST(a_container_block_with_no_entry_gains_a_default_one)
{
    ChunkColumn column(1, -2);
    column.setBlock(3, 41, 4, bid(mcver::Block::Chest));
    column.setBlock(5, 42, 6, bid(mcver::Block::MobSpawner));

    CHECK_EQ(world::reconcileTileEntities(column), 2);
    CHECK_EQ(int(column.tileEntities.size()), 2);

    const TileEntity* chest = tileAt(column, 1 * 16 + 3, 41, -2 * 16 + 4);
    CHECK(chest != nullptr);
    CHECK_EQ(int(chest->kind), int(TileEntityKind::Chest));

    const TileEntity* spawner = tileAt(column, 1 * 16 + 5, 42, -2 * 16 + 6);
    CHECK(spawner != nullptr);
    CHECK_EQ(spawner->entityId, std::string("Pig"));

    // Idempotent: a second pass finds nothing to do.
    CHECK_EQ(world::reconcileTileEntities(column), 0);
    CHECK_EQ(int(column.tileEntities.size()), 2);
}

TEST(an_entry_whose_block_is_gone_is_dropped_and_an_unknown_one_is_not)
{
    ChunkColumn column(0, 0);
    column.setBlock(3, 41, 4, bid(mcver::Block::Chest));
    CHECK_EQ(world::reconcileTileEntities(column), 1);
    column.tileEntities[0].items.push_back(item::ItemStack{});

    // A creeper takes the chest.
    column.setBlock(3, 41, 4, block::kAir);
    // ...and something this build has never heard of is standing beside it.
    TileEntity stranger;
    stranger.x = 6;
    stranger.y = 41;
    stranger.z = 7;
    stranger.id = "Cauldron";
    stranger.kind = TileEntityKind::Unknown;
    column.tileEntities.push_back(stranger);

    CHECK_EQ(world::reconcileTileEntities(column), 1);
    CHECK_EQ(int(column.tileEntities.size()), 1);
    CHECK_EQ(column.tileEntities[0].id, std::string("Cauldron"));
}

TEST(a_block_that_changed_container_is_rebuilt_rather_than_reused)
{
    ChunkColumn column(0, 0);
    column.setBlock(3, 41, 4, bid(mcver::Block::Chest));
    CHECK_EQ(world::reconcileTileEntities(column), 1);
    column.tileEntities[0].items.push_back(item::ItemStack{});

    column.setBlock(3, 41, 4, bid(mcver::Block::Furnace));
    CHECK_EQ(world::reconcileTileEntities(column), 2);  // one dropped, one made
    CHECK_EQ(int(column.tileEntities.size()), 1);
    CHECK_EQ(int(column.tileEntities[0].kind), int(TileEntityKind::Furnace));
    CHECK(column.tileEntities[0].items.empty());
}

// The negative-coordinate case every storage change owes: `>> 4` and `/ 16`
// disagree for a negative chunk, and a column whose blocks are all negative is
// where that shows.
TEST(reconcile_gets_negative_columns_right)
{
    ChunkColumn column(-1, -1);
    column.setBlock(0, 41, 0, bid(mcver::Block::Chest));
    column.setBlock(15, 41, 15, bid(mcver::Block::Chest));

    CHECK_EQ(world::reconcileTileEntities(column), 2);
    CHECK(tileAt(column, -16, 41, -16) != nullptr);
    CHECK(tileAt(column, -1, 41, -1) != nullptr);

    // An entry belonging to the next column along is not this column's to
    // judge, and is dropped rather than matched -- nothing puts one there.
    CHECK_EQ(world::reconcileTileEntities(column), 0);
}
