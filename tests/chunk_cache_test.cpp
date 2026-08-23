#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/world/chunk.hpp"
#include "core/world/chunk_cache.hpp"
#include "version_slots.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace mc;
using world::BlockId;
using world::ChunkCache;
using world::ChunkColumn;
using world::OpenResult;

namespace {

constexpr i64 kNow = 1284768000000LL;

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_cache_XXXXXX");
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

// **Counts what reaches the card**, which is the only way to state most of what
// this class promises. "Serves it from memory" and "reads it again" produce the
// same column; they differ in whether a file was opened, and nothing but a
// counter can tell the two apart.
class CountingFileSystem : public io::FileSystem {
public:
    int reads = 0;
    int writes = 0;
    int stats = 0;
    int listings = 0;

    void resetCounts() { reads = writes = stats = listings = 0; }

    int total() const { return reads + writes + stats + listings; }

    bool readFile(const char* path, std::vector<u8>* out, usize maxSize) override
    {
        ++reads;
        return inner_.readFile(path, out, maxSize);
    }
    bool writeFileAtomic(const char* path, ConstByteSpan data) override
    {
        ++writes;
        return inner_.writeFileAtomic(path, data);
    }
    bool exists(const char* path) override
    {
        ++stats;
        return inner_.exists(path);
    }
    bool isDirectory(const char* path) override { return inner_.isDirectory(path); }
    bool fileSize(const char* path, usize* out) override { return inner_.fileSize(path, out); }
    bool makeDirectories(const char* path) override { return inner_.makeDirectories(path); }
    bool removeFile(const char* path) override { return inner_.removeFile(path); }
    bool removeDirectory(const char* path) override { return inner_.removeDirectory(path); }
    bool listDirectory(const char* path, void* context, io::DirVisitor visit) override
    {
        ++listings;
        return inner_.listDirectory(path, context, visit);
    }

private:
    io::PosixFileSystem inner_;
};

ChunkColumn makeChunk(i32 x, i32 z, BlockId fill)
{
    ChunkColumn chunk(x, z);
    for (int lx = 0; lx < ChunkColumn::kWidth; ++lx) {
        for (int lz = 0; lz < ChunkColumn::kWidth; ++lz) {
            for (int y = 0; y < 40; ++y) {
                chunk.setBlock(lx, y, lz, fill);
            }
            chunk.heightMap[usize(lz) * ChunkColumn::kWidth + usize(lx)] = 40;
        }
    }
    chunk.setBlock(3, 12, 9, 56);
    chunk.setBlockData(3, 12, 9, 7);
    chunk.terrainPopulated = true;
    chunk.compact();
    return chunk;
}

bool sameBlocks(const ChunkColumn& a, const ChunkColumn& b)
{
    if (a.x != b.x || a.z != b.z || a.terrainPopulated != b.terrainPopulated) {
        return false;
    }
    for (int lx = 0; lx < ChunkColumn::kWidth; ++lx) {
        for (int lz = 0; lz < ChunkColumn::kWidth; ++lz) {
            for (int y = 0; y < ChunkColumn::kHeight; ++y) {
                if (a.block(lx, y, lz) != b.block(lx, y, lz)
                    || a.blockData(lx, y, lz) != b.blockData(lx, y, lz)) {
                    return false;
                }
            }
        }
    }
    return true;
}

// Makes an empty world on disk and returns its path, so each test starts from a
// real level.dat rather than a stub -- the cache opens a storage slot, and a
// slot that never opened answers nothing.
bool createWorld(io::FileSystem& fs, const std::string& dir)
{
    mcver::Storage storage(fs);
    if (storage.create(dir.c_str(), 1234LL, kNow) != OpenResult::Ok) {
        return false;
    }
    return storage.close(kNow);
}

// Drains an unthreaded cache. pump() does one unit a call on purpose; a test
// that wants the queues empty says so.
void drain(ChunkCache& cache)
{
    for (int i = 0; i < 4096 && !cache.idle(); ++i) {
        cache.pump();
    }
}

}  // namespace

// The base case, and the one everything else is a variation on: what goes in
// comes back out, and it reaches the card when told to.
TEST(a_saved_column_reads_back_and_survives_a_reopen)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    const ChunkColumn written = makeChunk(-13, 44, 1);
    {
        ChunkCache cache(fs);
        CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);
        CHECK(cache.save(written));

        ChunkColumn scratch;
        CHECK(cache.load(-13, 44, &scratch) != nullptr);
        CHECK(sameBlocks(written, scratch));
        cache.close(kNow);
    }

    ChunkCache reopened(fs);
    CHECK(reopened.open(dir.c_str(), kNow) == OpenResult::Ok);
    ChunkColumn scratch;
    CHECK(reopened.load(-13, 44, &scratch) != nullptr);
    CHECK(sameBlocks(written, scratch));
    reopened.close(kNow);
}

// **The invariant that would silently change the world if it broke.**
//
// WorldStreamer classifies a cell by asking whether the world has that chunk,
// and the answer decides whether the chunk gets generated -- which decides the
// population order, which *is* the world in a1.1.2. A write-back cache moves
// when bytes reach the card; it must not move when the answer changes.
TEST(a_saved_column_exists_before_its_bytes_reach_the_card)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    ChunkCache cache(fs);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    CHECK(!cache.hasChunk(7, 7));
    CHECK(cache.save(makeChunk(7, 7, 3)));

    // Nothing has been flushed, so there is no file yet -- and the answer is
    // true anyway, because the cache is what the world is now.
    fs.resetCounts();
    CHECK(cache.hasChunk(7, 7));
    CHECK_EQ(fs.total(), 0);

    cache.close(kNow);
}

// The retention half of the buffer zone. A column that leaves the grid and is
// walked straight back into used to be an open, a read, a close and a gzip
// inflate; the promise is that it is now none of those.
TEST(a_column_given_back_and_taken_again_costs_no_storage_operation)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    {
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == OpenResult::Ok);
        CHECK(storage.saveChunk(makeChunk(2, 3, 5)));
        CHECK(storage.close(kNow));
    }

    ChunkCache cache(fs);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    std::unique_ptr<ChunkColumn> column;
    CHECK(cache.tryTake(2, 3, &column) == ChunkCache::Take::Took);
    CHECK(column != nullptr);

    // Out of the grid, back into the cache.
    cache.give(std::move(column));

    fs.resetCounts();
    std::unique_ptr<ChunkColumn> again;
    CHECK(cache.tryTake(2, 3, &again) == ChunkCache::Take::Took);
    CHECK_EQ(fs.total(), 0);
    CHECK(again != nullptr);
    CHECK(sameBlocks(makeChunk(2, 3, 5), *again));

    cache.close(kNow);
}

// One listing settles a whole directory. The Alpha layout puts chunks 64 apart
// in the same leaf, so this is what turns a row of stats at every chunk-boundary
// crossing into nothing at all.
TEST(a_listed_group_answers_without_touching_the_card)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    {
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == OpenResult::Ok);
        // Same leaf directory: the coordinates differ by a multiple of 64.
        CHECK(storage.saveChunk(makeChunk(1, 1, 5)));
        CHECK(storage.saveChunk(makeChunk(65, 65, 5)));
        CHECK(storage.close(kNow));
    }

    ChunkCache cache(fs);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);
    cache.warmGroup(1, 1);
    drain(cache);

    fs.resetCounts();
    CHECK(cache.hasChunk(1, 1));
    CHECK(cache.hasChunk(65, 65));
    // ...and the negative answer is free too, which is the one the frontier
    // asks for over and over.
    CHECK(!cache.hasChunk(129, 129));
    CHECK_EQ(fs.total(), 0);
    CHECK_EQ(int(cache.stats().stats), 0);

    cache.close(kNow);
}

// A group listed at one moment and written to at the next. The listing must not
// overwrite what save() recorded, or hasChunk would deny a chunk that exists --
// and denying one is what would reorder generation.
TEST(a_group_listing_does_not_forget_a_chunk_saved_while_it_was_in_flight)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    ChunkCache cache(fs);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    // Queued, then written to before the listing runs.
    cache.warmGroup(4, 4);
    CHECK(cache.save(makeChunk(4, 4, 9)));
    drain(cache);

    CHECK(cache.hasChunk(4, 4));
    cache.close(kNow);
    CHECK(cache.hasChunk(4, 4) == false);  // closed: it answers nothing at all
}

// The byte cap has to bite, and it has to bite only on columns the card already
// agrees with. Evicting one that is still owed would lose it outright.
TEST(the_cap_evicts_clean_columns_and_never_dirty_ones)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    ChunkCache cache(fs);
    ChunkCache::Config config;
    // Two columns' worth, so filling it is a handful of saves rather than a
    // loop long enough to be its own test.
    config.cleanCapBytes = 40 * 1024;
    config.dirtyCapBytes = 64u << 20;  // nothing forced out by the other cap
    cache.configure(config);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    for (i32 i = 0; i < 24; ++i) {
        CHECK(cache.save(makeChunk(i, 0, BlockId(1 + (i % 5)))));
    }
    cache.pump();

    // Everything is dirty and nothing has been written, so nothing may have
    // been thrown away -- the cap does not apply to columns the card has never
    // seen.
    CHECK_EQ(int(cache.stats().evicted), 0);
    for (i32 i = 0; i < 24; ++i) {
        ChunkColumn scratch;
        CHECK(cache.load(i, 0, &scratch) != nullptr);
    }

    // Once they are on the card they are ordinary retained columns, and the cap
    // is free to reclaim them.
    cache.flush(true);
    drain(cache);
    cache.pump();
    CHECK(cache.stats().evicted > 0);
    CHECK(cache.stats().cleanBytes <= config.cleanCapBytes);

    // Evicted is not lost: it is on the card, and asking reads it back.
    for (i32 i = 0; i < 24; ++i) {
        ChunkColumn scratch;
        CHECK(cache.load(i, 0, &scratch) != nullptr);
        CHECK(sameBlocks(makeChunk(i, 0, BlockId(1 + (i % 5))), scratch));
    }
    cache.close(kNow);
}

// A file the world claims to have and that will not read. The answer has to be
// given once: asking again every frame for the rest of the session is what the
// Missing result exists to prevent.
TEST(an_unreadable_chunk_is_reported_once_rather_than_retried)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    {
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == OpenResult::Ok);
        CHECK(storage.saveChunk(makeChunk(5, 5, 2)));
        CHECK(storage.close(kNow));
    }

    // Truncate it to something that is not a gzip stream. The path is the
    // format's own, so it has to be built the way storage builds it.
    char path[256];
    std::snprintf(path, sizeof(path), "%s/5/5/c.5.5.dat", dir.c_str());
    const u8 rubbish[] = {'n', 'o', 't', 'g', 'z'};
    CHECK(fs.writeFileAtomic(path, ConstByteSpan(rubbish, sizeof(rubbish))));

    ChunkCache cache(fs);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    std::unique_ptr<ChunkColumn> column;
    CHECK(cache.tryTake(5, 5, &column) == ChunkCache::Take::Missing);

    fs.resetCounts();
    CHECK(cache.tryTake(5, 5, &column) == ChunkCache::Take::Missing);
    CHECK_EQ(fs.total(), 0);  // remembered, not asked again

    cache.close(kNow);
}

// The threaded configuration, which is the one the console runs. The point is
// not that it is faster here -- it will not be -- but that the same answers come
// out of it, and that a flush actually lands.
TEST(the_threaded_cache_writes_what_it_was_given)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    {
        ChunkCache cache(fs);
        ChunkCache::Config config;
        config.threaded = true;
        cache.configure(config);
        CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);
        CHECK(cache.stats().workerRunning);

        for (i32 i = 0; i < 8; ++i) {
            CHECK(cache.save(makeChunk(i, -i, BlockId(1 + i))));
        }

        // Asked for before anything can have been written, which is the
        // read-your-own-writes case the generator depends on.
        for (i32 i = 0; i < 8; ++i) {
            ChunkColumn scratch;
            CHECK(cache.load(i, -i, &scratch) != nullptr);
            CHECK(sameBlocks(makeChunk(i, -i, BlockId(1 + i)), scratch));
        }

        cache.flush(true);
        CHECK(cache.idle());
        cache.close(kNow);
    }

    // A fresh cache over the same directory, so this reads files and nothing
    // else.
    ChunkCache reopened(fs);
    CHECK(reopened.open(dir.c_str(), kNow) == OpenResult::Ok);
    for (i32 i = 0; i < 8; ++i) {
        ChunkColumn scratch;
        CHECK(reopened.load(i, -i, &scratch) != nullptr);
        CHECK(sameBlocks(makeChunk(i, -i, BlockId(1 + i)), scratch));
    }
    reopened.close(kNow);
}

// **Nothing reaches the card outside a save.** This is the shape a1.1.2 has --
// it writes a chunk when its 1024-slot table evicts one, and everything else
// only on Save and quit to title -- and it is what the autosave interval exists
// to bound. Writing eagerly instead would be safer than the original and less
// like it, and it would put a deflate and six file operations on the card every
// time a column was finished.
TEST(a_saved_column_does_not_reach_the_card_until_a_flush)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    ChunkCache cache(fs);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    fs.resetCounts();
    for (i32 i = 0; i < 8; ++i) {
        CHECK(cache.save(makeChunk(i, 4, BlockId(1 + i))));
    }
    CHECK_EQ(fs.writes, 0);

    // ...and it is the world regardless, which is what makes deferring the
    // write invisible to everything except a power cut.
    for (i32 i = 0; i < 8; ++i) {
        CHECK(cache.hasChunk(i, 4));
        ChunkColumn scratch;
        CHECK(cache.load(i, 4, &scratch) != nullptr);
        CHECK(sameBlocks(makeChunk(i, 4, BlockId(1 + i)), scratch));
    }
    CHECK_EQ(fs.writes, 0);

    cache.flush(true);
    drain(cache);
    CHECK(fs.writes >= 8);

    cache.close(kNow);
}

// Position, rotation and the world clock have to survive a round trip, and the
// clock has to survive it *absolutely* -- level.dat's Time is a running tick
// count, so storing the remainder would put the world back on day zero every
// time it was saved.
TEST(the_player_position_and_the_world_clock_round_trip)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    ChunkCache::PlayerState player;
    player.valid = true;
    player.pos[0] = -1234.5;
    player.pos[1] = 71.25;
    player.pos[2] = 6789.75;
    player.rotation[0] = 137.5f;
    player.rotation[1] = -22.25f;
    // Well past a day, so a run that kept only the remainder would fail here.
    player.timeTicks = 24000LL * 9 + 1234;

    {
        ChunkCache cache(fs);
        CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);
        cache.close(kNow, player);
    }

    ChunkCache reopened(fs);
    CHECK(reopened.open(dir.c_str(), kNow) == OpenResult::Ok);
    const world::LevelData& level = reopened.level();
    CHECK(level.player.present);
    CHECK_EQ(level.player.pos[0], -1234.5);
    CHECK_EQ(level.player.pos[1], 71.25);
    CHECK_EQ(level.player.pos[2], 6789.75);
    CHECK_EQ(double(level.player.rotation[0]), 137.5);
    CHECK_EQ(double(level.player.rotation[1]), -22.25);
    CHECK_EQ(level.time, 24000LL * 9 + 1234);
    reopened.close(kNow);
}

// A world a server made has no Player compound, and inventing one would move
// its player to 0,0,0 the next time it was opened elsewhere. Nobody has stood
// in this one, so nothing may be written.
TEST(a_world_nobody_stood_in_keeps_its_absent_player)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    {
        ChunkCache cache(fs);
        CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);
        CHECK(!cache.level().player.present);
        cache.close(kNow);  // no player state offered
    }

    ChunkCache reopened(fs);
    CHECK(reopened.open(dir.c_str(), kNow) == OpenResult::Ok);
    CHECK(!reopened.level().player.present);
    reopened.close(kNow);
}

// The autosave timer writes level.dat and refreshes session.lock, and neither
// may happen on the thread that asked for it. There is no way to observe "which
// thread" from here, so what is checked is the shape: asking does not do the
// work, and draining does.
TEST(housekeeping_is_queued_rather_than_done_where_it_was_asked_for)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    ChunkCache cache(fs);
    ChunkCache::Config config;
    config.threaded = true;
    cache.configure(config);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);
    CHECK(cache.stats().workerRunning);

    cache.requestHousekeeping(kNow + 60000);
    cache.flush(true);
    CHECK(cache.idle());

    // The lock is ours after the refresh, which is the whole reason to run it:
    // before this timer existed, refreshLock() was written and never called.
    cache.close(kNow + 60000);
}

// Reading ahead is the other half of the buffer zone. A column asked for after
// it has been read ahead must cost nothing, and must be counted as a prefetch
// hit -- that counter is how the debug page says whether the band is earning
// its memory.
TEST(a_prefetched_column_is_taken_without_touching_the_card)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    CountingFileSystem fs;
    const std::string dir = temp.world("World");
    CHECK(createWorld(fs, dir));

    {
        mcver::Storage storage(fs);
        CHECK(storage.open(dir.c_str(), kNow) == OpenResult::Ok);
        CHECK(storage.saveChunk(makeChunk(9, 9, 4)));
        CHECK(storage.close(kNow));
    }

    ChunkCache cache(fs);
    ChunkCache::Config config;
    config.threaded = true;
    cache.configure(config);
    CHECK(cache.open(dir.c_str(), kNow) == OpenResult::Ok);

    cache.prefetch(9, 9);
    for (int i = 0; i < 2000 && !cache.idle(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(cache.idle());

    fs.resetCounts();
    std::unique_ptr<ChunkColumn> column;
    CHECK(cache.tryTake(9, 9, &column) == ChunkCache::Take::Took);
    CHECK_EQ(fs.total(), 0);
    CHECK_EQ(int(cache.stats().prefetchHits), 1);

    cache.close(kNow);
}
