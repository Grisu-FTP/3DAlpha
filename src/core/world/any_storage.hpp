#pragma once

// One storage that is either of the two, decided by the world being opened.
//
// **Why this exists at all.** The version slot system binds exactly one storage
// implementation per binary, statically, with no vtable -- see
// docs/build-versions.md. That is right for a *version* difference: an a1.1.2
// build has no business carrying b1.7.3's format. But Folder and Packed are not
// a version difference. They are two shapes the same version's world can take
// on one card, and a player converting between them needs both in one binary at
// one time.
//
// So the slot is left exactly as it was -- `mcver::Storage` is still
// `AlphaChunkFileStorage`, still statically bound, still inlined -- and this
// composes it with a packed backend that is version-agnostic. The tag is an
// enum and the dispatch is a switch, not a vtable: every call here happens once
// per chunk, never per block, against four to six IPC round trips and a gzip
// inflate. A predictable branch is not measurable next to that, and a vtable
// would anchor a backend that `--gc-sections` could otherwise drop.
//
// **The format is never passed in except when creating.** `open` reads it off
// the folder, because that is the only source that cannot be wrong: a world
// copied onto the card from a PC has no setting to register, and a setting kept
// anywhere else could disagree with the disk. `create` is the one moment there
// is no disk to read, so it is the one place a caller says which to make.

#include "core/io/file_system.hpp"
#include "core/world/format/packed_storage.hpp"
#include "core/world/storage.hpp"
#include "core/world/world_format.hpp"

#include "version_slots.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace mc::world {

class AnyStorage {
public:
    explicit AnyStorage(io::FileSystem& fs) : fs_(fs), folder_(fs) {}

    AnyStorage(const AnyStorage&) = delete;
    AnyStorage& operator=(const AnyStorage&) = delete;

    WorldFormat format() const { return format_; }

    OpenResult open(std::string_view worldDir, i64 nowMillis);
    OpenResult create(std::string_view worldDir, i64 seed, i64 nowMillis, WorldFormat format);
    bool close(i64 nowMillis);

    // Reads a world's level without claiming it, whichever shape it is in.
    bool peekLevel(std::string_view worldDir, LevelData* out);

    bool isOpen() const;
    std::string_view worldDir() const;

    LevelData& level();
    const LevelData& level() const;
    bool saveLevel();

    bool hasChunk(i32 x, i32 z);
    bool loadChunk(i32 x, i32 z, ChunkColumn* out);
    bool saveChunk(const ChunkColumn& chunk);
    bool removeChunk(i32 x, i32 z);
    bool commit();

    bool refreshLock(i64 nowMillis);
    bool lockStillOurs();

    bool forEachChunk(void* context, ChunkVisitor visit);

    // Pure and cheap in both backends, which is what lets the cache call it
    // while holding its table lock.
    u64 chunkGroupKey(i32 x, i32 z) const;
    bool listChunkGroup(i32 x, i32 z, void* context, ChunkVisitor visit);

private:
    // Allocated only when a packed world is actually opened, so a session that
    // never touches one never pays for it.
    format::PackedStorage& packed();
    const format::PackedStorage* packedIfAny() const { return packed_.get(); }

    io::FileSystem& fs_;
    WorldFormat format_ = WorldFormat::Unknown;
    mcver::Storage folder_;
    std::unique_ptr<format::PackedStorage> packed_;
};

}  // namespace mc::world
