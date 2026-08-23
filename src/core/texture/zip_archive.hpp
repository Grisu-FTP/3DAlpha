#pragma once

// Reading a zip -- which is also what a Minecraft jar is.
//
// **The central directory is the only source of truth here, and that is a
// measurement rather than a preference.** 497 of the 538 entries in a real
// a1.1.2 client jar have general-purpose flag bit 3 set, which means their
// local headers carry zeroed CRC and size fields and the real values trail the
// compressed data in a descriptor. A reader that trusts local headers therefore
// reads garbage lengths for 92 % of that jar. Local headers are opened here for
// exactly one purpose: to learn how many bytes of name and extra field to skip
// before the data starts.
//
// The jar also mixes framings -- 41 stored entries and 497 deflated ones -- so
// both paths are exercised by the first real file this ever sees.
//
// Zip64 is refused rather than parsed. A texture pack that needs 64-bit offsets
// is over four gigabytes.
//
// Whole-buffer, like everything else that crosses io::FileSystem: the caller
// holds the file and this views into it. A jar is ~900 KB against a 16-40 MB
// newlib heap, so there is nothing to stream.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::texture {

enum class ZipError {
    Ok,
    NoEndRecord,   // no EOCD signature in the last 64 KB
    Zip64,         // needs 64-bit offsets
    Multidisk,     // spanned across volumes
    BadDirectory,  // a central-directory record does not fit or does not match
    BadEntry,      // a local header is missing or inconsistent
    UnsafeName,    // "..", an absolute path, or a backslash
    Unsupported,   // a compression method other than store or deflate
    InflateFailed,
    TooLarge,
};

const char* zipErrorText(ZipError error);

struct ZipEntry {
    std::string name;
    u16 method = 0;  // 0 store, 8 deflate; nothing else is accepted
    u32 crc = 0;
    u32 compressedSize = 0;
    u32 uncompressedSize = 0;
    u32 localHeaderOffset = 0;
};

class ZipArchive {
public:
    // Parses the central directory. The bytes must outlive the archive: entries
    // are read back out of this same span.
    ZipError open(ConstByteSpan bytes);

    const std::vector<ZipEntry>& entries() const { return entries_; }

    // Exact name match, case-sensitive -- zip names are bytes and a pack that
    // ships "Terrain.png" is a pack that did not work in the original game
    // either. Returns nullptr when absent.
    const ZipEntry* find(std::string_view name) const;

    // Decompresses into *out (clearing it first).
    ZipError read(const ZipEntry& entry, std::vector<u8>* out) const;

    // The entry's bytes exactly as they are stored, still in whatever framing
    // the archive used. This is what the jar importer copies: re-emitting these
    // verbatim means a 900 KB jar is extracted without a single inflate or
    // deflate call. Empty span on failure.
    ConstByteSpan rawBytes(const ZipEntry& entry) const;

private:
    // Locates the data of an entry, following its local header past the
    // variable-length name and extra fields. Returns false if the header is
    // missing or runs off the end.
    bool dataOffset(const ZipEntry& entry, usize* out) const;

    ConstByteSpan bytes_;
    std::vector<ZipEntry> entries_;
};

// True when a name is safe to write to a card relative to a directory: no
// leading slash, no drive letter, no "..", no backslash, no NUL, non-empty.
// Applied to every name read out of an archive, because a pack is a file a
// player downloaded from somewhere.
bool isSafeZipName(std::string_view name);

}  // namespace mc::texture
