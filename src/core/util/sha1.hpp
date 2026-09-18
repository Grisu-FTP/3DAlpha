#pragma once

// SHA-1, for the one thing in this build that needs it: the check byte inside a
// 3DS friend code.
//
// A friend code is a 32-bit principal ID and a checksum, and the checksum is
// `sha1(principal_id as little-endian u32)[0] >> 1`. Reproducing it here means
// the console can build its own identity string from a principal ID alone,
// without a second service call, and means the format can be tested on the host
// against AlphaComputer's `friend_code_for`.
//
// **It is a checksum and not a signature.** It rejects a mistyped code and
// nothing else; anyone can compute a valid-looking friend code for any
// principal ID, which is exactly why the identity is staked with an Ed25519 key
// instead. See core/util/ed25519.hpp and AlphaComputer's docs/identity.md.
//
// Nothing else in this tree hashes with SHA-1, and nothing should: it is here
// as a format, not as a security primitive.

#include "core/util/types.hpp"

namespace mc::util {

inline constexpr usize kSha1DigestSize = 20;

void sha1(const u8* data, usize size, u8 digest[kSha1DigestSize]);

}  // namespace mc::util
