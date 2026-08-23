#include "core/texture/zip_archive.hpp"

#include "core/util/compress.hpp"

#include <cstring>

namespace mc::texture {

namespace {

constexpr u32 kSigCentral = 0x02014B50;
constexpr u32 kSigLocal = 0x04034B50;
constexpr u32 kSigEnd = 0x06054B50;
constexpr u32 kSigZip64End = 0x06064B50;

constexpr u16 kMethodStore = 0;
constexpr u16 kMethodDeflate = 8;

// The fixed part of each record, before any variable-length field.
constexpr usize kEndSize = 22;
constexpr usize kCentralSize = 46;
constexpr usize kLocalSize = 30;

// The EOCD may be followed by up to 64 KB of comment, and the search has to
// walk back over all of it.
constexpr usize kMaxComment = 0xFFFF;

u16 readLe16(const u8* p)
{
    return u16(u16(p[0]) | (u16(p[1]) << 8));
}

u32 readLe32(const u8* p)
{
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

}  // namespace

const char* zipErrorText(ZipError error)
{
    switch (error) {
        case ZipError::Ok: return "ok";
        case ZipError::NoEndRecord: return "not a zip file";
        case ZipError::Zip64: return "zip64 archives are not supported";
        case ZipError::Multidisk: return "split archives are not supported";
        case ZipError::BadDirectory: return "the archive directory is corrupt";
        case ZipError::BadEntry: return "an entry header is corrupt";
        case ZipError::UnsafeName: return "an entry name is unsafe";
        case ZipError::Unsupported: return "an entry uses an unsupported compression method";
        case ZipError::InflateFailed: return "an entry could not be decompressed";
        case ZipError::TooLarge: return "an entry is too large";
    }
    return "unknown";
}

bool isSafeZipName(std::string_view name)
{
    if (name.empty() || name.size() >= 1024) {
        return false;
    }
    if (name.front() == '/') {
        return false;
    }
    // A backslash is a separator on the platform most packs are built on, and
    // treating it as an ordinary character would let "..\\.." through a check
    // that only looks for forward slashes.
    if (name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    // "C:" style prefixes. Rare in a zip and unambiguous when they appear.
    if (name.size() >= 2 && name[1] == ':') {
        return false;
    }

    // ".." as a whole path component, which is the only form that escapes.
    // "..png" and "a..b" are ordinary names.
    usize start = 0;
    while (start <= name.size()) {
        const usize slash = name.find('/', start);
        const usize end = slash == std::string_view::npos ? name.size() : slash;
        if (name.compare(start, end - start, "..") == 0) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

ZipError ZipArchive::open(ConstByteSpan bytes)
{
    bytes_ = ConstByteSpan();
    entries_.clear();

    if (bytes.size() < kEndSize) {
        return ZipError::NoEndRecord;
    }

    // Scan back for the EOCD signature. The last plausible position is
    // size - kEndSize; the earliest is that minus the maximum comment length.
    const usize last = bytes.size() - kEndSize;
    const usize floor = last > kMaxComment ? last - kMaxComment : 0;
    usize end = 0;
    bool found = false;
    for (usize i = last + 1; i-- > floor;) {
        if (readLe32(bytes.data() + i) == kSigEnd) {
            end = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return ZipError::NoEndRecord;
    }

    const u8* eocd = bytes.data() + end;
    const u16 disk = readLe16(eocd + 4);
    const u16 startDisk = readLe16(eocd + 6);
    const u16 countThisDisk = readLe16(eocd + 8);
    const u16 countTotal = readLe16(eocd + 10);
    const u32 directorySize = readLe32(eocd + 12);
    const u32 directoryOffset = readLe32(eocd + 16);

    if (disk != 0 || startDisk != 0 || countThisDisk != countTotal) {
        return ZipError::Multidisk;
    }
    // 0xFFFF/0xFFFFFFFF in any of these is the Zip64 escape, and a Zip64 EOCD
    // locator sitting just before the record says the same thing.
    if (countTotal == 0xFFFF || directorySize == 0xFFFFFFFFu
        || directoryOffset == 0xFFFFFFFFu) {
        return ZipError::Zip64;
    }
    if (end >= 20 && readLe32(bytes.data() + end - 20) == kSigZip64End) {
        return ZipError::Zip64;
    }
    if (usize(directoryOffset) + usize(directorySize) > bytes.size()) {
        return ZipError::BadDirectory;
    }

    entries_.reserve(countTotal);

    usize offset = directoryOffset;
    const usize directoryEnd = usize(directoryOffset) + usize(directorySize);

    for (u16 i = 0; i < countTotal; ++i) {
        if (offset + kCentralSize > directoryEnd) {
            return ZipError::BadDirectory;
        }
        const u8* record = bytes.data() + offset;
        if (readLe32(record) != kSigCentral) {
            return ZipError::BadDirectory;
        }

        const u16 method = readLe16(record + 10);
        const u32 crc = readLe32(record + 16);
        const u32 compressed = readLe32(record + 20);
        const u32 uncompressed = readLe32(record + 24);
        const u16 nameLength = readLe16(record + 28);
        const u16 extraLength = readLe16(record + 30);
        const u16 commentLength = readLe16(record + 32);
        const u32 localOffset = readLe32(record + 42);

        const usize recordSize =
            kCentralSize + usize(nameLength) + usize(extraLength) + usize(commentLength);
        if (offset + recordSize > directoryEnd) {
            return ZipError::BadDirectory;
        }

        const char* nameBytes = reinterpret_cast<const char*>(record + kCentralSize);
        const std::string_view name(nameBytes, nameLength);

        offset += recordSize;

        // A directory entry: zero length, name ending in a separator. Skipped
        // rather than refused -- a jar written by some tools has them and they
        // carry nothing.
        if (!name.empty() && name.back() == '/') {
            continue;
        }
        if (compressed == 0xFFFFFFFFu || uncompressed == 0xFFFFFFFFu
            || localOffset == 0xFFFFFFFFu) {
            return ZipError::Zip64;
        }
        if (!isSafeZipName(name)) {
            return ZipError::UnsafeName;
        }
        // An unsupported compression method is deliberately not fatal for the
        // archive. A pack with one bzip2 entry beside 57 ordinary ones is still
        // usable, and keeping the entry in the list with its method intact is
        // what lets `read` say why when something asks for that one.
        if (usize(localOffset) + kLocalSize > bytes.size()) {
            return ZipError::BadDirectory;
        }

        ZipEntry entry;
        entry.name.assign(name);
        entry.method = method;
        entry.crc = crc;
        entry.compressedSize = compressed;
        entry.uncompressedSize = uncompressed;
        entry.localHeaderOffset = localOffset;
        entries_.push_back(std::move(entry));
    }

    bytes_ = bytes;
    return ZipError::Ok;
}

const ZipEntry* ZipArchive::find(std::string_view name) const
{
    for (const ZipEntry& entry : entries_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

bool ZipArchive::dataOffset(const ZipEntry& entry, usize* out) const
{
    const usize header = entry.localHeaderOffset;
    if (header + kLocalSize > bytes_.size()) {
        return false;
    }
    const u8* local = bytes_.data() + header;
    if (readLe32(local) != kSigLocal) {
        return false;
    }
    // **Only these two fields are read.** The CRC and the two sizes beside them
    // are zero on any entry written with a data descriptor, which is nearly
    // every entry in a real jar. The central directory already gave us the
    // real values.
    const u16 nameLength = readLe16(local + 26);
    const u16 extraLength = readLe16(local + 28);

    const usize data = header + kLocalSize + usize(nameLength) + usize(extraLength);
    if (data + usize(entry.compressedSize) > bytes_.size()) {
        return false;
    }
    *out = data;
    return true;
}

ConstByteSpan ZipArchive::rawBytes(const ZipEntry& entry) const
{
    usize data = 0;
    if (!dataOffset(entry, &data)) {
        return ConstByteSpan();
    }
    return bytes_.subspan(data, entry.compressedSize);
}

ZipError ZipArchive::read(const ZipEntry& entry, std::vector<u8>* out) const
{
    out->clear();

    usize data = 0;
    if (!dataOffset(entry, &data)) {
        return ZipError::BadEntry;
    }
    const ConstByteSpan stored = bytes_.subspan(data, entry.compressedSize);

    if (entry.method == kMethodStore) {
        if (entry.compressedSize != entry.uncompressedSize) {
            return ZipError::BadEntry;
        }
        out->assign(stored.begin(), stored.end());
        return ZipError::Ok;
    }
    if (entry.method != kMethodDeflate) {
        return ZipError::Unsupported;
    }
    if (entry.uncompressedSize == 0) {
        // A zero-length deflated entry is two bytes of empty block, and
        // zip::decompress refuses a zero ceiling before it looks at them. The
        // answer is an empty buffer either way.
        return ZipError::Ok;
    }

    out->reserve(entry.uncompressedSize);
    // Bounded by the size the directory promised, so a lying entry cannot make
    // this allocate without limit.
    if (!zip::decompress(stored, *out, zip::Wrapper::Raw, entry.uncompressedSize)) {
        out->clear();
        return ZipError::InflateFailed;
    }
    if (out->size() != entry.uncompressedSize) {
        out->clear();
        return ZipError::BadEntry;
    }
    return ZipError::Ok;
}

}  // namespace mc::texture
