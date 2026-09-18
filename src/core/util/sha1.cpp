#include "core/util/sha1.hpp"

#include <cstring>
#include <vector>

namespace mc::util {

namespace {

inline u32 rotl(u32 x, int n)
{
    return (x << n) | (x >> (32 - n));
}

}  // namespace

void sha1(const u8* data, usize size, u8 digest[kSha1DigestSize])
{
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    // The padded message, built once. Everything this hashes is a handful of
    // bytes, so there is nothing to be gained by streaming it.
    std::vector<u8> message(data, data + size);
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    const u64 bits = u64(size) * 8;
    for (int i = 7; i >= 0; --i) {
        message.push_back(u8(bits >> (8 * i)));
    }

    for (usize offset = 0; offset < message.size(); offset += 64) {
        const u8* block = message.data() + offset;
        u32 w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16)
                   | (u32(block[i * 4 + 2]) << 8) | u32(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h[0];
        u32 b = h[1];
        u32 c = h[2];
        u32 d = h[3];
        u32 e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f = 0;
            u32 k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const u32 t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = u8(h[i] >> 24);
        digest[i * 4 + 1] = u8(h[i] >> 16);
        digest[i * 4 + 2] = u8(h[i] >> 8);
        digest[i * 4 + 3] = u8(h[i]);
    }
}

}  // namespace mc::util
