// Reading a world without opening it: level, chunks, both formats, and not a
// byte of the world changed afterwards. See core/world/world_peek.hpp.

#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/world_peek.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace mc;
using namespace mc::world;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_peek_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
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
    return chunk;
}

// Every file under `dir`, by path, with its bytes.
std::map<std::string, std::vector<char>> snapshot(const std::string& dir)
{
    std::map<std::string, std::vector<char>> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        files[entry.path().string()] =
            std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return files;
}

void makeWorld(io::PosixFileSystem& fs, const std::string& dir, WorldFormat format)
{
    AnyStorage storage(fs);
    CHECK(storage.create(dir, 424242, 1000, format) == OpenResult::Ok);
    storage.level().player.present = true;
    storage.level().player.pos[0] = 200.5;
    storage.level().player.pos[1] = 70.0;
    storage.level().player.pos[2] = -65.5;
    CHECK(storage.saveChunk(makeChunk(0, 0, 1)));
    CHECK(storage.saveChunk(makeChunk(-1, -1, 3)));
    CHECK(storage.saveChunk(makeChunk(-17, 5, 4)));
    CHECK(storage.commit());
    CHECK(storage.saveLevel());
    CHECK(storage.close(2000));
}

void checkPeek(WorldFormat format)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");
    makeWorld(fs, dir, format);
    const auto before = snapshot(dir);
    CHECK(!before.empty());

    {
        WorldPeek peek(fs);
        CHECK(peek.open(dir));
        CHECK(peek.format() == format);
        CHECK_EQ(peek.level().randomSeed, i64(424242));
        CHECK(peek.level().player.present);
        CHECK(peek.level().player.pos[0] == 200.5);

        ChunkColumn column;
        CHECK(peek.loadChunk(-1, -1, &column) == PeekRead::Ok);
        CHECK_EQ(column.x, -1);
        CHECK_EQ(column.z, -1);
        CHECK_EQ(column.block(3, 10, 3), BlockId(3));
        CHECK(peek.loadChunk(-17, 5, &column) == PeekRead::Ok);
        CHECK_EQ(column.block(0, 0, 0), BlockId(4));
        CHECK(peek.loadChunk(0, 0, &column) == PeekRead::Ok);
        CHECK_EQ(column.block(15, 23, 15), BlockId(1));
        CHECK_EQ(column.block(15, 24, 15), BlockId(0));

        // Never generated, near and far -- in a region that exists and in one
        // that does not.
        CHECK(peek.loadChunk(2, 3, &column) == PeekRead::Absent);
        CHECK(peek.loadChunk(-900, 700, &column) == PeekRead::Absent);
        peek.close();
        CHECK(!peek.isOpen());
    }

    // **Nothing written**: same files, same bytes, and no lock claimed.
    const auto after = snapshot(dir);
    CHECK_EQ(after.size(), before.size());
    CHECK(after == before);
}

// What a batch handed back, in the order it arrived.
struct Batch {
    std::vector<std::pair<i32, i32>> order;   // every chunk reported, in order
    std::map<std::pair<i32, i32>, BlockId> fill;  // only the ones that read
    usize missing = 0;                        // reported with no column
    usize stopAfter = 0;
    // CHECK expands to a bare `return` and this is a bool, so a mismatch is
    // recorded here and asserted by the caller.
    bool misplaced = false;

    static bool visit(void* context, i32 x, i32 z, ChunkColumn* column)
    {
        Batch& self = *static_cast<Batch*>(context);
        self.order.emplace_back(x, z);
        if (column == nullptr) {
            ++self.missing;
        } else {
            self.misplaced = self.misplaced || column->x != x || column->z != z;
            self.fill[{x, z}] = column->block(3, 10, 3);
        }
        return self.stopAfter == 0 || self.order.size() < self.stopAfter;
    }
};

BlockId filled(const Batch& batch, i32 x, i32 z)
{
    const auto it = batch.fill.find(std::pair<i32, i32>(x, z));
    return it == batch.fill.end() ? BlockId(0) : it->second;
}

void checkBatch(WorldFormat format)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");
    makeWorld(fs, dir, format);
    const auto before = snapshot(dir);

    WorldPeek peek(fs);
    CHECK(peek.open(dir));

    // The three chunks the world holds, three it does not, and one in a region
    // that was never created -- and they span three regions, so the grouping
    // is exercised too. Asked for in an order that is nobody's sector order.
    const i32 xs[] = {2, -17, -900, 0, 3, -1, 40};
    const i32 zs[] = {3, 5, 700, 0, 3, -1, 40};
    Batch batch;
    CHECK(peek.loadChunks(xs, zs, 7, &batch, &Batch::visit));

    // **Every chunk asked for is answered**, so a caller counting a group down
    // reaches zero: three read, four reported with no column.
    CHECK(!batch.misplaced);
    CHECK_EQ(batch.order.size(), usize(7));
    CHECK_EQ(batch.fill.size(), usize(3));
    CHECK_EQ(batch.missing, usize(4));
    CHECK_EQ(int(filled(batch, 0, 0)), 1);
    CHECK_EQ(int(filled(batch, -1, -1)), 3);
    CHECK_EQ(int(filled(batch, -17, 5)), 4);

    // Stopping is not a failure, and it stops.
    Batch stopped;
    stopped.stopAfter = 2;
    CHECK(peek.loadChunks(xs, zs, 7, &stopped, &Batch::visit));
    CHECK_EQ(stopped.order.size(), usize(2));

    // An empty batch is a batch.
    Batch none;
    CHECK(peek.loadChunks(xs, zs, 0, &none, &Batch::visit));
    CHECK(none.order.empty());

    peek.close();
    CHECK(snapshot(dir) == before);
}

}  // namespace

TEST(world_peek_reads_a_folder_world_and_writes_nothing)
{
    checkPeek(WorldFormat::Folder);
}

TEST(world_peek_reads_a_packed_world_and_writes_nothing)
{
    checkPeek(WorldFormat::Packed);
}

TEST(world_peek_tells_a_damaged_chunk_from_a_missing_one)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");
    makeWorld(fs, dir, WorldFormat::Folder);

    mcver::ChunkLayout::Path path;
    CHECK(mcver::ChunkLayout::filePath(dir, -1, -1, &path));
    const u8 garbage[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(fs.writeFileAtomic(path.text, ConstByteSpan(garbage, sizeof(garbage))));

    WorldPeek peek(fs);
    CHECK(peek.open(dir));
    ChunkColumn column;
    CHECK(peek.loadChunk(-1, -1, &column) == PeekRead::Failed);
    CHECK(peek.loadChunk(0, 0, &column) == PeekRead::Ok);
    CHECK(peek.loadChunk(1, 1, &column) == PeekRead::Absent);
}

TEST(world_peek_refuses_a_directory_that_is_not_a_world)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("Nothing");
    CHECK(fs.makeDirectories(dir.c_str()));
    WorldPeek peek(fs);
    CHECK(!peek.open(dir));
    ChunkColumn column;
    CHECK(peek.loadChunk(0, 0, &column) == PeekRead::Failed);
    CHECK(snapshot(dir).empty());
}

TEST(world_peek_batches_a_folder_world_into_the_same_chunks_one_at_a_time_gives)
{
    checkBatch(WorldFormat::Folder);
}

TEST(world_peek_batches_a_packed_world_into_the_same_chunks_one_at_a_time_gives)
{
    checkBatch(WorldFormat::Packed);
}

TEST(world_peek_batch_leaves_out_a_damaged_chunk_rather_than_failing_the_rest)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");
    makeWorld(fs, dir, WorldFormat::Folder);

    mcver::ChunkLayout::Path path;
    CHECK(mcver::ChunkLayout::filePath(dir, -1, -1, &path));
    const u8 garbage[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(fs.writeFileAtomic(path.text, ConstByteSpan(garbage, sizeof(garbage))));

    WorldPeek peek(fs);
    CHECK(peek.open(dir));
    const i32 xs[] = {-1, 0, -17};
    const i32 zs[] = {-1, 0, 5};
    Batch batch;
    CHECK(peek.loadChunks(xs, zs, 3, &batch, &Batch::visit));
    CHECK_EQ(batch.order.size(), usize(3));
    CHECK_EQ(batch.fill.size(), usize(2));
    CHECK_EQ(batch.missing, usize(1));  // the damaged one, reported as unread
    CHECK_EQ(int(filled(batch, 0, 0)), 1);
    CHECK_EQ(int(filled(batch, -17, 5)), 4);
}

TEST(world_peek_batch_on_a_world_that_is_not_open_fails)
{
    io::PosixFileSystem fs;
    WorldPeek peek(fs);
    const i32 xs[] = {0};
    const i32 zs[] = {0};
    Batch batch;
    CHECK(!peek.loadChunks(xs, zs, 1, &batch, &Batch::visit));
}
