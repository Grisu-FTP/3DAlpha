#pragma once

// Deflate wrappers.
//
// Two callers, two framings: Alpha chunk files and level.dat are gzip-wrapped
// (RFC 1952), while Map Chunk packet payloads are zlib-wrapped (RFC 1950).
// Raw deflate is here because packed-world containers store sectors without a
// redundant wrapper -- the container already records the length and checksum.
//
// Everything is whole-buffer: we always hold the complete stream before
// decoding, never a partial one. That is what makes libdeflate a candidate
// replacement for zlib later (see docs/3ds-performance.md) -- it is faster on
// ARM precisely for this pattern, and it only implements this pattern.
//
// Decompression never runs on core0. See docs/architecture.md.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::zip {

enum class Wrapper {
    Gzip,  // chunk files, level.dat
    Zlib,  // Map Chunk (0x33) payloads
    Raw,   // packed-world sectors
};

// A hostile or corrupt stream can claim to expand without bound, so decode
// stops once the output would exceed maxOutput and reports failure. Callers
// know their own ceiling: an Alpha chunk column is 80 KB plus entities.
inline constexpr usize kDefaultMaxOutput = 8u << 20;

// Appends to out (does not clear it), so several streams can be concatenated
// into one buffer. Returns false on malformed input or on exceeding maxOutput.
bool decompress(ConstByteSpan in, std::vector<u8>& out, Wrapper wrapper,
                usize maxOutput = kDefaultMaxOutput);

// Level 6 matches what the Java client wrote, which keeps file sizes in the
// same range as the original game's for a given world.
bool compress(ConstByteSpan in, std::vector<u8>& out, Wrapper wrapper, int level = 6);

}  // namespace mc::zip
