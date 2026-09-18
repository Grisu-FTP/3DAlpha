#pragma once

// A world, flattened into files that can cross a link, and the staging
// directory the far end assembles one in.
//
// **Format-agnostic on purpose.** A world is copied between consoles the same
// way `copyWorld` copies one on the card: file for file, in whichever shape the
// source is in. An import is a backup that travelled, not a conversion -- so
// this layer never looks inside a chunk, and a packed world arrives packed.
//
// **Everything a receiver is told arrives off a radio**, which is why the path
// rules below are here and are tested. A sender on a modified build can name a
// file anything at all; `safeRelativePath` is the one thing standing between
// that and a write outside the folder the player asked for.
//
// It is core rather than platform code for the usual reason: the console cannot
// run a unit test, and every rule here -- which paths are refused, what the
// limits are, what an interrupted import leaves behind -- is a rule worth
// testing. See docs/architecture.md.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

// The bounds a receiver holds a sender to. None of them is reachable by a world
// this console could have made; they exist so a malformed or hostile offer is
// refused in one comparison rather than after filling the card.
inline constexpr usize kMaxTransferPath = 200;
inline constexpr u32 kMaxTransferFiles = 200000;
inline constexpr u64 kMaxTransferFileBytes = 64ull << 20;
inline constexpr u64 kMaxTransferBytes = 2ull << 30;

// Where an import is assembled before it becomes a world. Dot-prefixed, which
// is what keeps it out of `listWorlds` -- see the name filter in world_list.cpp
// -- and fixed rather than per-world, because one import runs at a time and a
// fixed name is one a recovery pass can find without knowing what was being
// imported.
inline constexpr char kImportStagingName[] = ".importing";

// One file of a world, as it crosses.
struct TransferFile {
    // Relative to the world directory, '/' separated, always passing
    // `safeRelativePath`.
    std::string path;
    u64 size = 0;
};

// The whole world as a list. Directories are not carried: an empty one holds
// nothing and the receiver makes every parent on the way to a file.
struct TransferManifest {
    std::vector<TransferFile> files;
    u64 totalBytes = 0;
};

// Whether a path handed over by the far end may be written under a directory we
// own. Refuses an empty path, an absolute one, a backslash (a card is read on a
// PC too), any "." or ".." component, an empty component, a trailing separator,
// a control character, and anything longer than `kMaxTransferPath`.
bool safeRelativePath(std::string_view path);

// Walks `worldDir` into a manifest, sorted by path so two consoles agree on the
// order and a transfer is reproducible. False when the directory cannot be
// read, when it is not a world, or when what is in it exceeds the limits above.
bool buildTransferManifest(io::FileSystem& fs, std::string_view worldDir,
                           TransferManifest* out);

// Assembles an incoming world under `savesDir`/`kImportStagingName`, then moves
// it into place under the name the player chose.
//
// **Nothing is written outside the staging directory until `commit`**, so an
// import that is abandoned, refused or interrupted leaves a dot-directory the
// next `discardStagedImport` removes and never a half-world the list would
// offer.
class ImportStaging {
public:
    ImportStaging(io::FileSystem& fs, std::string_view savesDir);

    ImportStaging(const ImportStaging&) = delete;
    ImportStaging& operator=(const ImportStaging&) = delete;

    // Clears anything a previous import left and makes the directory. False
    // when the card refuses either.
    bool begin();

    // Starts a file. `path` is checked with `safeRelativePath` before anything
    // is touched; false means the offer was bad and the caller should stop.
    bool beginFile(std::string_view path, u64 size);

    // Appends to the file `beginFile` named. False when more bytes arrive than
    // were declared, or when the card refuses the write.
    bool writeChunk(const u8* data, usize size);

    // The declared size has arrived. False when it has not.
    bool endFile();

    // Every file arrived. Moves the staging directory to `savesDir`/`name`,
    // which must not already exist, and checks that what arrived really is a
    // world before it is given a world's name.
    bool commit(std::string_view name);

    // Throws the staging directory away. Safe on one that was never begun.
    void discard();

    u64 bytesWritten() const { return bytesWritten_; }
    u32 filesWritten() const { return filesWritten_; }

private:
    io::FileSystem& fs_;
    std::string savesDir_;
    std::string staging_;

    std::unique_ptr<io::RandomAccessFile> file_;
    u64 declared_ = 0;
    u64 received_ = 0;
    u64 bytesWritten_ = 0;
    u32 filesWritten_ = 0;
    bool open_ = false;
};

// Removes a staging directory left by an import that never finished. Idempotent
// and cheap when there is nothing there. Called beside `recoverConversions`,
// before the world list is read.
void discardStagedImport(io::FileSystem& fs, std::string_view savesDir);

}  // namespace mc::world
