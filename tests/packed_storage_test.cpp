#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/format/packed_storage.hpp"
#include "core/world/world_format.hpp"

#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::world;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_pak_XXXXXX");
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

ChunkColumn makeChunk(i32 x, i32 z, BlockId fill)
{
    ChunkColumn chunk(x, z);
    for (int lx = 0; lx < ChunkColumn::kWidth; ++lx) {
        for (int lz = 0; lz < ChunkColumn::kWidth; ++lz) {
            for (int y = 0; y < 24; ++y) {
                chunk.setBlock(lx, y, lz, fill);
            }
            chunk.heightMap[usize(lz) * ChunkColumn::kWidth + usize(lx)] = 24;
        }
    }
    chunk.setBlock(2, 30, 7, 41);
    return chunk;
}

struct Seen {
    std::vector<std::pair<i32, i32>> chunks;

    static bool visit(void* context, i32 x, i32 z)
    {
        static_cast<Seen*>(context)->chunks.emplace_back(x, z);
        return true;
    }
};

}  // namespace

TEST(a_packed_world_round_trips_chunks_at_negative_coordinates)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World");

    format::PackedStorage storage(fs);
    CHECK(storage.create(world, 987654321LL, 1000) == OpenResult::Ok);
    CHECK(storage.saveChunk(makeChunk(0, 0, 1)));
    CHECK(storage.saveChunk(makeChunk(-1, -1, 2)));
    CHECK(storage.saveChunk(makeChunk(-40, 33, 3)));
    CHECK(storage.commit());
    CHECK(storage.close(2000));

    // The mode is read off the folder, with no setting written anywhere.
    CHECK(detectFormat(fs, world) == WorldFormat::Packed);
    // And a packed world is deliberately not a valid Minecraft world: leaving
    // level.dat on disk would let a PC client generate terrain over it.
    CHECK(!fs.exists((world + "/level.dat").c_str()));

    format::PackedStorage reopened(fs);
    CHECK(reopened.open(world, 3000) == OpenResult::Ok);
    CHECK_EQ(reopened.level().randomSeed, i64(987654321LL));

    CHECK(reopened.hasChunk(-1, -1));
    CHECK(!reopened.hasChunk(5, 5));

    ChunkColumn loaded(0, 0);
    CHECK(reopened.loadChunk(-40, 33, &loaded));
    CHECK_EQ(loaded.x, -40);
    CHECK_EQ(loaded.z, 33);
    CHECK_EQ(int(loaded.block(4, 10, 4)), 3);
    CHECK_EQ(int(loaded.block(2, 30, 7)), 41);
    CHECK(reopened.close(4000));
}

TEST(a_region_is_one_group_so_one_listing_settles_a_thousand_chunks)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World");

    format::PackedStorage storage(fs);
    CHECK(storage.create(world, 1, 1000) == OpenResult::Ok);
    CHECK(storage.saveChunk(makeChunk(0, 0, 1)));
    CHECK(storage.saveChunk(makeChunk(31, 31, 2)));
    CHECK(storage.saveChunk(makeChunk(32, 0, 3)));  // the next region along
    CHECK(storage.commit());

    // Two chunks in one region share a key; one across the boundary does not.
    CHECK_EQ(storage.chunkGroupKey(0, 0), storage.chunkGroupKey(31, 31));
    CHECK(storage.chunkGroupKey(0, 0) != storage.chunkGroupKey(32, 0));

    // **The key must not collide inside a legal world.** Region coordinates
    // reach about +/-62,500 in a1.1.2, which needs 34 bits; a 32-bit key would
    // alias two real regions together and make the cache report a chunk that
    // exists as absent.
    CHECK(storage.chunkGroupKey(-2000000, 0) != storage.chunkGroupKey(0, -2000000));
    CHECK(storage.chunkGroupKey(2000000, 0) != storage.chunkGroupKey(0, 2000000));

    Seen seen;
    CHECK(storage.listChunkGroup(0, 0, &seen, Seen::visit));
    CHECK_EQ(seen.chunks.size(), usize(2));

    // A region that was never written is an empty answer, not a failure -- the
    // same contract the folder backend has for a missing leaf directory.
    Seen empty;
    CHECK(storage.listChunkGroup(900, 900, &empty, Seen::visit));
    CHECK(empty.chunks.empty());
    CHECK(storage.close(2000));
}

TEST(a_packed_world_walks_every_chunk_it_holds)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World");

    format::PackedStorage storage(fs);
    CHECK(storage.create(world, 1, 1000) == OpenResult::Ok);
    CHECK(storage.saveChunk(makeChunk(1, 1, 1)));
    CHECK(storage.saveChunk(makeChunk(-33, 5, 2)));
    CHECK(storage.saveChunk(makeChunk(70, -70, 3)));
    CHECK(storage.commit());

    Seen seen;
    CHECK(storage.forEachChunk(&seen, Seen::visit));
    CHECK_EQ(seen.chunks.size(), usize(3));
    CHECK(storage.close(2000));
}

TEST(any_storage_opens_whichever_shape_the_folder_is_in)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string packedWorld = temp.at("Packed");
    const std::string folderWorld = temp.at("Folder");

    {
        AnyStorage storage(fs);
        CHECK(storage.create(packedWorld, 11, 1000, WorldFormat::Packed) == OpenResult::Ok);
        CHECK(storage.saveChunk(makeChunk(3, -3, 5)));
        CHECK(storage.close(1000));
    }
    {
        AnyStorage storage(fs);
        CHECK(storage.create(folderWorld, 22, 1000, WorldFormat::Folder) == OpenResult::Ok);
        CHECK(storage.saveChunk(makeChunk(3, -3, 6)));
        CHECK(storage.close(1000));
    }

    // Nothing tells it which; it looks.
    AnyStorage storage(fs);
    CHECK(storage.open(packedWorld, 2000) == OpenResult::Ok);
    CHECK(storage.format() == WorldFormat::Packed);
    ChunkColumn loaded(0, 0);
    CHECK(storage.loadChunk(3, -3, &loaded));
    CHECK_EQ(int(loaded.block(1, 1, 1)), 5);
    CHECK(storage.close(2000));

    CHECK(storage.open(folderWorld, 2000) == OpenResult::Ok);
    CHECK(storage.format() == WorldFormat::Folder);
    CHECK(storage.loadChunk(3, -3, &loaded));
    CHECK_EQ(int(loaded.block(1, 1, 1)), 6);
    CHECK(storage.close(2000));

    // A directory that is neither is not a world in either backend's eyes.
    CHECK(storage.open(temp.at("Nothing"), 2000) == OpenResult::NotAWorld);
}

TEST(peeking_a_packed_world_reads_only_the_manifest_header)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World");

    {
        AnyStorage storage(fs);
        CHECK(storage.create(world, 4242, 111000, WorldFormat::Packed) == OpenResult::Ok);
        CHECK(storage.saveChunk(makeChunk(0, 0, 1)));
        CHECK(storage.close(222000));
    }

    // This is the world list's path: seed and last-played without opening a
    // region, parsing NBT or inflating anything.
    AnyStorage storage(fs);
    LevelData level;
    CHECK(storage.peekLevel(world, &level));
    CHECK_EQ(level.randomSeed, i64(4242));
    CHECK_EQ(level.lastPlayed, i64(222000));

    // And peeking must not claim the world -- the list peeks every world on
    // the card, including ones another session legitimately holds.
    CHECK(!storage.isOpen());
}

TEST(a_packed_world_survives_being_closed_without_an_explicit_commit)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World");

    {
        format::PackedStorage storage(fs);
        CHECK(storage.create(world, 7, 1000) == OpenResult::Ok);
        for (i32 i = 0; i < 12; ++i) {
            CHECK(storage.saveChunk(makeChunk(i, 0, u8(i + 1))));
        }
        // No commit() call: close() owes it, because a player leaving a world
        // is the ordinary way a session ends.
        CHECK(storage.close(2000));
    }

    format::PackedStorage reopened(fs);
    CHECK(reopened.open(world, 3000) == OpenResult::Ok);
    for (i32 i = 0; i < 12; ++i) {
        CHECK(reopened.hasChunk(i, 0));
    }
    CHECK(reopened.close(4000));
}

TEST(more_regions_than_stay_open_still_all_save)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World");

    // Six regions against four slots, so eviction runs -- and evicting has to
    // commit, or the columns a player walked away from are the ones lost.
    {
        format::PackedStorage storage(fs);
        CHECK(storage.create(world, 3, 1000) == OpenResult::Ok);
        for (i32 r = 0; r < 6; ++r) {
            CHECK(storage.saveChunk(makeChunk(r * 32, 0, u8(r + 1))));
        }
        CHECK(storage.close(2000));
    }

    format::PackedStorage reopened(fs);
    CHECK(reopened.open(world, 3000) == OpenResult::Ok);
    for (i32 r = 0; r < 6; ++r) {
        ChunkColumn loaded(0, 0);
        CHECK(reopened.loadChunk(r * 32, 0, &loaded));
        CHECK_EQ(int(loaded.block(1, 1, 1)), r + 1);
    }
    CHECK(reopened.close(4000));
}
