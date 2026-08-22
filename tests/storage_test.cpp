#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/chunk.hpp"
#include "impl/storage/alpha_chunkfiles/storage.hpp"
#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

using namespace mc;
using alpha::AlphaChunkFileStorage;
using world::BlockId;
using world::ChunkColumn;
using world::OpenResult;

namespace {

// Storage tests run against real files rather than a stubbed filesystem. The
// bugs this layer produces -- a path built wrong, a directory not created, a
// rename that does not replace -- only exist against a real one, and the POSIX
// implementation is the same code the console runs.
struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_test_XXXXXX");
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

    std::string world(const char* name) const { return std::string(path) + "/" + name; }
};

constexpr i64 kNow = 1284768000000LL;

ChunkColumn makeChunk(i32 x, i32 z, BlockId fill)
{
    ChunkColumn chunk(x, z);
    for (int lx = 0; lx < ChunkColumn::kWidth; ++lx) {
        for (int lz = 0; lz < ChunkColumn::kWidth; ++lz) {
            for (int y = 0; y < 40; ++y) {
                chunk.setBlock(lx, y, lz, fill);
            }
            for (int y = 40; y < ChunkColumn::kHeight; ++y) {
                chunk.setSkyLight(lx, y, lz, 15);
            }
            chunk.heightMap[usize(lz) * ChunkColumn::kWidth + usize(lx)] = 40;
        }
    }
    chunk.setBlock(3, 12, 9, 56);
    chunk.setBlockData(3, 12, 9, 7);
    chunk.lastUpdate = 99;
    chunk.terrainPopulated = true;
    chunk.compact();
    return chunk;
}

struct Collected {
    std::vector<u64> keys;

    // Shifting a negative signed value is undefined, and half these
    // coordinates are negative, so the pack goes through unsigned.
    static u64 key(i32 x, i32 z) { return (u64(u32(x)) << 32) | u64(u32(z)); }

    static bool visit(void* context, i32 x, i32 z)
    {
        static_cast<Collected*>(context)->keys.push_back(key(x, z));
        return true;
    }

    bool has(i32 x, i32 z) const
    {
        const u64 wanted = key(x, z);
        for (u64 k : keys) {
            if (k == wanted) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST(the_version_manifest_binds_the_storage_slot)
{
    // Pins the whole anti-hardcoding path from versions/a1.1.2.json through
    // configure.py to a usable type, with no vtable in between. A manifest
    // pointing at a slot that does not exist fails here rather than at link
    // time in a 3DS build.
    static_assert(std::is_same_v<mcver::Storage, AlphaChunkFileStorage>,
                  "the manifest should have bound storage to alpha_chunkfiles");

    io::PosixFileSystem fs;
    mcver::Storage storage(fs);
    CHECK(!storage.isOpen());
}

TEST(creating_a_world_writes_level_dat_and_the_lock)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');

    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);

    const std::string dir = temp.world("New World");
    CHECK(storage.create(dir, 1234567890LL, kNow) == OpenResult::Ok);
    CHECK(storage.isOpen());
    CHECK(fs.exists((dir + "/level.dat").c_str()));
    CHECK(fs.exists((dir + "/session.lock").c_str()));
    CHECK(storage.lockStillOurs());
    CHECK(storage.close(kNow));

    // And re-opening finds what was written.
    AlphaChunkFileStorage reopened(fs);
    CHECK(reopened.open(dir, kNow + 1000) == OpenResult::Ok);
    CHECK_EQ(reopened.level().randomSeed, 1234567890LL);
    CHECK_EQ(reopened.level().lastPlayed, kNow);
}

TEST(creating_over_an_existing_world_is_refused)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");

    AlphaChunkFileStorage first(fs);
    CHECK(first.create(dir, 1, kNow) == OpenResult::Ok);
    CHECK(first.close(kNow));

    // Silently overwriting would destroy a world. The caller has to delete it.
    AlphaChunkFileStorage second(fs);
    CHECK(second.create(dir, 2, kNow) == OpenResult::IoError);
}

TEST(opening_something_that_is_not_a_world_says_so)
{
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);

    CHECK(storage.open(temp.world("nothing here"), kNow) == OpenResult::NotAWorld);
    CHECK(!storage.isOpen());
}

TEST(a_corrupt_level_dat_is_not_mistaken_for_a_missing_one)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(fs.makeDirectories(dir.c_str()));

    const u8 garbage[] = {0x1f, 0x8b, 0x08, 0x00, 0x41, 0x42, 0x43, 0x44};
    CHECK(fs.writeFileAtomic((dir + "/level.dat").c_str(),
                             ConstByteSpan(garbage, sizeof(garbage))));

    AlphaChunkFileStorage storage(fs);
    CHECK(storage.open(dir, kNow) == OpenResult::Corrupt);
}

TEST(a_chunk_round_trips_through_the_disk)
{
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);
    const std::string dir = temp.world("World");
    CHECK(storage.create(dir, 7, kNow) == OpenResult::Ok);

    // Negative coordinates on both axes: the case where the two base36
    // encodings in one path diverge.
    const ChunkColumn written = makeChunk(-13, 44, 1);
    CHECK(storage.saveChunk(written));
    CHECK(storage.hasChunk(-13, 44));
    CHECK(!storage.hasChunk(-13, 45));

    // And it landed where a PC copy of the game would look for it.
    CHECK(fs.exists((dir + "/1f/18/c.-d.18.dat").c_str()));

    ChunkColumn read;
    CHECK(storage.loadChunk(-13, 44, &read));
    CHECK_EQ(read.x, -13);
    CHECK_EQ(read.z, 44);
    CHECK_EQ(read.lastUpdate, 99LL);
    CHECK_EQ(read.terrainPopulated, true);
    CHECK_EQ(read.block(0, 0, 0), BlockId(1));
    CHECK_EQ(read.block(3, 12, 9), BlockId(56));
    CHECK_EQ(read.blockData(3, 12, 9), u8(7));
    CHECK_EQ(read.skyLight(0, 100, 0), u8(15));
    CHECK_EQ(read.heightMap[0], u8(40));
}

TEST(saving_a_chunk_twice_replaces_it)
{
    // The atomic write goes through a temporary and a rename; if the rename did
    // not replace, the second save would silently do nothing.
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);
    CHECK(storage.create(temp.world("World"), 7, kNow) == OpenResult::Ok);

    CHECK(storage.saveChunk(makeChunk(2, 2, 1)));
    CHECK(storage.saveChunk(makeChunk(2, 2, 3)));

    ChunkColumn read;
    CHECK(storage.loadChunk(2, 2, &read));
    CHECK_EQ(read.block(0, 0, 0), BlockId(3));
}

TEST(a_chunk_filed_under_the_wrong_name_is_refused)
{
    // The filename is authoritative. Loading a mismatched file would put the
    // wrong terrain here and then save it back one folder further from correct.
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);
    const std::string dir = temp.world("World");
    CHECK(storage.create(dir, 7, kNow) == OpenResult::Ok);

    CHECK(storage.saveChunk(makeChunk(5, 5, 1)));

    // Move it to where chunk (6, 6) belongs, contents unchanged.
    std::vector<u8> bytes;
    CHECK(fs.readFile((dir + "/5/5/c.5.5.dat").c_str(), &bytes, 1u << 20));
    CHECK(fs.makeDirectories((dir + "/6/6").c_str()));
    CHECK(fs.writeFileAtomic((dir + "/6/6/c.6.6.dat").c_str(), bytes));

    ChunkColumn read;
    CHECK(!storage.loadChunk(6, 6, &read));
    CHECK(storage.loadChunk(5, 5, &read));
}

TEST(missing_and_damaged_chunks_fail_rather_than_load_something)
{
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);
    const std::string dir = temp.world("World");
    CHECK(storage.create(dir, 7, kNow) == OpenResult::Ok);

    ChunkColumn read;
    CHECK(!storage.loadChunk(0, 0, &read));

    CHECK(storage.saveChunk(makeChunk(1, 1, 1)));
    std::vector<u8> bytes;
    CHECK(fs.readFile((dir + "/1/1/c.1.1.dat").c_str(), &bytes, 1u << 20));

    // Truncate at every length: each prefix must be a refusal, never a partial
    // load. Chunk files come off a card that can be pulled mid-write.
    for (usize len = 0; len < bytes.size(); len += 13) {
        CHECK(fs.writeFileAtomic((dir + "/1/1/c.1.1.dat").c_str(),
                                 ConstByteSpan(bytes.data(), len)));
        ChunkColumn partial;
        CHECK(!storage.loadChunk(1, 1, &partial));
    }
}

TEST(the_world_scan_finds_every_chunk_and_ignores_everything_else)
{
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);
    const std::string dir = temp.world("World");
    CHECK(storage.create(dir, 7, kNow) == OpenResult::Ok);

    const i32 coords[][2] = {{0, 0}, {-13, 44}, {63, -64}, {1, 1}, {-1, -1}};
    for (const auto& c : coords) {
        CHECK(storage.saveChunk(makeChunk(c[0], c[1], 1)));
    }

    // Things a real card holds that are not chunks.
    CHECK(fs.writeFileAtomic((dir + "/0/0/notes.txt").c_str(), ConstByteSpan()));
    CHECK(fs.writeFileAtomic((dir + "/0/0/c.bad.dat").c_str(), ConstByteSpan()));

    Collected found;
    CHECK(storage.forEachChunk(&found, Collected::visit));
    CHECK_EQ(found.keys.size(), usize(5));
    for (const auto& c : coords) {
        CHECK(found.has(c[0], c[1]));
    }
}

TEST(the_session_lock_notices_another_writer)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");

    AlphaChunkFileStorage first(fs);
    CHECK(first.create(dir, 7, kNow) == OpenResult::Ok);
    CHECK(first.lockStillOurs());
    CHECK(first.close(kNow));

    AlphaChunkFileStorage mine(fs);
    CHECK(mine.open(dir, kNow + 1000) == OpenResult::Ok);
    CHECK(mine.lockStillOurs());

    // A second opener claims the lock; the first has to find out.
    AlphaChunkFileStorage other(fs);
    CHECK(other.open(dir, kNow + 2000) == OpenResult::Ok);
    CHECK(other.lockStillOurs());
    CHECK(!mine.lockStillOurs());

    // Refreshing takes it back, which is what a reclaim would do.
    CHECK(mine.refreshLock(kNow + 3000));
    CHECK(mine.lockStillOurs());
    CHECK(!other.lockStillOurs());
}

TEST(level_edits_persist_across_a_close_and_reopen)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");

    AlphaChunkFileStorage storage(fs);
    CHECK(storage.create(dir, 42, kNow) == OpenResult::Ok);
    storage.level().spawnX = 132;
    storage.level().spawnZ = -244;
    storage.level().time = 18000;
    storage.level().player.present = true;
    storage.level().player.pos[1] = 70.5;
    CHECK(storage.close(kNow + 5000));

    AlphaChunkFileStorage reopened(fs);
    CHECK(reopened.open(dir, kNow + 6000) == OpenResult::Ok);
    CHECK_EQ(reopened.level().spawnX, 132);
    CHECK_EQ(reopened.level().spawnZ, -244);
    CHECK_EQ(reopened.level().time, 18000LL);
    CHECK_EQ(reopened.level().lastPlayed, kNow + 5000);
    CHECK(reopened.level().player.present);
    CHECK_EQ(reopened.level().player.pos[1], 70.5);
}

TEST(nothing_works_before_the_world_is_open)
{
    TempDir temp;
    io::PosixFileSystem fs;
    AlphaChunkFileStorage storage(fs);

    ChunkColumn chunk;
    CHECK(!storage.isOpen());
    CHECK(!storage.hasChunk(0, 0));
    CHECK(!storage.loadChunk(0, 0, &chunk));
    CHECK(!storage.saveChunk(chunk));
    CHECK(!storage.saveLevel());
    CHECK(!storage.lockStillOurs());
    CHECK(!storage.refreshLock(kNow));
    CHECK(storage.close(kNow));  // closing an unopened world is not an error
}
