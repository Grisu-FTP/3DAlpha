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
    CHECK(settings::saveWorldSettings(fs, temp.path, written));

    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, temp.path, &read));
    CHECK(read.gamemode == settings::Gamemode::Creative);
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

TEST(spectator_is_the_only_implemented_gamemode)
{
    CHECK(settings::gamemodeImplemented(settings::Gamemode::Spectator));
    CHECK(!settings::gamemodeImplemented(settings::Gamemode::Survival));
    CHECK(!settings::gamemodeImplemented(settings::Gamemode::Creative));

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
