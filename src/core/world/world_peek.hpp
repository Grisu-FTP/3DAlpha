#pragma once

// **Reading a world without opening it**: its level and its chunks, and never
// one byte written back.
//
// `AnyStorage::open` is how the game reads a world, and it is not read-only
// even when the caller only wants to look: it claims `session.lock`, and closing
// rewrites `level.dat`. That is right for a world being played and wrong for
// the main menu's World screen, which draws a diorama of every world the cursor
// passes and preloads the ones either side of it. A world merely looked at must
// not have its lock taken -- another console, or a PC with the card in it, may
// be playing it -- and must not have its level rewritten with a new
// `LastPlayed`.
//
// So this is the read half of both backends and nothing else:
//
//   * **Folder:** `level.dat` read and inflated, and a chunk is its file read,
//     inflated and decoded -- the version slot's own codec and path, so an
//     a1.2 build reads a1.2's files.
//   * **Packed:** the manifest loaded and its `level.dat` blob decoded, and a
//     chunk is a read out of its region container. A region is opened only if
//     its file exists and is not empty, because `RegionFile::open` initialises
//     an empty file in place.
//
// There is no save, no commit and no lock, and `close` is a matter of dropping
// file handles. The host suite checks that every file in a world is identical
// before and after a full read of it, in both formats.
//
// **Absent and failed are different answers.** A chunk that was never
// generated is ordinary and the diorama draws the table under it; a chunk
// that will not decode is a damaged file, which the diorama also draws as
// table but which a caller may want to count.

#include "core/io/file_system.hpp"
#include "core/world/chunk.hpp"
#include "core/world/format/manifest.hpp"
#include "core/world/format/region_file.hpp"
#include "core/world/level_data.hpp"
#include "core/world/world_format.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

enum class PeekRead {
    Ok,
    Absent,
    Failed,
};

class WorldPeek {
public:
    explicit WorldPeek(io::FileSystem& fs) : fs_(fs) {}

    WorldPeek(const WorldPeek&) = delete;
    WorldPeek& operator=(const WorldPeek&) = delete;

    // Detects the format and reads the level. False when the directory is not
    // a world or its level will not decode.
    bool open(std::string_view worldDir);
    void close();

    bool isOpen() const { return open_; }
    WorldFormat format() const { return format_; }
    const LevelData& level() const { return level_; }

    PeekRead loadChunk(i32 x, i32 z, ChunkColumn* out);

    // One chunk of a batch, or **null for one that did not read**: absent, or
    // damaged past decoding. The column is the batch's own and is reused for
    // the next one, so a visitor that wants to keep it must move out of it.
    // Returning false stops the batch.
    using PeekVisitor = bool (*)(void* context, i32 chunkX, i32 chunkZ, ChunkColumn* column);

    // **Many chunks in as few card operations as the format allows.** A packed
    // world groups the request by region and coalesces each group's reads
    // (`RegionFile::readMany`); a folder world reads them one at a time,
    // because a chunk there is its own file and there is nothing to coalesce.
    //
    // **`visit` runs once for every chunk asked for**, with a null column for
    // one that was Absent or Failed -- a batch cannot say which of those two it
    // was, so a caller that needs to tell them apart wants `loadChunk`. What it
    // buys by reporting both is that a caller can count a group of chunks down
    // to nothing and act the moment the last one lands, rather than waiting for
    // the whole batch.
    //
    // The order is **not the order asked for**: sector order for a packed
    // world, with everything absent first.
    //
    // False means the read failed outright, and then some chunks may never be
    // visited. A batch the visitor stopped returns true, as
    // `RegionFile::readMany` does.
    bool loadChunks(const i32* xs, const i32* zs, usize count, void* context, PeekVisitor visit);

private:
    struct Region {
        std::unique_ptr<format::RegionFile> file;
        i32 x = 0;
        i32 z = 0;
        bool known = false;   // this slot holds an answer for (x, z)
        bool absent = false;  // and the answer is "no such region"
        u64 used = 0;
    };

    bool openFolder();
    bool openPacked();
    PeekRead loadFolderChunk(i32 x, i32 z, ChunkColumn* out);
    PeekRead loadPackedChunk(i32 x, i32 z, ChunkColumn* out);
    Region* regionFor(i32 chunkX, i32 chunkZ);

    // What `RegionFile::readMany` hands a payload to: inflate, decode, and on
    // to the caller's own visitor.
    static bool decodeOne(void* context, i32 chunkX, i32 chunkZ, ConstByteSpan payload);

    io::FileSystem& fs_;
    std::string worldDir_;
    WorldFormat format_ = WorldFormat::Unknown;
    bool open_ = false;
    LevelData level_;

    // Four regions is a 128-chunk square, which covers the diorama's 24x24
    // window wherever the region grid happens to cut it.
    static constexpr int kRegionSlots = 4;
    Region regions_[kRegionSlots];
    u64 clock_ = 0;

    std::vector<u8> raw_;
    std::vector<u8> inflated_;

    // `loadChunks` only, so a peek that never batches carries three empty
    // vectors. `batch_` is the merged-run scratch every region shares -- it
    // settles at its high-water mark rather than being reallocated per region
    // -- and the other two are one region's share of the request, gathered out
    // of the caller's arrays.
    std::vector<u8> batch_;
    std::vector<i32> groupX_;
    std::vector<i32> groupZ_;
    std::vector<u8> grouped_;
};

}  // namespace mc::world
