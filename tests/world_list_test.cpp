#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/chunk.hpp"
#include "core/world/world_list.hpp"
#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mc;
using world::WorldEntry;

namespace {

// Real files, for the reason storage_test gives: the bugs this layer has --
// a path built wrong, a directory left behind by a delete, a listing that
// depends on readdir order -- do not exist against a stub.
struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_list_XXXXXX");
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

// A world on disk, made the way the menu makes one.
void createWorld(io::FileSystem& fs, const std::string& dir, i64 seed, i64 lastPlayed)
{
    mcver::Storage storage(fs);
    CHECK(storage.create(dir, seed, lastPlayed) == world::OpenResult::Ok);
    CHECK(storage.close(lastPlayed));
}

}  // namespace

TEST(listing_an_absent_saves_folder_is_empty_rather_than_an_error)
{
    io::PosixFileSystem fs;
    std::vector<WorldEntry> worlds;
    world::listWorlds(fs, "/tmp/3dalpha_no_such_saves_dir", &worlds);
    CHECK(worlds.empty());
}

TEST(a_listing_reports_every_world_newest_first)
{
    TempDir temp;
    io::PosixFileSystem fs;

    createWorld(fs, temp.at("Alpha"), 111, 1000);
    createWorld(fs, temp.at("Beta"), 222, 3000);
    createWorld(fs, temp.at("Gamma"), 333, 2000);

    std::vector<WorldEntry> worlds;
    world::listWorlds(fs, temp.path, &worlds);

    CHECK_EQ(worlds.size(), usize(3));
    CHECK_EQ(worlds[0].name, std::string("Beta"));
    CHECK_EQ(worlds[1].name, std::string("Gamma"));
    CHECK_EQ(worlds[2].name, std::string("Alpha"));
    CHECK_EQ(worlds[0].seed, i64(222));
    CHECK_EQ(worlds[0].lastPlayed, i64(3000));
    CHECK_EQ(worlds[0].path, temp.at("Beta"));
}

TEST(worlds_played_at_the_same_moment_are_ordered_by_name)
{
    TempDir temp;
    io::PosixFileSystem fs;

    createWorld(fs, temp.at("zeta"), 1, 5000);
    createWorld(fs, temp.at("alpha"), 2, 5000);

    std::vector<WorldEntry> worlds;
    world::listWorlds(fs, temp.path, &worlds);

    CHECK_EQ(worlds.size(), usize(2));
    CHECK_EQ(worlds[0].name, std::string("alpha"));
    CHECK_EQ(worlds[1].name, std::string("zeta"));
}

TEST(a_directory_without_a_level_dat_is_not_a_world)
{
    TempDir temp;
    io::PosixFileSystem fs;

    createWorld(fs, temp.at("Real"), 7, 100);
    CHECK(fs.makeDirectories(temp.at("screenshots").c_str()));
    const std::vector<u8> junk{'h', 'i'};
    CHECK(fs.writeFileAtomic(temp.at("readme.txt").c_str(), junk));

    std::vector<WorldEntry> worlds;
    world::listWorlds(fs, temp.path, &worlds);

    CHECK_EQ(worlds.size(), usize(1));
    CHECK_EQ(worlds[0].name, std::string("Real"));
}

TEST(listing_a_world_does_not_touch_it)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string dir = temp.at("Untouched");
    createWorld(fs, dir, 42, 1234);

    // The whole reason listWorlds exists: open() writes session.lock and
    // close() rewrites level.dat, so a listing built on them would re-stamp
    // LastPlayed on a world the player only looked at.
    std::vector<u8> before;
    CHECK(fs.readFile((dir + "/level.dat").c_str(), &before, 1 << 20));

    std::vector<WorldEntry> worlds;
    world::listWorlds(fs, temp.path, &worlds);
    CHECK_EQ(worlds.size(), usize(1));

    std::vector<u8> after;
    CHECK(fs.readFile((dir + "/level.dat").c_str(), &after, 1 << 20));
    CHECK(before == after);
}

TEST(a_new_world_gets_the_first_free_slot_name)
{
    std::vector<WorldEntry> worlds;
    CHECK_EQ(world::defaultWorldName(worlds), std::string("World1"));

    WorldEntry one;
    one.name = "World1";
    worlds.push_back(one);
    CHECK_EQ(world::defaultWorldName(worlds), std::string("World2"));

    WorldEntry three;
    three.name = "World3";
    worlds.push_back(three);
    CHECK_EQ(world::defaultWorldName(worlds), std::string("World2"));
}

TEST(a_typed_world_name_is_made_safe_for_a_card)
{
    std::string name;

    CHECK(world::sanitizeWorldName("My World", &name));
    CHECK_EQ(name, std::string("My World"));

    CHECK(world::sanitizeWorldName("  spaced   out  ", &name));
    CHECK_EQ(name, std::string("spaced out"));

    CHECK(world::sanitizeWorldName("a/b\\c:d*e?f\"g<h>i|j", &name));
    CHECK_EQ(name, std::string("abcdefghij"));

    // Windows will not open a directory whose name ends in a dot, and a card is
    // read on a PC as often as on a console.
    CHECK(world::sanitizeWorldName("trailing...", &name));
    CHECK_EQ(name, std::string("trailing"));

    CHECK(!world::sanitizeWorldName("", &name));
    CHECK(!world::sanitizeWorldName("...", &name));
    CHECK(!world::sanitizeWorldName("///", &name));
    CHECK(!world::sanitizeWorldName("   ", &name));
}

TEST(deleting_a_world_removes_its_whole_tree)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string dir = temp.at("Doomed");
    createWorld(fs, dir, 5, 100);

    // A chunk, so the base36 tree exists and the delete has to recurse.
    {
        mcver::Storage storage(fs);
        CHECK(storage.open(dir, 100) == world::OpenResult::Ok);
        world::ChunkColumn chunk(-13, 44);
        chunk.setBlock(0, 0, 0, world::BlockId(1));
        CHECK(storage.saveChunk(chunk));
        CHECK(storage.close(100));
    }
    CHECK(fs.exists((dir + "/1f/18/c.-d.18.dat").c_str()));

    CHECK(world::deleteWorld(fs, dir));
    CHECK(!fs.exists(dir.c_str()));

    std::vector<WorldEntry> worlds;
    world::listWorlds(fs, temp.path, &worlds);
    CHECK(worlds.empty());
}

TEST(delete_refuses_anything_that_is_not_a_world)
{
    TempDir temp;
    io::PosixFileSystem fs;

    createWorld(fs, temp.at("Keep"), 1, 100);

    // The saves folder itself holds no level.dat, so this is the caller bug
    // that would otherwise take every world with it.
    CHECK(!world::deleteWorld(fs, temp.path));
    CHECK(fs.exists(temp.at("Keep").c_str()));

    CHECK(!world::deleteWorld(fs, temp.at("no_such_world")));
}
