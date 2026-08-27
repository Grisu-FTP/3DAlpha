#pragma once

// Turning a world from one on-disk shape into the other, without losing a byte.
//
// **The rule both directions follow: never destroy the source until the
// destination is complete and verified.** A conversion is the one operation
// here that touches every file a player owns, and the failure that matters is
// not "it did not work" -- it is "it half worked and the world is gone".
//
// **Nothing unrecognised is dropped.** Worlds pick up files from third-party
// tools and older servers, and a packer that silently discards what it does not
// understand is a data-loss bug waiting to happen. Anything that is not a chunk
// at its own canonical path goes into the manifest's blob verbatim and comes
// back at the same path. `3dalpha.ini` is the one file carried rather than
// stashed: it is ours, and it stays a plain readable file on both sides.
//
// **Verification lives here and not on the gameplay read path.** Playing a
// world does not check our CRC per chunk -- a payload is a gzip stream and
// inflate already checks gzip's own. Converting does, because "loses no data"
// is a promise that has to be checked rather than hoped for, and a conversion
// is a once-per-world operation that can afford it.
//
// **It runs where it is called**, synchronously, and the caller pumps a frame
// from the observer. The world is closed during a conversion -- it is started
// from the world list, not from inside a session -- so there is no I/O thread
// to hand it to and nothing else for the console to be doing.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"
#include "core/world/world_format.hpp"

#include <string_view>

namespace mc::world::format {

// The suffix on a staging directory, and the marker inside it that says the
// staged copy is complete. A directory carrying the suffix is never listed as a
// world; see world_list.cpp.
inline constexpr char kConvertingSuffix[] = ".converting";
inline constexpr char kCommitMarker[] = ".commit";

// Written into a world while it is being unpacked in place. Unpacking is the
// one direction that does not stage -- see the note in converter.cpp -- and
// this is what makes an interrupted one recoverable.
inline constexpr char kUnpackMarker[] = ".3dalpha-unpacking";

// The plain-text note a packed world carries, so a folder that Minecraft
// refuses to open still explains itself to whoever finds it.
inline constexpr char kReadmeName[] = "3DALPHA-PACKED.txt";

enum class ConvertResult {
    Ok,
    Cancelled,
    NotAWorld,
    AlreadyInFormat,
    NoSpace,
    SourceUnreadable,
    VerifyFailed,
    WriteFailed,
    ReservedName,
};

const char* describeConvertResult(ConvertResult result);

struct ConvertProgress {
    const char* stage = "";
    u32 filesDone = 0;
    u32 filesTotal = 0;
};

// Returning false cancels. Cancelling is always safe: it takes the same path a
// power cut does, and the original is untouched either way.
using ConvertObserver = bool (*)(void* context, const ConvertProgress& progress);

struct ConvertOptions {
    // Left free on the card after a successful conversion, so a world that
    // just barely fits does not leave the player unable to save it afterwards.
    u64 headroomBytes = 8u << 20;
    void* context = nullptr;
    ConvertObserver observe = nullptr;
    u32 observeEvery = 8;
};

// What the conversion will cost, for a screen that has to say so **before**
// starting rather than at 80 %.
struct ConvertEstimate {
    u64 sourceOnDisk = 0;
    u64 targetOnDisk = 0;
    u64 freeBytes = 0;
    u64 clusterSize = 0;
    u32 chunks = 0;
    u32 files = 0;
};

ConvertResult estimateConversion(io::FileSystem& fs, std::string_view worldDir,
                                 WorldFormat target, ConvertEstimate* out);

ConvertResult convertWorld(io::FileSystem& fs, std::string_view worldDir, WorldFormat target,
                           const ConvertOptions& options);

// Finishes or discards anything a previous conversion left behind. Idempotent,
// and cheap when there is nothing to do. Call it before listing worlds.
void recoverConversions(io::FileSystem& fs, std::string_view savesDir);

}  // namespace mc::world::format
