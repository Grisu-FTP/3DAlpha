#pragma once

// The file-system interface, one of the narrow platform seams.
//
// **Whole-file first, positional second.** Read a file, write a file, list a
// directory: that is what the texture-pack importer and the Alpha chunk-file
// storage want, because both always hold the complete buffer, and it keeps the
// 3DS implementation honest -- every operation is one IPC round trip to the FS
// sysmodule, so an interface that encourages many small calls encourages the
// expensive thing.
//
// This header used to say that was the *whole* interface, and that "a streaming
// API would only add state for nothing". A packed world's region container is
// what changed the arithmetic: it holds ~1,024 chunks in one file, so serving a
// chunk through readFile would read ~3 MB to hand back ~3 KB. `RandomAccessFile`
// below exists for that one caller. It is not a general invitation -- anything
// that can name a whole file should still read a whole file.
//
// Positional rather than seek-and-read on purpose: libctru's devoptab maps
// read() onto FSFILE_Read, which **takes a u64 offset**, so pread/pwrite cost
// the same one IPC round trip a seek pair would have cost two of.
//
// This is one of the few places the project accepts a vtable. It is called once
// per file, never per block, and the alternative -- binding the platform
// statically like the version slots -- would stop the host tests from injecting
// anything. See docs/architecture.md.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <memory>
#include <vector>

namespace mc::io {

// One open file, read and written at explicit offsets.
//
// Owns its descriptor and closes it when destroyed. Held open for as long as
// the caller wants it, which is the point: a region container pays one open and
// then seeks, against the Alpha layout's one open per chunk.
//
// Every method is exact -- a short read is a failure, not a partial success.
// The caller knows how many bytes its own format says are there, and a
// truncated region file should be reported as broken rather than quietly
// returning half a chunk.
class RandomAccessFile {
public:
    virtual ~RandomAccessFile() = default;

    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    // Fills `out` completely from `offset`. False if the file ends first.
    virtual bool readAt(u64 offset, ByteSpan out) = 0;

    // Writes `data` at `offset`, extending the file if it ends before that.
    // Bytes between the old end and `offset` read back as zero, which is what
    // a sector allocator that skips a hole depends on.
    virtual bool writeAt(u64 offset, ConstByteSpan data) = 0;

    virtual bool size(u64* out) = 0;

    // fsync. A region commit is only crash-safe if the payload reaches the
    // card before the header that points at it, and this is what orders them.
    virtual bool flush() = 0;

protected:
    RandomAccessFile() = default;
};

// FAT32's long-name limit plus a terminator.
inline constexpr usize kMaxNameLength = 256;

struct DirEntry {
    char name[kMaxNameLength];
    bool isDirectory;
};

// Returns false to stop the walk early.
using DirVisitor = bool (*)(void* context, const DirEntry& entry);

class FileSystem {
public:
    virtual ~FileSystem() = default;

    // Appends the whole file to *out. Fails if the file is larger than
    // maxSize -- world folders come off a card the user can put anything on,
    // and a 2 GB "chunk file" should be a refusal, not an allocation.
    virtual bool readFile(const char* path, std::vector<u8>* out, usize maxSize) = 0;

    // Writes to a sibling temporary and renames over the target, so a console
    // switched off mid-write leaves either the old file or the new one.
    virtual bool writeFileAtomic(const char* path, ConstByteSpan data) = 0;

    virtual bool exists(const char* path) = 0;
    virtual bool isDirectory(const char* path) = 0;

    // The size of a file without reading it. One stat, where readFile is one
    // open, one or more reads and a close -- which is the difference between
    // listing the jars on a card and loading them all into memory to find out
    // how big they are. False if the path is missing or is a directory.
    virtual bool fileSize(const char* path, usize* out) = 0;

    // Creates every missing component, like mkdir -p. Succeeds if it is
    // already there.
    virtual bool makeDirectories(const char* path) = 0;

    virtual bool removeFile(const char* path) = 0;

    // Removes an *empty* directory. Not recursive: the recursion lives in
    // core, where a test can run it, and the platform layer stays one syscall
    // per method. See world::deleteWorld.
    virtual bool removeDirectory(const char* path) = 0;

    // Visits every entry except "." and "..". Returns false if the directory
    // could not be opened; stopping early through the visitor is success.
    virtual bool listDirectory(const char* path, void* context, DirVisitor visit) = 0;

    // Opens one file for positional reads and writes. Null if it could not be
    // opened -- which, with `create` false, includes "it is not there".
    //
    // The returned handle is the only allocation here, taken once per region
    // file rather than once per chunk, so it stays out of the frame path.
    virtual std::unique_ptr<RandomAccessFile> openRandomAccess(const char* path,
                                                               bool create) = 0;

    // Moves a file or directory. **Not atomic everywhere**: POSIX replaces an
    // existing target atomically and FAT does not, so a caller that needs the
    // target gone first should remove it first. Used to commit a conversion by
    // moving a finished staging directory into place.
    virtual bool rename(const char* from, const char* to) = 0;
};

}  // namespace mc::io
