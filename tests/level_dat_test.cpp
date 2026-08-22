#include "framework.hpp"

#include "core/nbt/writer.hpp"
#include "core/world/level_data.hpp"
#include "impl/storage/alpha_chunkfiles/level_dat.hpp"

#include <vector>

using namespace mc;
using item::ItemStack;
using world::LevelData;

namespace {

struct BuildOptions {
    bool player = true;
    bool inventory = true;
    bool extraDataTag = false;
    bool extraPlayerTag = false;
    bool rootSibling = false;
    bool healthAsInt = false;
    bool shortPos = false;
    // Off by default because a real a1.1.2 level.dat has no Dimension tag.
    bool dimension = false;
    // The original writes this on every save, whatever its value, so it is
    // always present -- unlike Dimension. See the note by the writeByte below.
    bool snowCovered = false;
};

// Written in the order encodeLevelDat emits, so a faithful round trip is
// byte-identical. Note that this says nothing about real saves: the original
// game keeps compounds in a HashMap and writes them in hash order.
std::vector<u8> buildLevelDat(const BuildOptions& opt = {})
{
    std::vector<u8> out;
    nbt::Writer w(out);
    w.beginRoot();
    w.beginCompound("Data");
    w.writeLong("LastPlayed", 1284768000000LL);
    w.writeLong("SizeOnDisk", 1234567LL);
    w.writeLong("RandomSeed", -4172144997902289642LL);
    w.writeInt("SpawnX", 132);
    w.writeInt("SpawnY", 67);
    w.writeInt("SpawnZ", -244);
    w.writeLong("Time", 41200LL);
    // **Always written, never optional.** a1.1.2's World.saveLevel emits
    // SnowCovered unconditionally, so a level.dat without it is one we wrote
    // wrongly rather than one the game could produce. It is modelled rather
    // than preserved because the terrain generator reads it.
    w.writeByte("SnowCovered", opt.snowCovered ? 1 : 0);

    if (opt.player) {
        w.beginCompound("Player");
        if (opt.dimension) {
            w.writeInt("Dimension", 0);
        }

        w.beginList("Pos", nbt::TagType::Double);
        w.listDouble(132.5);
        w.listDouble(68.62000000476837);
        if (!opt.shortPos) {
            // A two-element Pos is not something we could write back correctly,
            // so the decoder has to refuse it rather than guess a third.
            w.listDouble(-244.5);
        }
        w.endList();

        w.beginList("Rotation", nbt::TagType::Float);
        w.listFloat(-134.7f);
        w.listFloat(12.15f);
        w.endList();

        w.beginList("Motion", nbt::TagType::Double);
        w.listDouble(0.0);
        w.listDouble(-0.0784000015258789);
        w.listDouble(0.0);
        w.endList();

        w.writeByte("OnGround", 1);
        w.writeFloat("FallDistance", 0.0f);
        if (opt.healthAsInt) {
            w.writeInt("Health", 20);
        } else {
            w.writeShort("Health", 17);
        }
        w.writeShort("AttackTime", 0);
        w.writeShort("HurtTime", 3);
        w.writeShort("DeathTime", 0);
        w.writeShort("Air", 300);
        w.writeShort("Fire", -20);
        w.writeInt("Score", 0);

        // The declared type does not survive an empty list -- endList rewrites
        // it to Byte, matching the original game.
        w.beginList("Inventory", nbt::TagType::Compound);
        if (opt.inventory) {
            w.beginListElementCompound();
            w.writeByte("Slot", 0);
            w.writeShort("id", 278);
            w.writeByte("Count", 1);
            w.writeShort("Damage", 42);
            w.endCompound();

            w.beginListElementCompound();
            w.writeByte("Slot", 27);
            w.writeShort("id", 4);
            w.writeByte("Count", 64);
            w.writeShort("Damage", 0);
            w.endCompound();
        }
        w.endList();

        if (opt.extraPlayerTag) {
            w.writeInt("SomeServerTag", 7);
        }
        w.endCompound();
    }

    if (opt.extraDataTag) {
        w.writeString("LevelName", "a name a later version would add");
    }
    w.endCompound();
    if (opt.rootSibling) {
        w.writeByte("StrayRootTag", 1);
    }
    w.endRoot();
    return out;
}

}  // namespace

TEST(level_dat_fields_decode)
{
    LevelData level;
    CHECK(alpha::decodeLevelDat(buildLevelDat(), &level));

    CHECK_EQ(level.lastPlayed, 1284768000000LL);
    CHECK_EQ(level.sizeOnDisk, 1234567LL);
    CHECK_EQ(level.randomSeed, -4172144997902289642LL);
    CHECK_EQ(level.spawnX, 132);
    CHECK_EQ(level.spawnY, 67);
    CHECK_EQ(level.spawnZ, -244);
    CHECK_EQ(level.time, 41200LL);

    CHECK(level.player.present);
    CHECK_EQ(level.player.pos[0], 132.5);
    CHECK_EQ(level.player.pos[1], 68.62000000476837);
    CHECK_EQ(level.player.pos[2], -244.5);
    CHECK_EQ(level.player.rotation[0], -134.7f);
    CHECK_EQ(level.player.rotation[1], 12.15f);
    CHECK_EQ(level.player.motion[1], -0.0784000015258789);
    CHECK_EQ(level.player.onGround, true);
    CHECK_EQ(level.player.health, i16(17));
    CHECK_EQ(level.player.hurtTime, i16(3));
    CHECK_EQ(level.player.fire, i16(-20));

    CHECK_EQ(level.player.inventory.size(), usize(2));
    CHECK_EQ(level.player.inventory[0].slot, i8(0));
    CHECK_EQ(level.player.inventory[0].id, i16(278));
    CHECK_EQ(level.player.inventory[0].count, i8(1));
    CHECK_EQ(level.player.inventory[0].damage, i16(42));
    CHECK_EQ(level.player.inventory[1].slot, i8(27));
    CHECK_EQ(level.player.inventory[1].id, i16(4));
    CHECK_EQ(level.player.inventory[1].count, i8(64));
}

// SnowCovered is the one level.dat field the *terrain generator* reads -- it
// decides whether every ocean in the world gets a lid of ice -- so it is
// modelled rather than round-tripped as an opaque tag. Both values have to
// survive, and the false case is the one that would pass by accident if the
// field were silently dropped.
TEST(snow_covered_round_trips_in_both_states)
{
    for (const bool snowy : {false, true}) {
        BuildOptions opt;
        opt.snowCovered = snowy;
        const std::vector<u8> original = buildLevelDat(opt);

        LevelData level;
        CHECK(alpha::decodeLevelDat(original, &level));
        CHECK_EQ(level.snowCovered, snowy);

        std::vector<u8> encoded;
        CHECK(alpha::encodeLevelDat(level, &encoded));
        CHECK(encoded == original);
    }
}

// A level.dat with no SnowCovered at all is not something a1.1.2 can write, but
// it is something a third-party tool might. It must decode as false rather than
// failing, and then gain the tag on save -- which is what the original would do
// with the same file.
TEST(a_level_dat_without_snow_covered_decodes_as_false_and_gains_the_tag)
{
    std::vector<u8> without;
    {
        nbt::Writer w(without);
        w.beginRoot();
        w.beginCompound("Data");
        w.writeLong("LastPlayed", 1LL);
        w.writeLong("SizeOnDisk", 0LL);
        w.writeLong("RandomSeed", 42LL);
        w.writeInt("SpawnX", 0);
        w.writeInt("SpawnY", 64);
        w.writeInt("SpawnZ", 0);
        w.writeLong("Time", 0LL);
        w.endCompound();
        w.endRoot();
    }

    LevelData level;
    CHECK(alpha::decodeLevelDat(without, &level));
    CHECK_EQ(level.snowCovered, false);

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(level, &encoded));
    CHECK(encoded != without);

    // And the gained tag reads back as false, rather than as whatever byte
    // happened to be there.
    LevelData again;
    CHECK(alpha::decodeLevelDat(encoded, &again));
    CHECK_EQ(again.snowCovered, false);
}

TEST(level_dat_round_trips_byte_for_byte)
{
    const std::vector<u8> original = buildLevelDat();

    LevelData level;
    CHECK(alpha::decodeLevelDat(original, &level));

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(level, &encoded));
    CHECK(encoded == original);
}

TEST(a_dimension_tag_is_neither_invented_nor_dropped)
{
    // a1.1.2 does not write Dimension; it arrived with the Nether in a1.2.0.
    // Adding one would change every world this client touches.
    LevelData plain;
    const std::vector<u8> withoutIt = buildLevelDat();
    CHECK(alpha::decodeLevelDat(withoutIt, &plain));
    CHECK(!plain.player.hasDimension);

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(plain, &encoded));
    CHECK(encoded == withoutIt);

    // A later version's file keeps it.
    BuildOptions opt;
    opt.dimension = true;
    const std::vector<u8> withIt = buildLevelDat(opt);

    LevelData later;
    CHECK(alpha::decodeLevelDat(withIt, &later));
    CHECK(later.player.hasDimension);

    encoded.clear();
    CHECK(alpha::encodeLevelDat(later, &encoded));
    CHECK(encoded == withIt);
}

TEST(an_empty_inventory_round_trips_as_an_empty_list)
{
    // An empty NBT list carries Byte as its element type, not TAG_End: Java's
    // NBTTagList.write() takes the type from the first element and falls back
    // to 1 when there is none. Confirmed against a real a1.1.2 level.dat.
    BuildOptions opt;
    opt.inventory = false;
    const std::vector<u8> original = buildLevelDat(opt);

    LevelData level;
    CHECK(alpha::decodeLevelDat(original, &level));
    CHECK(level.player.inventory.empty());

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(level, &encoded));
    CHECK(encoded == original);

    // Pin the element type itself, since a semantic diff cannot see it: a list
    // with a zero count compares equal whatever type byte precedes it.
    const std::string_view marker("\x09\x00\x09Inventory", 12);
    const std::string_view text(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    const usize at = text.find(marker);
    CHECK(at != std::string_view::npos);
    CHECK_EQ(encoded[at + marker.size()], u8(nbt::TagType::Byte));
}

TEST(an_absent_player_stays_absent)
{
    // Server-created worlds have no Player compound. Inventing one would move
    // the player to 0,0,0 the next time the world is opened on a PC.
    BuildOptions opt;
    opt.player = false;
    const std::vector<u8> original = buildLevelDat(opt);

    LevelData level;
    CHECK(alpha::decodeLevelDat(original, &level));
    CHECK(!level.player.present);

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(level, &encoded));
    CHECK(encoded == original);
}

TEST(unmodelled_level_tags_survive_a_load_and_save)
{
    BuildOptions opt;
    opt.extraDataTag = true;
    opt.extraPlayerTag = true;
    opt.rootSibling = true;
    const std::vector<u8> original = buildLevelDat(opt);

    LevelData level;
    CHECK(alpha::decodeLevelDat(original, &level));
    CHECK(level.preserved.find("LevelName") != nullptr);
    CHECK(level.player.preserved.find("SomeServerTag") != nullptr);
    CHECK(level.preservedRoot.find("StrayRootTag") != nullptr);

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(level, &encoded));
    CHECK(encoded == original);
}

TEST(a_modelled_tag_with_the_wrong_type_is_rejected)
{
    // Health as an Int means this is not an a1.1.2 level.dat. Loading it as one
    // and saving would write a mangled file back over someone's world.
    BuildOptions opt;
    opt.healthAsInt = true;
    LevelData level;
    CHECK(!alpha::decodeLevelDat(buildLevelDat(opt), &level));

    BuildOptions shortPos;
    shortPos.shortPos = true;
    CHECK(!alpha::decodeLevelDat(buildLevelDat(shortPos), &level));
}

TEST(a_failed_level_dat_decode_leaves_the_target_alone)
{
    LevelData level;
    CHECK(alpha::decodeLevelDat(buildLevelDat(), &level));

    BuildOptions bad;
    bad.healthAsInt = true;
    CHECK(!alpha::decodeLevelDat(buildLevelDat(bad), &level));

    CHECK_EQ(level.spawnX, 132);
    CHECK_EQ(level.player.health, i16(17));
}

TEST(truncated_level_dat_fails_cleanly_at_every_length)
{
    const std::vector<u8> full = buildLevelDat();
    for (usize len = 0; len < full.size(); ++len) {
        LevelData level;
        CHECK(!alpha::decodeLevelDat(ConstByteSpan(full.data(), len), &level));
    }
}

TEST(corrupted_level_dat_fails_cleanly)
{
    std::vector<u8> full = buildLevelDat();
    for (usize i = 0; i < full.size(); ++i) {
        const u8 original = full[i];
        for (u8 flip : {u8(0x00), u8(0xFF), u8(0x7F)}) {
            full[i] = flip;
            LevelData level;
            alpha::decodeLevelDat(full, &level);
        }
        full[i] = original;
    }
}

TEST(an_edited_level_saves_what_was_edited)
{
    LevelData level;
    CHECK(alpha::decodeLevelDat(buildLevelDat(), &level));

    level.time = 18000;
    level.player.pos[1] = 90.0;
    level.player.health = 20;
    level.player.inventory.push_back(ItemStack{});
    level.player.inventory.back().slot = 8;
    level.player.inventory.back().id = 50;
    level.player.inventory.back().count = 12;

    std::vector<u8> encoded;
    CHECK(alpha::encodeLevelDat(level, &encoded));

    LevelData reloaded;
    CHECK(alpha::decodeLevelDat(encoded, &reloaded));
    CHECK_EQ(reloaded.time, 18000LL);
    CHECK_EQ(reloaded.player.pos[1], 90.0);
    CHECK_EQ(reloaded.player.health, i16(20));
    CHECK_EQ(reloaded.player.inventory.size(), usize(3));
    CHECK_EQ(reloaded.player.inventory[2].id, i16(50));
    CHECK_EQ(reloaded.player.inventory[2].count, i8(12));
}
