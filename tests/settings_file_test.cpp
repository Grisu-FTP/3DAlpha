#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/settings/settings_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mc;
using settings::ControlScheme;
using settings::GameSettings;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_ini_XXXXXX");
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

    std::string at(const char* name) const { return std::string(path) + "/" + name; }
};

void writeText(io::FileSystem& fs, const std::string& path, const char* text)
{
    const std::string content(text);
    fs.writeFileAtomic(path.c_str(), ConstByteSpan(
                                         reinterpret_cast<const u8*>(content.data()),
                                         content.size()));
}

TEST(settings_round_trip)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    GameSettings written;
    written.renderDistance = 10;
    written.texturePack = "minecraft-a1.1.2_01-client.zip";
    written.skin = "file:Steve.png";
    written.lookSensitivity = 135;
    written.controlScheme = ControlScheme::Old3DSAlt;
    CHECK(settings::saveSettings(fs, path.c_str(), written));

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK_EQ(read.renderDistance, 10);
    CHECK_EQ(read.lookSensitivity, 135);
    // The Controls row is a word in the file rather than a number, so the round
    // trip is through `controlSchemeToken` -- and a file that was written by
    // this build has been asked, which is what the flag says.
    CHECK(read.controlSchemeChosen);
    CHECK(read.controlScheme == ControlScheme::Old3DSAlt);
    CHECK_EQ(read.texturePack, written.texturePack);
    // The skin key carries a prefix and a colon, which is the one value in this
    // file that is not a bare name or a number -- and `key=value` splits on the
    // first `=`, so the colon has to be nothing special.
    CHECK_EQ(read.skin, written.skin);
}

// Dev Art is the empty string, and it has to survive the round trip as one --
// a blank value must not be read back as "no setting" and replaced by whatever
// pack the card happens to hold.
TEST(the_built_in_pack_round_trips_as_empty)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    GameSettings written;
    written.renderDistance = 6;
    CHECK(settings::saveSettings(fs, path.c_str(), written));

    GameSettings read;
    read.texturePack = "leftover.zip";
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(read.texturePack.empty());
    CHECK_EQ(read.renderDistance, 6);
}

// A file written before the Sensitivity row existed has no key for it, and the
// menu has to be able to tell that from a player who chose a value. -1 is the
// convention the autosave timer already uses; the row turns it into 100%, which
// is the rate that build already had.
TEST(a_settings_file_from_before_the_sensitivity_row_says_so_rather_than_reading_as_zero)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path, "render_distance=8\nmusic_volume=100\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK_EQ(read.renderDistance, 8);
    // Not 0, which is a real setting on this row -- it is `*yawn*`.
    CHECK_EQ(read.lookSensitivity, -1);
}

// The same question for the Controls row, which cannot answer it with a
// sentinel value: every scheme is a real answer, so "nobody has been asked" is
// a flag of its own and the menu is what fills the row in -- with the console
// model, which this file cannot see.
TEST(a_settings_file_from_before_the_controls_row_says_it_has_no_scheme)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path, "render_distance=8\nlook_sensitivity=100\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(!read.controlSchemeChosen);
}

// And a card edited on a PC to a word this build has never heard of. The flag
// stays false, so the menu fills the row in rather than the file quietly
// meaning a scheme it did not name.
TEST(an_unknown_controls_word_in_the_file_is_not_a_chosen_scheme)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path, "controls=wii-u\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(!read.controlSchemeChosen);
}

// The first boot on a console that has never run this. Not an error, and the
// defaults have to be intact afterwards.
TEST(a_missing_file_is_the_first_boot_state)
{
    TempDir dir;
    io::PosixFileSystem fs;

    GameSettings read;
    read.renderDistance = 99;
    read.texturePack = "stale.zip";
    CHECK(!settings::loadSettings(fs, dir.at("absent.ini").c_str(), &read));
    CHECK_EQ(read.renderDistance, 0);
    CHECK(read.texturePack.empty());
}

// A card is edited on a PC. Comments, blank lines, whitespace and Windows line
// endings all have to survive, and a line that means nothing has to be skipped
// rather than failing the load and losing the lines around it.
TEST(a_hand_edited_file_still_loads)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path,
              "# written by hand\r\n"
              "\r\n"
              "  render_distance = 8  \r\n"
              "this line has no equals sign\r\n"
              "future_key=something\r\n"
              "texture_pack = My Pack.zip \r\n"
              "render_distance=nonsense\r\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    // The good line wins and the malformed one below it is ignored rather than
    // taken as a zero.
    CHECK_EQ(read.renderDistance, 8);
    CHECK_EQ(read.texturePack, std::string("My Pack.zip"));
}

// An edited ini must not be able to point the loader outside the packs folder.
TEST(a_pack_name_with_a_separator_is_ignored)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path, "texture_pack=../../../boot.firm\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(read.texturePack.empty());

    writeText(fs, path, "texture_pack=sub\\dir.zip\n");
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(read.texturePack.empty());
}

// The skin key names a file inside one of two known folders, so it gets the
// same guard for the same reason.
TEST(a_skin_key_with_a_separator_is_ignored)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path, "skin=file:../../../boot.firm\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(read.skin.empty());

    writeText(fs, path, "skin=pack:sub\\dir.zip\n");
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(read.skin.empty());

    // And an ordinary one still gets through.
    writeText(fs, path, "skin=pack:Faithful.zip\n");
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK_EQ(read.skin, std::string("pack:Faithful.zip"));
}

// Stated in the header so it is a decision rather than a surprise: saving
// rewrites the file from the keys this build knows.
TEST(saving_drops_keys_this_build_does_not_know)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("3ds.ini");

    writeText(fs, path, "render_distance=4\nfuture_key=kept?\n");

    GameSettings read;
    CHECK(settings::loadSettings(fs, path.c_str(), &read));
    CHECK(settings::saveSettings(fs, path.c_str(), read));

    std::vector<u8> bytes;
    CHECK(fs.readFile(path.c_str(), &bytes, 64 * 1024));
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(text.find("render_distance=4") != std::string::npos);
    CHECK(text.find("future_key") == std::string::npos);
}

}  // namespace
