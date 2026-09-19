// PosixFileSystem::makeDirectories: what it creates and what it leaves alone.
//
// The device prefix is the case that matters. A CIA build's working directory
// is the SD root, so libctru turns mkdir("sdmc:") into creating "/". That fails
// with an error that is not EEXIST, and every world, pack and chunk folder
// failed with it. A 3DSX build hid this because its working directory is the
// launch folder, which exists. Linux has no devices, but a relative "dev:"
// resolves against the working directory just as libctru's does. So the test
// runs in an empty directory and checks that no "dev:" folder appears.

#include "framework.hpp"

#include "core/io/posix_file_system.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace mc;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_mkdir_XXXXXX");
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

}  // namespace

TEST(make_directories_tolerates_doubled_and_trailing_slashes)
{
    TempDir temp;
    io::PosixFileSystem fs;
    CHECK(fs.makeDirectories(temp.at("a//b/").c_str()));
    CHECK(fs.isDirectory(temp.at("a/b").c_str()));
}

TEST(make_directories_never_creates_the_device_prefix)
{
    TempDir temp;
    io::PosixFileSystem fs;
    char previous[1024];
    CHECK(::getcwd(previous, sizeof(previous)) != nullptr);
    CHECK(::chdir(temp.path) == 0);

    // With the prefix treated as the root, nothing below it exists, so this
    // fails. The old walk created "dev:" first and then carried on.
    CHECK(!fs.makeDirectories("dev:/saves/World1"));
    CHECK(!fs.exists("dev:"));

    // A bare device is the root itself and needs nothing done.
    CHECK(fs.makeDirectories("dev:"));
    CHECK(fs.makeDirectories("dev:/"));
    CHECK(!fs.exists("dev:"));

    CHECK(::chdir(previous) == 0);
}

TEST(make_directories_treats_a_later_colon_as_part_of_a_name)
{
    TempDir temp;
    io::PosixFileSystem fs;
    CHECK(fs.makeDirectories(temp.at("a:b/c").c_str()));
    CHECK(fs.isDirectory(temp.at("a:b/c").c_str()));
}
