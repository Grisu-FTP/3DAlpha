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
