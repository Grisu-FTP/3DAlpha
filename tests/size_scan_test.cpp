#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/chunk.hpp"
#include "core/world/size_scan.hpp"
#include "version_slots.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace mc;
using world::SizeScan;
using world::WorldSize;

namespace {

// Real files, for world_list_test's reason: what this layer gets wrong -- a
// walk that never stops, a total published before it is finished -- does not
// exist against a stub.
struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_scan_XXXXXX");
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

// A world with `chunks` columns in it, so the tree the scan walks has depth.
void createWorld(io::FileSystem& fs, const std::string& dir, int chunks)
{
    mcver::Storage storage(fs);
    CHECK(storage.create(dir, 7, 100) == world::OpenResult::Ok);
    CHECK(storage.close(100));

    CHECK(storage.open(dir, 100) == world::OpenResult::Ok);
    for (int i = 0; i < chunks; ++i) {
        world::ChunkColumn chunk(i, -i);
        chunk.setBlock(0, 0, 0, world::BlockId(1));
        CHECK(storage.saveChunk(chunk));
    }
    CHECK(storage.close(100));
}

// The scan runs on a thread of its own, so a test waits for it rather than
// assuming a scheduler. Bounded, so a scan that never finishes fails rather
// than hangs.
bool waitForScan(SizeScan& scan)
{
    for (int i = 0; i < 2000 && scan.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !scan.running();
}

}  // namespace

TEST(a_scan_measures_the_same_world_worldSize_does)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string dir = temp.at("Measured");
    createWorld(fs, dir, 16);

    WorldSize expected;
    CHECK(world::worldSize(fs, dir, &expected));

    SizeScan scan;
    scan.start(fs, dir);
    CHECK(waitForScan(scan));
    CHECK(scan.state() == SizeScan::State::Done);

    WorldSize measured;
    CHECK(scan.result(&measured));
    CHECK(measured.contentBytes == expected.contentBytes);
    CHECK(measured.onDiskBytes == expected.onDiskBytes);
    CHECK(measured.fileCount == expected.fileCount);
    CHECK(measured.directoryCount == expected.directoryCount);
}

TEST(there_is_no_number_to_read_until_the_walk_has_finished)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string dir = temp.at("Pending");
    createWorld(fs, dir, 4);

    SizeScan scan;
    CHECK(scan.state() == SizeScan::State::Idle);

    WorldSize measured;
    CHECK(!scan.result(&measured));  // nothing has been asked for yet

    scan.start(fs, dir);
    CHECK(waitForScan(scan));
    CHECK(scan.result(&measured));
    CHECK(measured.fileCount > 0);
}

TEST(cancelling_drops_the_walk_rather_than_reporting_a_partial_total)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string dir = temp.at("Cancelled");
    createWorld(fs, dir, 64);

    SizeScan scan;
    scan.start(fs, dir);
    scan.cancel();

    CHECK(!scan.running());
    // Either it finished before the cancel landed -- a small world on a warm
    // page cache -- or it stopped. What must never happen is a half-walked
    // total offered as an answer.
    CHECK(scan.state() == SizeScan::State::Idle || scan.state() == SizeScan::State::Done);

    WorldSize measured;
    if (scan.state() == SizeScan::State::Idle) {
        CHECK(!scan.result(&measured));
    }
}

TEST(starting_again_replaces_what_the_scan_is_about)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string first = temp.at("Small");
    const std::string second = temp.at("Large");
    createWorld(fs, first, 1);
    createWorld(fs, second, 32);

    SizeScan scan;
    scan.start(fs, first);
    scan.start(fs, second);  // cancels and joins the first
    CHECK(waitForScan(scan));

    WorldSize measured;
    CHECK(scan.result(&measured));

    WorldSize expected;
    CHECK(world::worldSize(fs, second, &expected));
    CHECK(measured.fileCount == expected.fileCount);
}

TEST(a_world_that_is_not_there_fails_rather_than_measuring_nothing)
{
    io::PosixFileSystem fs;

    SizeScan scan;
    scan.start(fs, "/tmp/3dalpha_no_such_world_at_all");
    CHECK(waitForScan(scan));
    CHECK(scan.state() == SizeScan::State::Failed);

    WorldSize measured;
    CHECK(!scan.result(&measured));

    // An empty path is the same answer without a thread: the screen has no
    // world selected.
    SizeScan nowhere;
    nowhere.start(fs, "");
    CHECK(nowhere.state() == SizeScan::State::Failed);
}

TEST(a_scan_left_running_is_joined_by_the_destructor)
{
    TempDir temp;
    io::PosixFileSystem fs;

    const std::string dir = temp.at("Abandoned");
    createWorld(fs, dir, 32);

    {
        SizeScan scan;
        scan.start(fs, dir);
    }
    // Nothing to check beyond arriving here: a scan that outlived its object
    // would be walking freed memory, which the sanitiser builds catch.
    CHECK(true);
}
