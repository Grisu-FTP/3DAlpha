#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/format/region_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::world::format;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_rgn_XXXXXX");
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

std::vector<u8> payload(usize bytes, u8 seed)
{
    std::vector<u8> out(bytes);
    for (usize i = 0; i < bytes; ++i) {
        out[i] = u8(seed + u8(i * 7));
    }
    return out;
}

ConstByteSpan span(const std::vector<u8>& v)
{
    return ConstByteSpan(v.data(), v.size());
}

// Fails every write or flush after the Nth, so a commit can be torn at each of
// its three steps and the previous generation checked for survival. A power cut
// on a handheld is the ordinary case, not the exotic one.
class FaultyFile : public io::RandomAccessFile {
public:
    FaultyFile(std::unique_ptr<io::RandomAccessFile> inner, int allowedOps)
        : inner_(std::move(inner)), allowed_(allowedOps)
    {
    }

    bool readAt(u64 offset, ByteSpan out) override { return inner_->readAt(offset, out); }

    bool writeAt(u64 offset, ConstByteSpan data) override
    {
        if (spend()) {
            return false;
        }
        return inner_->writeAt(offset, data);
    }

    bool size(u64* out) override { return inner_->size(out); }

    bool flush() override
    {
        if (spend()) {
            return false;
        }
        return inner_->flush();
    }

private:
    bool spend()
    {
        if (allowed_ <= 0) {
            return true;
        }
        --allowed_;
        return false;
    }

    std::unique_ptr<io::RandomAccessFile> inner_;
    int allowed_ = 0;
};

class FaultyFileSystem : public io::PosixFileSystem {
public:
    int allowedOps = 1 << 30;

    std::unique_ptr<io::RandomAccessFile> openRandomAccess(const char* path,
                                                           bool create) override
    {
        auto inner = io::PosixFileSystem::openRandomAccess(path, create);
        if (inner == nullptr) {
            return nullptr;
        }
        return std::unique_ptr<io::RandomAccessFile>(
            new FaultyFile(std::move(inner), allowedOps));
    }
};

}  // namespace

TEST(region_coordinates_floor_rather_than_truncate)
{
    // Chunk -1 belongs to region -1. Truncating would put it in region 0
    // alongside chunk 0, and half the coordinates in a world are negative.
    CHECK_EQ(regionCoord(0), 0);
    CHECK_EQ(regionCoord(31), 0);
    CHECK_EQ(regionCoord(32), 1);
    CHECK_EQ(regionCoord(-1), -1);
    CHECK_EQ(regionCoord(-32), -1);
    CHECK_EQ(regionCoord(-33), -2);
    CHECK_EQ(regionCoord(-1000), -32);

    // Every chunk of a region maps to a distinct slot, negatives included.
    CHECK_EQ(regionSlot(0, 0), u32(0));
    CHECK_EQ(regionSlot(31, 31), u32(1023));
    CHECK_EQ(regionSlot(-1, -1), u32(31 + 31 * 32));
    CHECK_EQ(regionSlot(-32, -32), u32(0));
    CHECK(regionSlot(-1, -1) != regionSlot(-2, -1));
}

TEST(a_region_round_trips_payloads_at_negative_coordinates)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.-1.-1.3dr");

    const std::vector<u8> a = payload(2917, 1);   // the measured median
    const std::vector<u8> b = payload(5872, 2);   // the measured largest
    const std::vector<u8> c = payload(1194, 3);   // the measured smallest

    {
        RegionFile region;
        CHECK(region.open(fs, path.c_str(), -1, -1, true));
        CHECK(region.write(-1, -1, span(a)));
        CHECK(region.write(-32, -32, span(b)));
        CHECK(region.write(-17, -3, span(c)));
        CHECK(region.commit());
        CHECK_EQ(region.chunkCount(), u32(3));
        CHECK(region.close());
    }

    RegionFile region;
    CHECK(region.open(fs, path.c_str(), -1, -1, false));
    CHECK(region.has(-1, -1));
    CHECK(!region.has(-2, -2));

    std::vector<u8> read;
    CHECK(region.read(-1, -1, &read));
    CHECK(read == a);
    read.clear();
    CHECK(region.read(-32, -32, &read));
    CHECK(read == b);
    read.clear();
    CHECK(region.read(-17, -3, &read));
    CHECK(read == c);
}

TEST(a_region_survives_being_filled_completely)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.0.0.3dr");

    RegionFile region;
    CHECK(region.open(fs, path.c_str(), 0, 0, true));
    for (i32 z = 0; z < kRegionChunks; ++z) {
        for (i32 x = 0; x < kRegionChunks; ++x) {
            const std::vector<u8> bytes = payload(1200 + usize(x + z), u8(x * 31 + z));
            CHECK(region.write(x, z, span(bytes)));
        }
    }
    CHECK(region.commit());
    CHECK_EQ(region.chunkCount(), kRegionArea);

    std::vector<u8> read;
    CHECK(region.read(17, 29, &read));
    CHECK_EQ(read.size(), usize(1200 + 17 + 29));
}

TEST(a_rewritten_chunk_never_overwrites_the_live_copy)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.0.0.3dr");

    const std::vector<u8> small = payload(1200, 9);
    const std::vector<u8> large = payload(9000, 4);

    RegionFile region;
    CHECK(region.open(fs, path.c_str(), 0, 0, true));
    CHECK(region.write(5, 5, span(small)));
    CHECK(region.commit());

    u32 firstLength = 0;
    u32 firstCrc = 0;
    CHECK(region.payloadInfo(5, 5, &firstLength, &firstCrc));
    CHECK_EQ(firstLength, u32(1200));

    // Growing forces a new run; the committed one has to stay intact until the
    // commit that stops referring to it.
    CHECK(region.write(5, 5, span(large)));
    CHECK(region.commit());

    std::vector<u8> read;
    CHECK(region.read(5, 5, &read));
    CHECK(read == large);

    // Shrinking again, and the sectors the large copy held come back: a region
    // that only ever grew would be a leak with extra steps.
    const u32 grownSectors = region.sectorCount();
    CHECK(region.write(5, 5, span(small)));
    CHECK(region.commit());
    CHECK(region.sectorCount() <= grownSectors);

    read.clear();
    CHECK(region.read(5, 5, &read));
    CHECK(read == small);
}

TEST(freed_sectors_are_handed_out_again)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.0.0.3dr");

    RegionFile region;
    CHECK(region.open(fs, path.c_str(), 0, 0, true));

    const std::vector<u8> bytes = payload(3000, 7);
    CHECK(region.write(0, 0, span(bytes)));
    CHECK(region.write(1, 0, span(bytes)));
    CHECK(region.commit());
    const u32 full = region.sectorCount();

    CHECK(region.erase(0, 0));
    CHECK(region.commit());
    CHECK(!region.has(0, 0));

    // The same size goes back into the hole rather than off the end.
    CHECK(region.write(2, 0, span(bytes)));
    CHECK(region.commit());
    CHECK_EQ(region.sectorCount(), full);
}

TEST(saving_one_chunk_repeatedly_between_commits_does_not_leak_the_region)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.0.0.3dr");

    RegionFile region;
    CHECK(region.open(fs, path.c_str(), 0, 0, true));

    const std::vector<u8> bytes = payload(3000, 5);
    CHECK(region.write(0, 0, span(bytes)));
    CHECK(region.commit());

    // Only the first write after a commit holds a run the committed directory
    // names; every one after it replaces a run that was never durable and must
    // be reclaimed at once. So the file grows by a bounded couple of runs and
    // then stops, however many times the same chunk is saved.
    for (int i = 0; i < 30; ++i) {
        CHECK(region.write(0, 0, span(bytes)));
    }
    CHECK(region.commit());
    const u32 afterThirty = region.sectorCount();

    for (int i = 0; i < 60; ++i) {
        CHECK(region.write(0, 0, span(bytes)));
    }
    CHECK(region.commit());

    // Twice the writes, not one sector more. That is the difference between
    // reusing the vacated run and leaking it.
    CHECK_EQ(region.sectorCount(), afterThirty);
    CHECK_EQ(region.chunkCount(), u32(1));
}

TEST(a_torn_commit_leaves_the_previous_generation_whole)
{
    TempDir temp;
    const std::string path = temp.at("r.0.0.3dr");

    const std::vector<u8> first = payload(2000, 1);
    const std::vector<u8> second = payload(2500, 2);

    {
        io::PosixFileSystem fs;
        RegionFile region;
        CHECK(region.open(fs, path.c_str(), 0, 0, true));
        CHECK(region.write(3, 4, span(first)));
        CHECK(region.commit());
        CHECK(region.close());
    }

    // Cut power after each of the writes and flushes a second commit takes.
    // Whichever step is torn, reopening has to find the first generation
    // intact -- losing the last save is the bar; losing the region is not.
    for (int allowed = 0; allowed < 8; ++allowed) {
        char command[256];
        std::snprintf(command, sizeof(command), "cp '%s' '%s.torn'", path.c_str(),
                      path.c_str());
        CHECK_EQ(std::system(command), 0);
        const std::string torn = path + ".torn";

        {
            FaultyFileSystem faulty;
            faulty.allowedOps = allowed;
            RegionFile region;
            if (region.open(faulty, torn.c_str(), 0, 0, false)) {
                region.write(9, 9, span(second));
                region.commit();
            }
        }

        io::PosixFileSystem fs;
        RegionFile reopened;
        CHECK(reopened.open(fs, torn.c_str(), 0, 0, false));
        std::vector<u8> read;
        CHECK(reopened.read(3, 4, &read));
        CHECK(read == first);

        // The new chunk either landed whole or is absent. A half-written one
        // would mean the directory pointed at bytes that were never finished.
        if (reopened.has(9, 9)) {
            std::vector<u8> newer;
            CHECK(reopened.read(9, 9, &newer));
            CHECK(newer == second);
        }
    }
}

TEST(a_region_with_both_headers_destroyed_is_refused_rather_than_guessed_at)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.0.0.3dr");

    const std::vector<u8> bytes = payload(2000, 3);
    const std::vector<u8> later = payload(2400, 4);
    {
        RegionFile region;
        CHECK(region.open(fs, path.c_str(), 0, 0, true));
        CHECK(region.write(1, 1, span(bytes)));
        CHECK(region.commit());
        // A second commit, so that the chunk above is present in *both* live
        // generations. Losing the newest generation is the documented cost of
        // a torn header; losing a chunk that predates it would not be.
        CHECK(region.write(2, 2, span(later)));
        CHECK(region.close());
    }

    // One torn header is survivable, which is the whole point of keeping two.
    {
        auto file = fs.openRandomAccess(path.c_str(), false);
        CHECK(file != nullptr);
        const u8 rubbish[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(file->writeAt(0, ConstByteSpan(rubbish, sizeof(rubbish))));
    }
    {
        RegionFile region;
        CHECK(region.open(fs, path.c_str(), 0, 0, false));
        std::vector<u8> read;
        CHECK(region.read(1, 1, &read));
        CHECK(read == bytes);
    }

    // Both gone, and there is no honest answer left to give.
    {
        auto file = fs.openRandomAccess(path.c_str(), false);
        CHECK(file != nullptr);
        const u8 rubbish[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(file->writeAt(0, ConstByteSpan(rubbish, sizeof(rubbish))));
        CHECK(file->writeAt(u64(kSectorBytes), ConstByteSpan(rubbish, sizeof(rubbish))));
    }
    RegionFile region;
    CHECK(!region.open(fs, path.c_str(), 0, 0, false));
}

TEST(a_missing_region_is_only_created_when_asked_for)
{
    TempDir temp;
    io::PosixFileSystem fs;

    RegionFile region;
    CHECK(!region.open(fs, temp.at("r.5.5.3dr").c_str(), 5, 5, false));
    CHECK(!fs.exists(temp.at("r.5.5.3dr").c_str()));

    // A region created and never written is still a valid region, not an empty
    // file: the header goes down at open so a power cut a moment later leaves
    // something readable.
    CHECK(region.open(fs, temp.at("r.5.5.3dr").c_str(), 5, 5, true));
    CHECK(region.close());

    RegionFile reopened;
    CHECK(reopened.open(fs, temp.at("r.5.5.3dr").c_str(), 5, 5, false));
    CHECK_EQ(reopened.chunkCount(), u32(0));
}

TEST(a_walk_reports_every_chunk_as_world_coordinates)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.-2.1.3dr");

    RegionFile region;
    CHECK(region.open(fs, path.c_str(), -2, 1, true));
    const std::vector<u8> bytes = payload(1500, 8);
    CHECK(region.write(-64, 32, span(bytes)));   // slot 0 of region (-2, 1)
    CHECK(region.write(-33, 63, span(bytes)));   // slot 1023
    CHECK(region.commit());

    struct Seen {
        std::vector<std::pair<i32, i32>> chunks;
        static bool visit(void* context, i32 x, i32 z)
        {
            static_cast<Seen*>(context)->chunks.emplace_back(x, z);
            return true;
        }
    } seen;

    CHECK(region.forEachChunk(&seen, Seen::visit));
    CHECK_EQ(seen.chunks.size(), usize(2));
    CHECK_EQ(seen.chunks[0].first, -64);
    CHECK_EQ(seen.chunks[0].second, 32);
    CHECK_EQ(seen.chunks[1].first, -33);
    CHECK_EQ(seen.chunks[1].second, 63);
}

TEST(staged_writes_reach_the_card_without_a_commit_per_chunk)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string path = temp.at("r.0.0.3dr");

    // The bound that stops an unflushing caller losing an unbounded amount of
    // world. It has to be high enough never to fire in ordinary play -- the
    // cache flushes on a 45-second timer -- and low enough to be a real bound.
    RegionFile region;
    CHECK(region.open(fs, path.c_str(), 0, 0, true));
    const std::vector<u8> bytes = payload(1500, 2);
    for (i32 i = 0; i < 100; ++i) {
        CHECK(region.write(i % kRegionChunks, i / kRegionChunks, span(bytes)));
    }
    CHECK(region.close());

    RegionFile reopened;
    CHECK(reopened.open(fs, path.c_str(), 0, 0, false));
    CHECK_EQ(reopened.chunkCount(), u32(100));
}
