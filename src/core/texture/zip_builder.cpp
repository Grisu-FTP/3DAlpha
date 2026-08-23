#include "core/texture/zip_builder.hpp"

#include "core/texture/zip_archive.hpp"

#include <zlib.h>

namespace mc::texture {

namespace {

constexpr u32 kSigCentral = 0x02014B50;
constexpr u32 kSigLocal = 0x04034B50;
constexpr u32 kSigEnd = 0x06054B50;

// 2.0 is what "deflate is used somewhere in here" means, and it is what every
// tool writes for an ordinary archive.
constexpr u16 kVersionMadeBy = 20;
constexpr u16 kVersionNeeded = 20;

// The offsets a zip stores are 32-bit, so an archive has to stay under 4 GB.
// This is a long way below that and still far above any texture pack: a jar is
// under a megabyte.
constexpr usize kMaxArchive = 512u << 20;

void put16(std::vector<u8>& out, u16 value)
{
    out.push_back(u8(value & 0xFF));
    out.push_back(u8((value >> 8) & 0xFF));
}

void put32(std::vector<u8>& out, u32 value)
{
    out.push_back(u8(value & 0xFF));
    out.push_back(u8((value >> 8) & 0xFF));
    out.push_back(u8((value >> 16) & 0xFF));
    out.push_back(u8((value >> 24) & 0xFF));
}

}  // namespace

bool ZipBuilder::addRaw(std::string_view name, u16 method, u32 crc, u32 uncompressedSize,
                        ConstByteSpan compressed)
{
    if (finished_ || !isSafeZipName(name) || name.size() > 0xFFFF) {
        return false;
    }
    if (out_.size() + compressed.size() + name.size() + 30 > kMaxArchive) {
        return false;
    }
    // The end record counts entries in 16 bits, so an archive cannot hold more
    // than this without a Zip64 record we do not write. A jar holds 538.
    if (records_.size() >= 0xFFFF) {
        return false;
    }

    Record record;
    record.name.assign(name);
    record.method = method;
    record.crc = crc;
    record.compressedSize = u32(compressed.size());
    record.uncompressedSize = uncompressedSize;
    record.localHeaderOffset = u32(out_.size());

    put32(out_, kSigLocal);
    put16(out_, kVersionNeeded);
    // **Flags zero, and specifically bit 3 clear.** See the header: the sizes
    // below are real, so there is no trailing data descriptor to announce.
    put16(out_, 0);
    put16(out_, method);
    // Modification time and date. Zero is 1980-01-01 00:00, which is what the
    // format's epoch is; a console whose clock a player has never set would
    // otherwise stamp every pack with a date from 2000.
    put16(out_, 0);
    put16(out_, 0);
    put32(out_, crc);
    put32(out_, record.compressedSize);
    put32(out_, uncompressedSize);
    put16(out_, u16(name.size()));
    put16(out_, 0);  // no extra field
    out_.insert(out_.end(), name.begin(), name.end());
    out_.insert(out_.end(), compressed.begin(), compressed.end());

    records_.push_back(std::move(record));
    return true;
}

bool ZipBuilder::addStored(std::string_view name, ConstByteSpan data)
{
    const u32 crc =
        u32(::crc32(::crc32(0, nullptr, 0), data.data(), uInt(data.size())));
    return addRaw(name, 0, crc, u32(data.size()), data);
}

void ZipBuilder::finish()
{
    if (finished_) {
        return;
    }
    finished_ = true;

    const u32 directoryOffset = u32(out_.size());

    for (const Record& record : records_) {
        put32(out_, kSigCentral);
        put16(out_, kVersionMadeBy);
        put16(out_, kVersionNeeded);
        put16(out_, 0);  // flags -- matching the local header, bit 3 clear
        put16(out_, record.method);
        put16(out_, 0);  // time
        put16(out_, 0);  // date
        put32(out_, record.crc);
        put32(out_, record.compressedSize);
        put32(out_, record.uncompressedSize);
        put16(out_, u16(record.name.size()));
        put16(out_, 0);  // extra field
        put16(out_, 0);  // comment
        put16(out_, 0);  // disk number
        put16(out_, 0);  // internal attributes
        put32(out_, 0);  // external attributes
        put32(out_, record.localHeaderOffset);
        out_.insert(out_.end(), record.name.begin(), record.name.end());
    }

    const u32 directorySize = u32(out_.size()) - directoryOffset;
    const u16 count = u16(records_.size());

    put32(out_, kSigEnd);
    put16(out_, 0);  // this disk
    put16(out_, 0);  // disk the directory starts on
    put16(out_, count);
    put16(out_, count);
    put32(out_, directorySize);
    put32(out_, directoryOffset);
    put16(out_, 0);  // no archive comment
}

}  // namespace mc::texture
