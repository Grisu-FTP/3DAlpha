#include "core/world/format/manifest.hpp"

#include "core/util/crc32.hpp"

#include <cstring>

namespace mc::world::format {

namespace {

constexpr u8 kMagic[4] = {'3', 'D', 'M', '1'};
constexpr u16 kFormatVersion = 1;
constexpr u32 kHeaderBytes = 64;
constexpr u32 kHeaderCrcOffset = 60;

// A manifest larger than this is not one of ours.
constexpr usize kMaxFileBytes = kMaxBlobBytes + (1u << 20);

void putU16(u8* p, u16 v)
{
    p[0] = u8(v);
    p[1] = u8(v >> 8);
}

void putU32(u8* p, u32 v)
{
    p[0] = u8(v);
    p[1] = u8(v >> 8);
    p[2] = u8(v >> 16);
    p[3] = u8(v >> 24);
}

void putU64(u8* p, u64 v)
{
    putU32(p, u32(v));
    putU32(p + 4, u32(v >> 32));
}

u16 getU16(const u8* p)
{
    return u16(u16(p[0]) | u16(u16(p[1]) << 8));
}

u32 getU32(const u8* p)
{
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

u64 getU64(const u8* p)
{
    return u64(getU32(p)) | (u64(getU32(p + 4)) << 32);
}

void appendU16(std::vector<u8>& out, u16 v)
{
    u8 buffer[2];
    putU16(buffer, v);
    out.insert(out.end(), buffer, buffer + sizeof(buffer));
}

void appendU32(std::vector<u8>& out, u32 v)
{
    u8 buffer[4];
    putU32(buffer, v);
    out.insert(out.end(), buffer, buffer + sizeof(buffer));
}

void appendU64(std::vector<u8>& out, u64 v)
{
    u8 buffer[8];
    putU64(buffer, v);
    out.insert(out.end(), buffer, buffer + sizeof(buffer));
}

}  // namespace

void Manifest::clear()
{
    summary = ManifestSummary();
    entries_.clear();
    blob_.clear();
}

bool Manifest::addFile(std::string_view path, ConstByteSpan bytes)
{
    if (blob_.size() + bytes.size() > kMaxBlobBytes) {
        return false;
    }
    ManifestEntry entry;
    entry.path.assign(path);
    entry.isDirectory = false;
    entry.byteLength = u32(bytes.size());
    entry.crc = util::crc32(bytes);
    entry.blobOffset = u64(blob_.size());
    blob_.insert(blob_.end(), bytes.begin(), bytes.end());
    entries_.push_back(std::move(entry));
    return true;
}

void Manifest::addDirectory(std::string_view path)
{
    ManifestEntry entry;
    entry.path.assign(path);
    entry.isDirectory = true;
    entries_.push_back(std::move(entry));
}

bool Manifest::file(std::string_view path, ConstByteSpan* out) const
{
    for (const ManifestEntry& entry : entries_) {
        if (entry.isDirectory || entry.path != path) {
            continue;
        }
        *out = ConstByteSpan(blob_.data() + entry.blobOffset, entry.byteLength);
        return true;
    }
    return false;
}

bool Manifest::replaceFile(std::string_view path, ConstByteSpan bytes)
{
    // Rebuilt rather than patched in place: an entry that grew would have to
    // move every offset after it, and this runs once per autosave over a few
    // kilobytes. Simplicity is worth more here than the copy costs.
    std::vector<ManifestEntry> kept;
    kept.reserve(entries_.size());
    std::vector<u8> rebuilt;
    rebuilt.reserve(blob_.size() + bytes.size());

    bool replaced = false;
    for (const ManifestEntry& entry : entries_) {
        if (entry.isDirectory) {
            kept.push_back(entry);
            continue;
        }
        ManifestEntry copy = entry;
        const bool isTarget = entry.path == path;
        const ConstByteSpan source =
            isTarget ? bytes : ConstByteSpan(blob_.data() + entry.blobOffset, entry.byteLength);
        copy.byteLength = u32(source.size());
        copy.crc = util::crc32(source);
        copy.blobOffset = u64(rebuilt.size());
        if (rebuilt.size() + source.size() > kMaxBlobBytes) {
            return false;
        }
        rebuilt.insert(rebuilt.end(), source.begin(), source.end());
        kept.push_back(std::move(copy));
        replaced = replaced || isTarget;
    }

    entries_ = std::move(kept);
    blob_ = std::move(rebuilt);
    if (!replaced) {
        return addFile(path, bytes);
    }
    return true;
}

bool Manifest::save(io::FileSystem& fs, const char* path) const
{
    std::vector<u8> body;
    for (const ManifestEntry& entry : entries_) {
        if (entry.path.size() > 0xFFFFu) {
            return false;
        }
        appendU16(body, u16(entry.path.size()));
        body.insert(body.end(), entry.path.begin(), entry.path.end());
        body.push_back(entry.isDirectory ? 1u : 0u);
        appendU32(body, entry.byteLength);
        appendU32(body, entry.crc);
        appendU64(body, entry.blobOffset);
    }

    std::vector<u8> out;
    out.resize(kHeaderBytes);
    const u32 blobOffset = u32(kHeaderBytes + body.size());

    u8* header = out.data();
    std::memcpy(header, kMagic, sizeof(kMagic));
    putU16(header + 4, kFormatVersion);
    putU16(header + 6, u16(kHeaderBytes));
    putU32(header + 8, u32(entries_.size()));
    putU32(header + 12, blobOffset);
    putU64(header + 16, u64(blob_.size()));
    putU64(header + 24, u64(summary.lastPlayed));
    putU64(header + 32, u64(summary.randomSeed));
    putU32(header + 40, summary.chunkCount);
    putU32(header + 44, summary.regionCount);
    putU64(header + 48, summary.unpackedBytes);
    putU32(header + 56, 0);
    putU32(header + kHeaderCrcOffset, util::crc32(ConstByteSpan(header, kHeaderCrcOffset)));

    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), blob_.begin(), blob_.end());

    // Whole and atomic. A manifest is a few kilobytes and it is the one file
    // that says what the world is, so a half-written one is not a state worth
    // being able to reach.
    return fs.writeFileAtomic(path, ConstByteSpan(out.data(), out.size()));
}

bool Manifest::peek(io::FileSystem& fs, const char* path, ManifestSummary* out)
{
    auto file = fs.openRandomAccess(path, false);
    if (file == nullptr) {
        return false;
    }
    u8 header[kHeaderBytes];
    if (!file->readAt(0, ByteSpan(header, sizeof(header)))) {
        return false;
    }
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0
        || getU16(header + 4) != kFormatVersion) {
        return false;
    }
    if (getU32(header + kHeaderCrcOffset)
        != util::crc32(ConstByteSpan(header, kHeaderCrcOffset))) {
        return false;
    }
    out->lastPlayed = i64(getU64(header + 24));
    out->randomSeed = i64(getU64(header + 32));
    out->chunkCount = getU32(header + 40);
    out->regionCount = getU32(header + 44);
    out->unpackedBytes = getU64(header + 48);
    return true;
}

bool Manifest::load(io::FileSystem& fs, const char* path)
{
    clear();

    std::vector<u8> bytes;
    if (!fs.readFile(path, &bytes, kMaxFileBytes)) {
        return false;
    }
    if (bytes.size() < kHeaderBytes) {
        return false;
    }
    const u8* header = bytes.data();
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0
        || getU16(header + 4) != kFormatVersion) {
        return false;
    }
    if (getU32(header + kHeaderCrcOffset)
        != util::crc32(ConstByteSpan(header, kHeaderCrcOffset))) {
        return false;
    }

    const u32 entryCount = getU32(header + 8);
    const u32 blobOffset = getU32(header + 12);
    const u64 blobBytes = getU64(header + 16);
    if (u64(blobOffset) + blobBytes > bytes.size() || blobBytes > kMaxBlobBytes) {
        return false;
    }

    summary.lastPlayed = i64(getU64(header + 24));
    summary.randomSeed = i64(getU64(header + 32));
    summary.chunkCount = getU32(header + 40);
    summary.regionCount = getU32(header + 44);
    summary.unpackedBytes = getU64(header + 48);

    blob_.assign(bytes.begin() + blobOffset, bytes.begin() + blobOffset + usize(blobBytes));

    usize cursor = kHeaderBytes;
    entries_.reserve(entryCount);
    for (u32 i = 0; i < entryCount; ++i) {
        if (cursor + 2 > blobOffset) {
            return false;
        }
        const u16 pathLength = getU16(bytes.data() + cursor);
        cursor += 2;
        if (cursor + pathLength + 17 > blobOffset) {
            return false;
        }
        ManifestEntry entry;
        entry.path.assign(reinterpret_cast<const char*>(bytes.data() + cursor), pathLength);
        cursor += pathLength;
        entry.isDirectory = bytes[cursor] != 0;
        cursor += 1;
        entry.byteLength = getU32(bytes.data() + cursor);
        cursor += 4;
        entry.crc = getU32(bytes.data() + cursor);
        cursor += 4;
        entry.blobOffset = getU64(bytes.data() + cursor);
        cursor += 8;

        if (!entry.isDirectory
            && entry.blobOffset + entry.byteLength > blob_.size()) {
            return false;
        }
        entries_.push_back(std::move(entry));
    }
    return true;
}

}  // namespace mc::world::format
