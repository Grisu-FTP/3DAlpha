#include "framework.hpp"

#include "core/io/posix_file_system.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mc;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_fs_XXXXXX");
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

struct Names {
    std::vector<std::string> files;
    std::vector<std::string> dirs;

    static bool visit(void* context, const io::DirEntry& entry)
    {
        auto& self = *static_cast<Names*>(context);
        (entry.isDirectory ? self.dirs : self.files).push_back(entry.name);
        return true;
    }

    static bool stopAtFirst(void* context, const io::DirEntry& entry)
    {
        static_cast<Names*>(context)->files.push_back(entry.name);
        return false;
    }
};

}  // namespace

TEST(files_round_trip_through_the_posix_backend)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string path = temp.at("hello.bin");
    const u8 payload[] = {0, 1, 2, 250, 251, 255};

    CHECK(!fs.exists(path.c_str()));
    CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan(payload, sizeof(payload))));
    CHECK(fs.exists(path.c_str()));
    CHECK(!fs.isDirectory(path.c_str()));

    std::vector<u8> read;
    CHECK(fs.readFile(path.c_str(), &read, 1024));
    CHECK_EQ(read.size(), usize(sizeof(payload)));
    CHECK(read == std::vector<u8>(payload, payload + sizeof(payload)));

    // readFile appends, so several files can be concatenated into one buffer.
    CHECK(fs.readFile(path.c_str(), &read, 1024));
    CHECK_EQ(read.size(), usize(2 * sizeof(payload)));
}

TEST(an_atomic_write_replaces_and_leaves_no_temporary)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("data.bin");

    const u8 first[] = {1, 1, 1, 1};
    const u8 second[] = {2, 2};
    CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan(first, sizeof(first))));
    CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan(second, sizeof(second))));

    std::vector<u8> read;
    CHECK(fs.readFile(path.c_str(), &read, 1024));
    CHECK_EQ(read.size(), usize(2));
    CHECK_EQ(read[0], u8(2));

    // A leftover .tmp would accumulate one file per save on a card that already
    // wastes a cluster per chunk.
    CHECK(!fs.exists((path + ".tmp").c_str()));
}

TEST(an_empty_file_is_a_file_not_a_failure)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("empty.bin");

    CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan()));
    CHECK(fs.exists(path.c_str()));

    std::vector<u8> read;
    CHECK(fs.readFile(path.c_str(), &read, 1024));
    CHECK(read.empty());
}

TEST(read_refuses_a_file_over_the_ceiling)
{
    // A world folder holds whatever the user put there; a huge file named like
    // a chunk should be a refusal, not an allocation.
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("big.bin");

    std::vector<u8> payload(4096, 0xAB);
    CHECK(fs.writeFileAtomic(path.c_str(), payload));

    std::vector<u8> read;
    CHECK(!fs.readFile(path.c_str(), &read, 1024));
    CHECK(read.empty());
    CHECK(fs.readFile(path.c_str(), &read, 4096));
}

TEST(reading_a_missing_file_or_a_directory_fails_cleanly)
{
    TempDir temp;
    io::PosixFileSystem fs;

    std::vector<u8> read;
    CHECK(!fs.readFile(temp.at("nope.bin").c_str(), &read, 1024));
    // A directory opens fine on POSIX, so the regular-file check is what stops
    // it from being read as a file.
    CHECK(!fs.readFile(temp.path, &read, 1024));
    CHECK(read.empty());
}

TEST(make_directories_creates_every_component_and_is_idempotent)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string deep = temp.at("a/b/c");

    CHECK(fs.makeDirectories(deep.c_str()));
    CHECK(fs.isDirectory(deep.c_str()));
    CHECK(fs.isDirectory(temp.at("a").c_str()));
    CHECK(fs.makeDirectories(deep.c_str()));

    // A file in the way is a real failure, not something to plough through.
    CHECK(fs.writeFileAtomic(temp.at("a/file").c_str(), ConstByteSpan()));
    CHECK(!fs.makeDirectories(temp.at("a/file/d").c_str()));
}

TEST(directory_listing_separates_files_from_directories)
{
    TempDir temp;
    io::PosixFileSystem fs;

    CHECK(fs.makeDirectories(temp.at("sub").c_str()));
    CHECK(fs.writeFileAtomic(temp.at("one.dat").c_str(), ConstByteSpan()));
    CHECK(fs.writeFileAtomic(temp.at("two.dat").c_str(), ConstByteSpan()));

    Names names;
    CHECK(fs.listDirectory(temp.path, &names, Names::visit));
    CHECK_EQ(names.files.size(), usize(2));
    CHECK_EQ(names.dirs.size(), usize(1));
    CHECK_EQ(names.dirs[0], std::string("sub"));

    // "." and ".." must never reach the visitor, or the world scan recurses
    // into itself.
    for (const std::string& name : names.files) {
        CHECK(name != "." && name != "..");
    }

    Names stopped;
    CHECK(fs.listDirectory(temp.path, &stopped, Names::stopAtFirst));
    CHECK_EQ(stopped.files.size(), usize(1));

    CHECK(!fs.listDirectory(temp.at("missing").c_str(), &names, Names::visit));
}

TEST(remove_file_is_idempotent)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("gone.bin");

    CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan()));
    CHECK(fs.removeFile(path.c_str()));
    CHECK(!fs.exists(path.c_str()));
    // Removing what is already gone is the outcome the caller wanted.
    CHECK(fs.removeFile(path.c_str()));
}
