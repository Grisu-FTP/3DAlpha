#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/settings/world_settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mc;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_ws_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }
};

void writeText(io::FileSystem& fs, const std::string& path, const char* text)
{
    const usize length = std::strlen(text);
    CHECK(fs.writeFileAtomic(path.c_str(),
                             ConstByteSpan(reinterpret_cast<const u8*>(text), length)));
}

}  // namespace

TEST(world_settings_round_trip)
{
    TempDir temp;
    io::PosixFileSystem fs;

    settings::WorldSettings written;
    written.gamemode = settings::Gamemode::Creative;
    written.difficulty = settings::Difficulty::Hard;
    CHECK(settings::saveWorldSettings(fs, temp.path, written));

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Creative);
    CHECK(read.difficulty == settings::Difficulty::Hard);
}

TEST(the_difficulty_defaults_to_normal_and_survives_a_file_that_only_names_a_gamemode)
{
    // **Normal is a1.1.2's own default** -- `Minecraft.difficulty` is
    // initialised to 2 -- and every world written before the monsters landed
    // has a settings file with no `difficulty=` line at all. Reading one must
    // leave the default rather than fall to Peaceful, which would silently
    // remove every monster in a world that had never chosen to.
    TempDir temp;
    io::PosixFileSystem fs;
    settings::WorldSettings fresh;
    CHECK(fresh.difficulty == settings::Difficulty::Normal);

    writeText(fs, settings::worldSettingsPath(temp.path), "gamemode=creative\n");
    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Creative);
    CHECK(read.difficulty == settings::Difficulty::Normal);

    // And a word this build does not know keeps the default rather than
    // failing the load -- the same bargain the gamemode row takes, because a
    // card can be edited on a PC.
    writeText(fs, settings::worldSettingsPath(temp.path),
              "gamemode=creative\ndifficulty=nightmare\n");
    settings::WorldSettings later;
    CHECK(settings::loadWorldSettings(fs, temp.path, &later));
    CHECK(later.difficulty == settings::Difficulty::Normal);

    // The four tokens are stable words, not ordinals.
    CHECK_EQ(std::string(settings::difficultyToken(settings::Difficulty::Peaceful)),
             std::string("peaceful"));
    CHECK_EQ(std::string(settings::difficultyToken(settings::Difficulty::Hard)),
             std::string("hard"));
    CHECK_EQ(std::string(settings::difficultyLabel(settings::Difficulty::Normal)),
             std::string("Normal"));
    // And their ordinals are `cn.l`'s, so a table indexed by one stays in step.
    CHECK_EQ(int(settings::Difficulty::Peaceful), 0);
    CHECK_EQ(int(settings::Difficulty::Hard), 3);
}

TEST(a_world_with_no_settings_file_is_the_ordinary_case)
{
    TempDir temp;
    io::PosixFileSystem fs;

    // Every world that predates this feature, and every Alpha save copied in
    // from a PC. False means "there was no file", not "something went wrong",
    // and the defaults have to be usable either way.
    settings::WorldSettings read;
    read.gamemode = settings::Gamemode::Creative;
    CHECK(!settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Spectator);
}

TEST(reading_a_world_does_not_write_a_settings_file)
{
    TempDir temp;
    io::PosixFileSystem fs;

    settings::WorldSettings read;
    settings::loadWorldSettings(fs, temp.path, &read);

    // Merely looking at a world must not touch the card: the world list reads
    // every world on it.
    CHECK(!fs.exists(settings::worldSettingsPath(temp.path).c_str()));
}

TEST(every_gamemode_is_implemented)
{
    CHECK(settings::gamemodeImplemented(settings::Gamemode::Spectator));
    // Creative landed with M3 step 3, Survival with step 4: health, break
    // progress, wear, spent stacks and the container screens.
    CHECK(settings::gamemodeImplemented(settings::Gamemode::Creative));
    CHECK(settings::gamemodeImplemented(settings::Gamemode::Survival));

    CHECK_EQ(std::string(settings::gamemodeToken(settings::Gamemode::Spectator)),
             std::string("spectator"));
    CHECK_EQ(std::string(settings::gamemodeLabel(settings::Gamemode::Spectator)),
             std::string("Spectator"));
}

TEST(an_unknown_gamemode_keeps_the_default)
{
    TempDir temp;
    io::PosixFileSystem fs;

    // A word a later build wrote, or one somebody mistyped on a PC. Neither is
    // worth failing the load over, and neither may be guessed at.
    writeText(fs, settings::worldSettingsPath(temp.path), "gamemode=adventure\n");

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Spectator);
}

TEST(a_hand_edited_world_settings_file_still_loads)
{
    TempDir temp;
    io::PosixFileSystem fs;

    writeText(fs, settings::worldSettingsPath(temp.path),
              "# edited on a PC\r\n"
              "\r\n"
              "   gamemode  =  survival  \r\n"
              "this line has no equals sign\r\n"
              "unknown_key=7\r\n");

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Survival);
}

TEST(saving_drops_keys_this_build_does_not_know)
{
    TempDir temp;
    io::PosixFileSystem fs;

    writeText(fs, settings::worldSettingsPath(temp.path),
              "gamemode=creative\nfrom_a_later_build=yes\n");

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(settings::saveWorldSettings(fs, temp.path, read));

    std::vector<u8> bytes;
    CHECK(fs.readFile(settings::worldSettingsPath(temp.path).c_str(), &bytes, 8 * 1024));
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(text.find("gamemode=creative") != std::string::npos);
    CHECK(text.find("from_a_later_build") == std::string::npos);
}

TEST(the_settings_path_sits_inside_the_world_folder)
{
    // It has to be a sibling of level.dat, because that is the folder a real
    // client ignores everything else in.
    CHECK_EQ(settings::worldSettingsPath("saves/World1"),
             std::string("saves/World1/3dalpha.ini"));
    CHECK_EQ(settings::worldSettingsPath("saves/World1/"),
             std::string("saves/World1/3dalpha.ini"));
}

// The Extra Settings rows survive a round trip, and every one of them defaults
// to off -- which is what makes a world that predates the screen, and every
// Alpha save copied in from a PC, read as plain a1.1.2.
TEST(the_extra_settings_round_trip_and_default_to_vanilla)
{
    TempDir temp;
    io::PosixFileSystem fs;

    settings::WorldSettings fresh;
    CHECK(!fresh.fixOreGeneration);
    CHECK(!fresh.fixBedrockHole);
    CHECK(!fresh.improvedFencePlacement);
    CHECK(fresh.texturePack.empty());
    CHECK_EQ(int(fresh.panoramaTileX), 0);
    CHECK_EQ(int(fresh.panoramaTileZ), 0);
    CHECK(fresh.panoramaAnchor == settings::PanoramaAnchor::Spawn);

    settings::WorldSettings written;
    written.fixOreGeneration = true;
    written.fixBedrockHole = true;
    written.improvedFencePlacement = true;
    written.texturePack = "Faithful.zip";
    written.panoramaTileX = -7;
    written.panoramaTileZ = 13;
    written.panoramaAnchor = settings::PanoramaAnchor::Player;
    CHECK(settings::saveWorldSettings(fs, temp.path, written));

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.fixOreGeneration);
    CHECK(read.fixBedrockHole);
    CHECK(read.improvedFencePlacement);
    CHECK_EQ(read.texturePack, std::string("Faithful.zip"));
    CHECK_EQ(int(read.panoramaTileX), -7);
    CHECK_EQ(int(read.panoramaTileZ), 13);
    CHECK(read.panoramaAnchor == settings::PanoramaAnchor::Player);

    // The gamemode and difficulty rows are untouched by any of it.
    CHECK(read.gamemode == settings::Gamemode::Spectator);
    CHECK(read.difficulty == settings::Difficulty::Normal);
}

// A file written before the screen existed has none of these keys, and an
// older build's file is the ordinary case on a card that has been carried
// between two versions of this port. Every missing key has to read as off.
TEST(a_settings_file_without_the_extra_keys_reads_as_vanilla)
{
    TempDir temp;
    io::PosixFileSystem fs;

    writeText(fs, settings::worldSettingsPath(temp.path),
              "gamemode=creative\ndifficulty=hard\n");

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Creative);
    CHECK(read.difficulty == settings::Difficulty::Hard);
    CHECK(!read.fixOreGeneration);
    CHECK(!read.fixBedrockHole);
    CHECK(!read.improvedFencePlacement);
    CHECK(read.texturePack.empty());
    CHECK_EQ(int(read.panoramaTileX), 0);
    CHECK_EQ(int(read.panoramaTileZ), 0);
    // **Spawn, not Origin.** A world written before the anchor key existed had
    // its table on block 0, 0 because that was the only place there was -- but
    // the table has since moved to spawn for every such world, and reading the
    // old file as Origin would put it back where it could see nothing.
    CHECK(read.panoramaAnchor == settings::PanoramaAnchor::Spawn);
}

// The anchor is a word in the file for the reason gamemode and difficulty are:
// a file a later build wrote, with an anchor this build has never heard of,
// keeps the default here instead of being an ordinal that means something else.
TEST(the_panorama_anchor_is_a_word_and_an_unknown_one_keeps_the_default)
{
    TempDir temp;
    io::PosixFileSystem fs;

    settings::PanoramaAnchor anchor = settings::PanoramaAnchor::Spawn;
    CHECK(settings::panoramaAnchorFromToken("origin", &anchor));
    CHECK(anchor == settings::PanoramaAnchor::Origin);
    CHECK(settings::panoramaAnchorFromToken("player", &anchor));
    CHECK(anchor == settings::PanoramaAnchor::Player);
    CHECK(settings::panoramaAnchorFromToken("spawn", &anchor));
    CHECK(anchor == settings::PanoramaAnchor::Spawn);

    // Every token round-trips through its own label-free spelling.
    const settings::PanoramaAnchor all[3] = {settings::PanoramaAnchor::Spawn,
                                             settings::PanoramaAnchor::Origin,
                                             settings::PanoramaAnchor::Player};
    for (settings::PanoramaAnchor one : all) {
        settings::PanoramaAnchor back = settings::PanoramaAnchor::Spawn;
        CHECK(settings::panoramaAnchorFromToken(settings::panoramaAnchorToken(one), &back));
        CHECK(back == one);
    }

    writeText(fs, settings::worldSettingsPath(temp.path),
              "gamemode=creative\npanorama_anchor=the_nether_portal\npanorama_tile_x=2\n");
    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.panoramaAnchor == settings::PanoramaAnchor::Spawn);
    CHECK_EQ(int(read.panoramaTileX), 2);
}

// `true`/`false` is what the game writes; the other four spellings are there
// because this is a file a player edits by hand on a PC, and a word none of
// them matches keeps the default rather than failing the load.
TEST(the_extra_switches_accept_the_spellings_a_person_would_type)
{
    TempDir temp;
    io::PosixFileSystem fs;

    writeText(fs, settings::worldSettingsPath(temp.path),
              "fix_ore_generation=yes\nfix_bedrock_hole=1\n");
    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.fixOreGeneration);
    CHECK(read.fixBedrockHole);

    writeText(fs, settings::worldSettingsPath(temp.path),
              "fix_ore_generation=no\nfix_bedrock_hole=off\n");
    settings::WorldSettings again;
    CHECK(settings::loadWorldSettings(fs, temp.path, &again));
    CHECK(!again.fixOreGeneration);
    // "off" is not one of the six, so the key is ignored and the default
    // stands -- which for a switch that is off by default is the same answer,
    // and is deliberately not an error.
    CHECK(!again.fixBedrockHole);

    CHECK_EQ(std::string(settings::boolToken(true)), std::string("true"));
    CHECK_EQ(std::string(settings::boolToken(false)), std::string("false"));
    bool value = false;
    CHECK(!settings::boolFromToken("maybe", &value));
}

// **Empty is not Dev Art here.** The console's own setting spells Dev Art as
// an empty pack name; a world needs a third answer -- follow the console --
// and that is what empty means, so the built-in art gets a token of its own.
TEST(a_world_pack_tells_default_apart_from_the_built_in_art)
{
    TempDir temp;
    io::PosixFileSystem fs;

    settings::WorldSettings devArt;
    devArt.texturePack = settings::kWorldPackDevArt;
    CHECK(settings::saveWorldSettings(fs, temp.path, devArt));

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK_EQ(read.texturePack, std::string(settings::kWorldPackDevArt));
    CHECK(!read.texturePack.empty());

    settings::WorldSettings follow;
    CHECK(settings::saveWorldSettings(fs, temp.path, follow));
    settings::WorldSettings back;
    CHECK(settings::loadWorldSettings(fs, temp.path, &back));
    CHECK(back.texturePack.empty());
}
