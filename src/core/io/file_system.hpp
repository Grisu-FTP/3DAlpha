#pragma once

// The file-system interface, one of the narrow platform seams.
//
// This is deliberately whole-file: read a file, write a file, list a directory.
// Both callers -- the world storage and the texture-pack importer -- always
// hold the complete buffer, and a streaming API would only add state for
// nothing. It also keeps the 3DS implementation honest: every operation is one
// IPC round trip to the FS sysmodule, so an interface that encourages many
// small calls encourages the expensive thing.
//
// This is one of the few places the project accepts a vtable. It is called once
// per file, never per block, and the alternative -- binding the platform
// statically like the version slots -- would stop the host tests from injecting
// anything. See docs/architecture.md.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::io {

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
};

}  // namespace mc::io
