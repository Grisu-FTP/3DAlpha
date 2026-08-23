#pragma once

// The `storage` slot contract.
//
// Unlike the platform seams, storage is bound statically -- `mcver::Storage` is
// a `using` alias to whichever implementation the version manifest selected, so
// there is no vtable and the compiler inlines through it. That means the
// contract cannot be an abstract base class; it is this comment, enforced by
// the fact that code using `mcver::Storage` will not compile if a member is
// missing.
//
// An implementation must provide:
//
//   explicit Storage(io::FileSystem& fs);
//
//   OpenResult open(std::string_view worldDir, i64 nowMillis);
//   OpenResult create(std::string_view worldDir, i64 seed, i64 nowMillis);
//   bool       close(i64 nowMillis);
//
//   LevelData&       level();
//   const LevelData& level() const;
//   bool             saveLevel();
//
//   bool hasChunk(i32 x, i32 z);
//   bool loadChunk(i32 x, i32 z, ChunkColumn* out);
//   bool saveChunk(const ChunkColumn& chunk);
//
//   bool refreshLock(i64 nowMillis);
//   bool lockStillOurs();
//
//   bool forEachChunk(void* context, ChunkVisitor visit);
//
//   u32  chunkGroupKey(i32 x, i32 z) const;
//   bool listChunkGroup(i32 x, i32 z, void* context, ChunkVisitor visit);
//
// The last two are what lets a cache answer "does the world have this chunk"
// without a `stat` per chunk, and without knowing the on-disk layout.
//
// A **group** is the set of chunks whose existence one listing answers. For the
// Alpha format that is a leaf directory -- `<x & 63>/<z & 63>` -- so a group
// holds only chunks spaced 64 apart, and one listing settles every one of them
// for the rest of the session. `chunkGroupKey` names the group a chunk belongs
// to; two chunks with the same key are answered by the same listing.
// `listChunkGroup` reports every chunk in the group containing (x, z), and
// succeeds on a group that does not exist on disk yet -- an empty group is an
// answer, not a failure.
//
// A backend whose existence check is already cheap (a packed region's header is
// its own index) may give every chunk its own key and report just that chunk,
// which degenerates to one `hasChunk` per question and stays correct.
//
// `nowMillis` is passed in rather than read from a clock. Core has no clock
// seam, the value only ever lands in session.lock and LastPlayed, and tests
// need it to be deterministic.

#include "core/util/types.hpp"

namespace mc::world {

// Why opening a world failed. These are distinguished because the UI has to
// say different things: "another copy of the game has this open" is not the
// same problem as "this folder is not a world".
enum class OpenResult {
    Ok,
    NotAWorld,  // no level.dat where one was expected
    Corrupt,    // level.dat is there and unreadable
    Locked,     // session.lock moved under us
    IoError,
};

const char* describeOpenResult(OpenResult result);

// Called for each chunk found by a world scan. Returns false to stop early.
using ChunkVisitor = bool (*)(void* context, i32 chunkX, i32 chunkZ);

}  // namespace mc::world
