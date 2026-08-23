#pragma once

// The Alpha level format as a `storage` slot: one gzipped NBT file per chunk
// column, under a base36 directory tree, with level.dat and session.lock beside
// them. Format details in docs/world-format.md; this is the file-level layer
// over the codecs in chunk_nbt.hpp and level_dat.hpp.
//
// Everything here is I/O-thread work. Nothing on this path may run on core0 --
// it opens files, and it inflates and deflates 80 KB buffers.

#include "core/io/file_system.hpp"
#include "core/world/chunk.hpp"
#include "core/world/level_data.hpp"
#include "core/world/storage.hpp"
#include "impl/storage/alpha_chunkfiles/chunk_path.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::alpha {

// A gzipped column is well under this; the ceiling exists so that a hostile or
// damaged file on the card is refused rather than allocated.
inline constexpr usize kMaxChunkFileBytes = 4u << 20;
inline constexpr usize kMaxChunkNbtBytes = 2u << 20;
inline constexpr usize kMaxLevelFileBytes = 1u << 20;
inline constexpr usize kMaxLevelNbtBytes = 1u << 20;

// Eight bytes: one big-endian i64 of milliseconds since the epoch.
inline constexpr usize kSessionLockBytes = 8;

class AlphaChunkFileStorage {
public:
    explicit AlphaChunkFileStorage(io::FileSystem& fs) : fs_(fs) {}

    // Opens an existing world: reads level.dat, then claims session.lock.
    world::OpenResult open(std::string_view worldDir, i64 nowMillis);

    // Creates a new one. Fails with IoError rather than overwriting if a
    // level.dat is already there.
    world::OpenResult create(std::string_view worldDir, i64 seed, i64 nowMillis);

    // **Reads level.dat without claiming the world**, for a world list.
    //
    // This exists because `open()` is not read-only and cannot be made so:
    // it writes session.lock, and `close()` rewrites level.dat. Listing the
    // saves folder through open/close would therefore re-stamp LastPlayed on
    // every world the player merely *looked* at, and would fight a lock that
    // another copy of the game legitimately holds. Nothing here writes, nothing
    // here locks, and the object's own state is untouched -- so it may be
    // called on a storage that is already open on a different world.
    //
    // False when the directory holds no level.dat, or one that will not decode.
    bool peekLevel(std::string_view worldDir, world::LevelData* out);

    // Writes level.dat back and drops the world. Chunks are the caller's to
    // flush first: this layer has no cache and therefore no dirty set.
    bool close(i64 nowMillis);

    bool isOpen() const { return open_; }
    std::string_view worldDir() const { return worldDir_; }

    world::LevelData& level() { return level_; }
    const world::LevelData& level() const { return level_; }
    bool saveLevel();

    bool hasChunk(i32 x, i32 z);
    bool loadChunk(i32 x, i32 z, world::ChunkColumn* out);
    bool saveChunk(const world::ChunkColumn& chunk);
    bool removeChunk(i32 x, i32 z);

    // Rewrites session.lock with a fresh timestamp and remembers it.
    bool refreshLock(i64 nowMillis);

    // Re-reads session.lock and compares it with what we last wrote. The
    // original game does this on every chunk save; we do not, because a console
    // cannot run two copies of the game at once and the check would double the
    // file operations on the hottest path there is. The caller runs it on a
    // timer instead. See docs/world-format.md.
    bool lockStillOurs();

    // Walks the base36 tree and reports every chunk file it finds. This is the
    // uncached form -- one directory listing per populated folder, up to 64x64
    // of them. The cached index described in docs/world-format.md sits on top
    // of this and is not built yet.
    bool forEachChunk(void* context, world::ChunkVisitor visit);

    // The leaf directory a chunk lives in, as an opaque key: `<x & 63>` and
    // `<z & 63>` packed into twelve bits. Two chunks with the same key share a
    // directory and are therefore answered by the same listing.
    u32 chunkGroupKey(i32 x, i32 z) const
    {
        return u32(x & 63) | (u32(z & 63) << 6);
    }

    // Lists the one leaf directory containing (x, z) and reports every chunk
    // file in it. **A directory that is not there yet is success with nothing
    // reported** -- an unexplored corner of the world is an answer, and failing
    // would make a caller fall back to a `stat` per chunk for ever.
    //
    // This is the cached-index primitive docs/world-format.md asks for, in its
    // incremental form: one listing settles up to a thousand chunks that are
    // spaced 64 apart, it costs nothing for a region the player never visits,
    // and there is no file to invalidate. See core/world/chunk_cache.hpp.
    bool listChunkGroup(i32 x, i32 z, void* context, world::ChunkVisitor visit);

private:
    bool readAndInflate(const char* path, usize maxFile, usize maxNbt,
                        std::vector<u8>* out);
    bool deflateAndWrite(const char* path, ConstByteSpan nbt);
    bool ensureChunkDir(const ChunkPath& dir);
    bool writeLock(i64 nowMillis);
    bool pathFor(i32 x, i32 z, ChunkPath* out) const;
    void levelPath(ChunkPath* out) const;
    void lockPath(ChunkPath* out) const;

    io::FileSystem& fs_;
    std::string worldDir_;
    world::LevelData level_;
    i64 lockValue_ = 0;
    bool open_ = false;

    // Chunks save in bursts that share a directory, so remembering the last one
    // created turns two mkdir calls per chunk into two per folder. On a device
    // where every file operation is an IPC round trip, that is the kind of
    // saving that actually shows up.
    std::string lastChunkDir_;
};

}  // namespace mc::alpha
