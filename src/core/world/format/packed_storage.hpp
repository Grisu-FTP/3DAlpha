#pragma once

// The storage contract, served out of region containers instead of one file
// per chunk.
//
// Shaped like `mcver::Storage` deliberately -- same members, same meanings --
// so `world::AnyStorage` can hold either and the chunk cache above it never
// learns which. It is **not** a version slot: the folder format is a property
// of a1.1.2 and this is a property of the card, so both exist in one binary and
// the world being opened decides which is used.
//
// **Where the version does reach in.** The container moves opaque payloads and
// knows no NBT, so it would back a real McRegion unchanged. Playing a world
// needs payload bytes turned into a ChunkColumn, and that is this version's
// business: it goes through `mcver::ChunkCodec` and `mcver::LevelCodec`, the
// aliases the slot binds, so core never names an implementation.
//
// **What differs from the folder backend, and why it is better here:**
//
//   * `chunkGroupKey` names a whole region, and `listChunkGroup` answers it
//     from the region's own directory -- one open and one 16 KB read settles
//     all 1,024 of its chunks. The Alpha layout needs a directory listing per
//     leaf, and below 64x64 chunks every chunk has a leaf of its own, so
//     warming a distance-8 view at spawn is 289 listings there against 1 here.
//   * `hasChunk` on an already-open region touches no file at all.
//   * `saveChunk` stages; `commit()` is what makes writes durable. Committing
//     per chunk would write the 16 KB directory and the header for every
//     column saved -- see region_file.hpp.
//
// `session.lock` stays a real file on disk, identical to the folder format's,
// so the lock logic is the same eight big-endian bytes in both and neither
// backend has a lock story of its own.

#include "core/io/file_system.hpp"
#include "core/world/chunk.hpp"
#include "core/world/format/manifest.hpp"
#include "core/world/format/region_file.hpp"
#include "core/world/level_data.hpp"
#include "core/world/storage.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc::world::format {

// How many region containers stay open. At render distance 8 a player's view
// spans at most two regions on each axis, and usually one; four covers a
// 64x64-chunk span and costs ~17 KB of heap each, which honours the "few open
// handles" rule the Alpha backend follows for the same reason.
inline constexpr usize kOpenRegions = 4;

class PackedStorage {
public:
    explicit PackedStorage(io::FileSystem& fs) : fs_(fs) {}
    ~PackedStorage();

    PackedStorage(const PackedStorage&) = delete;
    PackedStorage& operator=(const PackedStorage&) = delete;

    OpenResult open(std::string_view worldDir, i64 nowMillis);
    OpenResult create(std::string_view worldDir, i64 seed, i64 nowMillis);

    // Reads the manifest's metadata block without claiming the world: one open
    // and one 64-byte read, no region touched and no NBT parsed. The folder
    // backend's equivalent has to read and gunzip level.dat.
    bool peekLevel(std::string_view worldDir, LevelData* out);

    bool close(i64 nowMillis);

    bool isOpen() const { return open_; }
    std::string_view worldDir() const { return worldDir_; }

    LevelData& level() { return level_; }
    const LevelData& level() const { return level_; }
    bool saveLevel();

    bool hasChunk(i32 x, i32 z);
    bool loadChunk(i32 x, i32 z, ChunkColumn* out);
    bool saveChunk(const ChunkColumn& chunk);
    bool removeChunk(i32 x, i32 z);

    // Makes every staged write findable. Cheap when nothing is staged.
    bool commit();

    bool refreshLock(i64 nowMillis);
    bool lockStillOurs();

    bool forEachChunk(void* context, ChunkVisitor visit);

    u64 chunkGroupKey(i32 x, i32 z) const;
    bool listChunkGroup(i32 x, i32 z, void* context, ChunkVisitor visit);

private:
    struct Slot {
        std::unique_ptr<RegionFile> region;
        i32 x = 0;
        i32 z = 0;
        u64 used = 0;
    };

    // Null when the region has no file and `create` is false, which is the
    // ordinary state of an unexplored corner rather than a failure.
    RegionFile* regionFor(i32 chunkX, i32 chunkZ, bool create);
    bool closeRegions();
    std::string pathFor(const char* name) const;
    bool writeLock(i64 nowMillis);

    io::FileSystem& fs_;
    std::string worldDir_;
    bool open_ = false;
    LevelData level_;
    Manifest manifest_;
    i64 lockValue_ = 0;

    Slot regions_[kOpenRegions];
    u64 clock_ = 0;

    // Reused across chunk loads and saves so streaming does not allocate a
    // fresh pair of buffers per column.
    std::vector<u8> payload_;
    std::vector<u8> scratch_;
};

}  // namespace mc::world::format
