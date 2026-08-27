#pragma once

// `world.3dm` -- what a packed world keeps that is not a chunk.
//
// Two jobs, and the second is the one that makes packing lossless.
//
// **A metadata block the world list can read on its own.** Name, last played,
// seed, chunk count, sizes. It sits in a fixed 64-byte header, so listing a
// packed world costs one open and one 64-byte read -- no NBT, no inflate, no
// region touched. That makes a packed world *cheaper* to list than a folder
// one, which has to read and gunzip `level.dat` per world.
//
// **A verbatim store for every file that is not a chunk.** `level.dat`,
// `session.lock`, and anything a third-party tool or an older server left
// behind. Worlds pick up files nobody planned for, and a packer that silently
// drops what it does not recognise is a data-loss bug waiting to happen. Every
// such file goes in byte for byte, with its path and its CRC, and comes back
// out at the same path.
//
// **`level.dat` moves in here on purpose.** Left on disk, a packed world
// copied to a PC would look to Minecraft like a world with a level and no
// chunks, and it would cheerfully generate fresh terrain over the top. A folder
// the real game refuses to open is the safer failure, and the README the packer
// writes beside this file explains what happened and how to undo it.
//
// **Chunks are deliberately not listed here.** The region directories already
// record which chunks exist and where; repeating that would put ~640 KB of
// entries in a file that is read whole on every world open. What keeps the
// round trip exact without listing them is a strict rule applied when packing:
// a file counts as a chunk **only** if its name parses as `c.<b36>.<b36>.dat`
// *and* it sits at exactly the path that chunk belongs at. A chunk file in the
// wrong leaf directory -- which tools do produce -- is not a chunk, it is a
// stray file, and it is stashed at its own path like any other.

#include "core/io/file_system.hpp"
#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::world::format {

inline constexpr char kManifestName[] = "world.3dm";

// The blob area is held in memory whole, both while packing and for as long as
// a packed world is open, so it needs a ceiling that a 40 MB heap can live
// with. `level.dat` is about 2 KB; anything approaching this is a world with
// something very unusual in it, and saying so is better than failing later.
inline constexpr usize kMaxBlobBytes = 4u << 20;

struct ManifestEntry {
    // Relative to the world directory, '/' separated.
    std::string path;
    // An empty directory is worth recording: restoring a tree that had one
    // without it would not be the tree that was packed.
    bool isDirectory = false;
    u32 byteLength = 0;
    u32 crc = 0;
    u64 blobOffset = 0;
};

// What the world list needs, without opening anything else.
struct ManifestSummary {
    i64 lastPlayed = 0;
    i64 randomSeed = 0;
    u32 chunkCount = 0;
    u32 regionCount = 0;
    u64 unpackedBytes = 0;
};

class Manifest {
public:
    ManifestSummary summary;

    void clear();

    // Adds a file's bytes to the blob. False if it would pass kMaxBlobBytes.
    bool addFile(std::string_view path, ConstByteSpan bytes);
    void addDirectory(std::string_view path);

    // Points *out at the stored bytes, which live in this object.
    bool file(std::string_view path, ConstByteSpan* out) const;
    bool replaceFile(std::string_view path, ConstByteSpan bytes);

    const std::vector<ManifestEntry>& entries() const { return entries_; }

    bool load(io::FileSystem& fs, const char* path);
    bool save(io::FileSystem& fs, const char* path) const;

    // The header alone -- one open and one 64-byte read. This is the world
    // list's path and the reason the metadata is a fixed block rather than
    // entries to be scanned for.
    static bool peek(io::FileSystem& fs, const char* path, ManifestSummary* out);

private:
    std::vector<ManifestEntry> entries_;
    std::vector<u8> blob_;
};

}  // namespace mc::world::format
