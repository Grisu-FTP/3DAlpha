#pragma once

// Building PNGs and zips in memory, so the texture tests need no binary
// fixtures.
//
// Checked-in fixtures would be opaque: a decoder test that fails against a blob
// tells you nothing about which of the five scanline filters broke. Encoding
// here means every test states the exact bytes it is about to decode, and the
// encoder is small because it only has to produce what the decoder claims to
// accept.
//
// The deflate side comes from core/util/compress.hpp, the same wrappers the
// decoder uses in the other direction.

#include "core/util/compress.hpp"
#include "core/util/types.hpp"

#include <zlib.h>

#include <string>
#include <vector>

namespace mc::test {

inline void putBe32(std::vector<u8>& out, u32 value)
{
    out.push_back(u8(value >> 24));
    out.push_back(u8((value >> 16) & 0xFF));
    out.push_back(u8((value >> 8) & 0xFF));
    out.push_back(u8(value & 0xFF));
}

inline void putLe16(std::vector<u8>& out, u16 value)
{
    out.push_back(u8(value & 0xFF));
    out.push_back(u8(value >> 8));
}

inline void putLe32(std::vector<u8>& out, u32 value)
{
    out.push_back(u8(value & 0xFF));
    out.push_back(u8((value >> 8) & 0xFF));
    out.push_back(u8((value >> 16) & 0xFF));
    out.push_back(u8((value >> 24) & 0xFF));
}

inline u32 crcOf(const u8* data, usize size)
{
    return u32(::crc32(::crc32(0, nullptr, 0), data, uInt(size)));
}

// One PNG chunk: length, type, body, CRC over type and body.
inline void putChunk(std::vector<u8>& out, const char type[5], const std::vector<u8>& body)
{
    putBe32(out, u32(body.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    putBe32(out, crcOf(out.data() + start, out.size() - start));
}

// A PNG built from already-filtered scanlines: `filtered` is one filter byte
// followed by `width * channels` bytes, per row. Passing the filter bytes in is
// the point -- it is how a test exercises Paeth rather than hoping an encoder
// picked it.
//
// `splitIdat` cuts the zlib stream into two IDAT chunks, which is what any real
// encoder does above a few kilobytes and what a decoder that inflates chunks
// individually gets wrong.
inline std::vector<u8> makePng(int width, int height, u8 colourType,
                               const std::vector<u8>& filtered,
                               const std::vector<u8>& palette = {},
                               const std::vector<u8>& trns = {}, bool splitIdat = false)
{
    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    std::vector<u8> ihdr;
    putBe32(ihdr, u32(width));
    putBe32(ihdr, u32(height));
    ihdr.push_back(8);           // bit depth
    ihdr.push_back(colourType);
    ihdr.push_back(0);           // compression
    ihdr.push_back(0);           // filter method
    ihdr.push_back(0);           // interlace
    putChunk(png, "IHDR", ihdr);

    if (!palette.empty()) {
        putChunk(png, "PLTE", palette);
    }
    if (!trns.empty()) {
        putChunk(png, "tRNS", trns);
    }

    std::vector<u8> stream;
    zip::compress(filtered, stream, zip::Wrapper::Zlib);

    if (splitIdat && stream.size() > 4) {
        const usize half = stream.size() / 2;
        putChunk(png, "IDAT", std::vector<u8>(stream.begin(), stream.begin() + long(half)));
        putChunk(png, "IDAT", std::vector<u8>(stream.begin() + long(half), stream.end()));
    } else {
        putChunk(png, "IDAT", stream);
    }

    putChunk(png, "IEND", {});
    return png;
}

// An unfiltered RGBA PNG from raw pixels, which is what most tests want.
inline std::vector<u8> makeRgbaPng(int width, int height, const std::vector<u8>& rgba)
{
    std::vector<u8> filtered;
    filtered.reserve(rgba.size() + usize(height));
    for (int y = 0; y < height; ++y) {
        filtered.push_back(0);  // filter None
        const usize offset = usize(y) * usize(width) * 4;
        filtered.insert(filtered.end(), rgba.begin() + long(offset),
                        rgba.begin() + long(offset) + long(usize(width) * 4));
    }
    return makePng(width, height, 6, filtered);
}

// A zip built one entry at a time, with control over the things a real jar
// varies: the compression method, and whether the local header is written with
// a data descriptor (flag bit 3) and therefore lies about the sizes.
class TestZip {
public:
    void add(const std::string& name, const std::vector<u8>& content, bool deflate,
             bool dataDescriptor)
    {
        std::vector<u8> stored = content;
        u16 method = 0;
        if (deflate) {
            stored.clear();
            zip::compress(content, stored, zip::Wrapper::Raw);
            method = 8;
        }
        const u32 crc = crcOf(content.data(), content.size());

        Record record;
        record.name = name;
        record.method = method;
        record.crc = crc;
        record.compressed = u32(stored.size());
        record.uncompressed = u32(content.size());
        record.offset = u32(bytes_.size());
        record.flags = dataDescriptor ? u16(0x0008) : u16(0);

        putLe32(bytes_, 0x04034B50);
        putLe16(bytes_, 20);
        putLe16(bytes_, record.flags);
        putLe16(bytes_, method);
        putLe16(bytes_, 0);
        putLe16(bytes_, 0);
        // **The whole point of the flag.** With bit 3 set these three fields
        // are zero in the local header and the truth trails the data, which is
        // how 92 % of a real client jar is written.
        putLe32(bytes_, dataDescriptor ? 0u : crc);
        putLe32(bytes_, dataDescriptor ? 0u : record.compressed);
        putLe32(bytes_, dataDescriptor ? 0u : record.uncompressed);
        putLe16(bytes_, u16(name.size()));
        putLe16(bytes_, 0);
        bytes_.insert(bytes_.end(), name.begin(), name.end());
        bytes_.insert(bytes_.end(), stored.begin(), stored.end());

        if (dataDescriptor) {
            putLe32(bytes_, 0x08074B50);
            putLe32(bytes_, crc);
            putLe32(bytes_, record.compressed);
            putLe32(bytes_, record.uncompressed);
        }
        records_.push_back(std::move(record));
    }

    std::vector<u8> finish() const
    {
        std::vector<u8> out = bytes_;
        const u32 directoryOffset = u32(out.size());
        for (const Record& record : records_) {
            putLe32(out, 0x02014B50);
            putLe16(out, 20);
            putLe16(out, 20);
            putLe16(out, record.flags);
            putLe16(out, record.method);
            putLe16(out, 0);
            putLe16(out, 0);
            putLe32(out, record.crc);
            putLe32(out, record.compressed);
            putLe32(out, record.uncompressed);
            putLe16(out, u16(record.name.size()));
            putLe16(out, 0);
            putLe16(out, 0);
            putLe16(out, 0);
            putLe16(out, 0);
            putLe32(out, 0);
            putLe32(out, record.offset);
            out.insert(out.end(), record.name.begin(), record.name.end());
        }
        const u32 directorySize = u32(out.size()) - directoryOffset;
        putLe32(out, 0x06054B50);
        putLe16(out, 0);
        putLe16(out, 0);
        putLe16(out, u16(records_.size()));
        putLe16(out, u16(records_.size()));
        putLe32(out, directorySize);
        putLe32(out, directoryOffset);
        putLe16(out, 0);
        return out;
    }

private:
    struct Record {
        std::string name;
        u16 method;
        u16 flags;
        u32 crc;
        u32 compressed;
        u32 uncompressed;
        u32 offset;
    };

    std::vector<u8> bytes_;
    std::vector<Record> records_;
};

// A terrain.png of the given edge whose every texel encodes which tile it is
// in, so a rescaled atlas can be checked tile by tile rather than merely run.
// Tile (tx, ty) is filled with red = tx * 16, green = ty * 16, blue = 200,
// alpha 255 -- flat within a tile, so any correct scaling reproduces it exactly.
inline std::vector<u8> makeTerrainPng(int edge)
{
    const int tilePixels = edge / 16;
    std::vector<u8> rgba(usize(edge) * usize(edge) * 4);
    for (int y = 0; y < edge; ++y) {
        for (int x = 0; x < edge; ++x) {
            u8* p = rgba.data() + (usize(y) * usize(edge) + usize(x)) * 4;
            p[0] = u8((x / tilePixels) * 16);
            p[1] = u8((y / tilePixels) * 16);
            p[2] = 200;
            p[3] = 255;
        }
    }
    return makeRgbaPng(edge, edge, rgba);
}

}  // namespace mc::test
