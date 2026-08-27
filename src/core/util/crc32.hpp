#pragma once

// CRC-32, the one zlib and zip and gzip all mean by the name.
//
// A wrapper rather than a table of our own: zlib is already linked for the
// chunk files, its implementation is the one everything else on the card
// agrees with, and a second table would be 1 KB of .rodata to say the same
// thing slightly differently.
//
// **Where this is and is not used.** A packed region records a payload's CRC
// so a *conversion* can prove the bytes it wrote are the bytes it read,
// without inflating them. It is deliberately not checked on the gameplay read
// path: the payload is a gzip stream, and inflate already verifies gzip's own
// CRC-32 and length, so checking ours first would be a second pass over the
// same bytes for the same answer on a 268 MHz ARM11.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

namespace mc::util {

// The CRC-32 of `data` on its own.
u32 crc32(ConstByteSpan data);

// Continues a running CRC over another block, for a checksum taken across
// buffers that are never all in memory at once. Start from `crc32Seed()`.
u32 crc32Update(u32 crc, ConstByteSpan data);
u32 crc32Seed();

}  // namespace mc::util
