#pragma once

// Writing a zip, for the jar importer's output.
//
// **There is no deflate here, on purpose.** The importer copies entries out of
// a jar exactly as they are stored -- already-compressed bytes, with the
// method, CRC and sizes the source's central directory recorded -- so a 900 KB
// jar becomes a pack without one inflate or deflate call. PNGs are already
// deflated inside their own container, so recompressing them would spend
// seconds of ARM11 time to save nothing. `addStored` exists for the rare thing
// we generate ourselves.
//
// The one place this does not merely copy is the local header. A source entry
// written with a data descriptor (general-purpose flag bit 3) has zeroed CRC
// and size fields in its local header, and 92 % of a real jar is written that
// way. We know the real values from the central directory, so the local headers
// written here carry them and **flag bit 3 is cleared**. Copying the flag
// across without also copying the trailing descriptor would produce an archive
// that lenient readers accept and strict ones reject.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::texture {

class ZipBuilder {
public:
    // Adds an entry whose bytes are already in the framing `method` names.
    // Everything but `name` comes from the source archive's central directory.
    // False if the name is unsafe or the archive would exceed 4 GB.
    bool addRaw(std::string_view name, u16 method, u32 crc, u32 uncompressedSize,
                ConstByteSpan compressed);

    // Adds an entry stored uncompressed. Computes the CRC itself.
    bool addStored(std::string_view name, ConstByteSpan data);

    // Appends the central directory and the end record. The builder is finished
    // afterwards and must not be added to again.
    void finish();

    const std::vector<u8>& bytes() const { return out_; }
    usize entryCount() const { return records_.size(); }

private:
    struct Record {
        std::string name;
        u16 method;
        u32 crc;
        u32 compressedSize;
        u32 uncompressedSize;
        u32 localHeaderOffset;
    };

    std::vector<u8> out_;
    std::vector<Record> records_;
    bool finished_ = false;
};

}  // namespace mc::texture
