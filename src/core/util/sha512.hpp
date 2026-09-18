#pragma once

// SHA-512, FIPS 180-4, because Ed25519 is defined in terms of it.
//
// **Written from the specification rather than linked.** The only consumer is
// core/util/ed25519.hpp, which signs one challenge per login; a hash that costs
// a few microseconds a year does not justify pulling a crypto library onto a
// console that has none. zlib is linked for the chunk files and has no digest
// of this family, and libctru's `PS_` services hash with keys the caller
// supplies, which is a different thing entirely -- see the note in
// docs/identity.md about what the console can and cannot prove.
//
// The block loop is the specification's, unrolled no further than the round
// constant table. Nothing here is constant-time and nothing here needs to be:
// the input is a server's nonce and a public identity, neither of them secret.

#include "core/util/types.hpp"

namespace mc::util {

inline constexpr usize kSha512DigestSize = 64;
inline constexpr usize kSha512BlockSize = 128;

class Sha512 {
public:
    Sha512();

    void update(const u8* data, usize size);

    // Writes `kSha512DigestSize` bytes. The object is finished afterwards and
    // must not be updated again.
    void finish(u8* digest);

private:
    void compress(const u8* block);

    u64 state_[8];
    u8 buffer_[kSha512BlockSize];
    usize buffered_ = 0;
    // The message length in bits. A 128-bit field in the specification; the
    // high half is written as zero because nothing here hashes two exabytes.
    u64 bits_ = 0;
};

// The digest of one buffer, for the callers that have it all at once.
void sha512(const u8* data, usize size, u8* digest);

}  // namespace mc::util
