#pragma once

// FileSystem on POSIX file descriptors.
//
// One implementation serves both targets. devkitARM's newlib provides the
// descriptor API, and libctru's devoptab maps it almost directly onto the FS
// service: read() is one FSFILE_Read, fsync() is one FSFILE_Flush, and
// readdir() pulls 32 entries per FSDIR_Read and serves the rest from a cache.
// That is why this uses open/read/write and not fopen/fread -- the cost worth
// avoiding is newlib's FILE layer stacked on top, with its own buffering,
// _reent lookup and per-call locking, not the devoptab underneath it. The
// measurements behind that are in docs/3ds-performance.md.
//
// The consequence for the host build is the more valuable half: the code the
// tests exercise is the code that runs on the console.

#include "core/io/file_system.hpp"

namespace mc::io {

class PosixFileSystem : public FileSystem {
public:
    bool readFile(const char* path, std::vector<u8>* out, usize maxSize) override;
    bool writeFileAtomic(const char* path, ConstByteSpan data) override;
    bool exists(const char* path) override;
    bool isDirectory(const char* path) override;
    bool fileSize(const char* path, usize* out) override;
    bool makeDirectories(const char* path) override;
    bool removeFile(const char* path) override;
    bool removeDirectory(const char* path) override;
    bool listDirectory(const char* path, void* context, DirVisitor visit) override;
};

}  // namespace mc::io
