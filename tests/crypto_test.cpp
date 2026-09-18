// SHA-512 and Ed25519 against answers from somewhere else.
//
// **The point of this file is that nothing in it was produced by the code it
// checks.** The digests are OpenSSL's, printed by `openssl dgst -sha512`; the
// signatures are OpenSSL's too, in tests/ed25519_vectors.hpp, over RFC 8032's
// own seeds and over the exact payload AlphaComputer asks a console to sign.
// An implementation written from a specification and checked against itself
// proves only that it is consistent.

#include "core/util/ed25519.hpp"
#include "core/util/sha512.hpp"
#include "ed25519_vectors.hpp"
#include "framework.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::util;

namespace {

std::string hex(const u8* bytes, usize size)
{
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (usize i = 0; i < size; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0xf]);
    }
    return out;
}

std::string digestOf(const std::string& text)
{
    u8 digest[kSha512DigestSize];
    sha512(reinterpret_cast<const u8*>(text.data()), text.size(), digest);
    return hex(digest, sizeof(digest));
}

}  // namespace

TEST(sha512_matches_openssl_for_the_usual_three)
{
    CHECK_EQ(digestOf(""),
             std::string("cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                         "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    CHECK_EQ(digestOf("abc"),
             std::string("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                         "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    // 56 bytes: one byte short of needing a second block for the padding, which
    // is the case a hand-written `finish` gets wrong.
    CHECK_EQ(digestOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
                         "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445"));
}

// A million bytes fed in awkward pieces: the buffering, not the compression.
TEST(sha512_is_the_same_however_the_message_is_split)
{
    Sha512 sha;
    const std::vector<u8> block(1000, u8('a'));
    for (int i = 0; i < 1000; ++i) {
        sha.update(block.data(), block.size());
    }
    u8 digest[kSha512DigestSize];
    sha.finish(digest);
    CHECK_EQ(hex(digest, sizeof(digest)),
             std::string("e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                         "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"));

    Sha512 awkward;
    for (usize i = 0; i < 1000000; i += 7) {
        const usize take = 1000000 - i < 7 ? 1000000 - i : 7;
        const std::vector<u8> piece(take, u8('a'));
        awkward.update(piece.data(), piece.size());
    }
    u8 second[kSha512DigestSize];
    awkward.finish(second);
    CHECK(std::memcmp(digest, second, sizeof(digest)) == 0);
}

TEST(a_seed_gives_the_public_key_openssl_derives_from_it)
{
    for (const test::Ed25519Vector& vector : test::kEd25519Vectors) {
        u8 publicKey[kEd25519PublicKeySize];
        ed25519PublicKey(vector.seed, publicKey);
        CHECK_EQ(hex(publicKey, sizeof(publicKey)),
                 hex(vector.publicKey, sizeof(vector.publicKey)));
    }
}

TEST(a_signature_is_the_one_openssl_produces_for_the_same_message)
{
    for (const test::Ed25519Vector& vector : test::kEd25519Vectors) {
        u8 signature[kEd25519SignatureSize];
        ed25519Sign(vector.seed, vector.publicKey, vector.message, vector.messageSize,
                    signature);
        CHECK_EQ(hex(signature, sizeof(signature)),
                 hex(vector.signature, sizeof(vector.signature)));
    }
}

// Ed25519 is deterministic by design -- the nonce comes from the key and the
// message, not from a random number generator. A console has nothing worth
// calling an entropy source at the moment it answers a challenge, so this is
// the property that makes signing safe there at all.
TEST(signing_the_same_message_twice_gives_the_same_signature)
{
    const test::Ed25519Vector& vector = test::kEd25519Vectors[0];
    u8 first[kEd25519SignatureSize];
    u8 second[kEd25519SignatureSize];
    ed25519Sign(vector.seed, vector.publicKey, vector.message, vector.messageSize, first);
    ed25519Sign(vector.seed, vector.publicKey, vector.message, vector.messageSize, second);
    CHECK(std::memcmp(first, second, sizeof(first)) == 0);
}

TEST(one_changed_byte_changes_the_whole_signature)
{
    const test::Ed25519Vector& vector = test::kEd25519Vectors[3];
    std::vector<u8> message(vector.message, vector.message + vector.messageSize);
    message[17] = u8(message[17] ^ 0x01);

    u8 signature[kEd25519SignatureSize];
    ed25519Sign(vector.seed, vector.publicKey, message.data(), message.size(), signature);
    CHECK(std::memcmp(signature, vector.signature, sizeof(signature)) != 0);
}
