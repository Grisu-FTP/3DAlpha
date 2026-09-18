#pragma once

// Ed25519 signatures, RFC 8032, enough of them to prove this console is the
// one that claimed its identity.
//
// **Why the console has a key at all.** A 3DS friend code is an identifier and
// not a credential: `FRD_PrincipalIdToFriendCode` and its inverse are a pure
// reversible pair, so anyone can mint a valid-looking code for any principal
// ID, and `PS_SignRsaSha256` signs with a key the *caller* supplies, so it
// attests to nothing. There is no attestation to be had on a modded console.
// So identity is staked rather than proved: the first console to claim an
// identity binds a public key to it, and every login afterwards signs a nonce
// the server picked. See AlphaComputer's docs/identity.md, which is where that
// decision is written down and argued.
//
// **Signing only.** Verification happens on the server, which has a real crypto
// library; nothing on the console ever checks a signature, so the code for it
// is not here. What is here is key generation from a seed, the public key, and
// a signature -- about four hundred lines rather than a dependency this tree
// would otherwise not have.
//
// **Implemented from the specification, not vendored.** There is no Ed25519 in
// libctru, in zlib or in anything else this build already links, and the
// licensing rules in CONTRIBUTING.md make copying somebody else's arithmetic
// the more expensive option rather than the cheaper one. The field is 2^255-19
// in eight 32-bit limbs with 64-bit products -- an ARM11 has no 128-bit
// integer, which rules out the usual radix-2^51 form -- and the curve
// arithmetic is the unified extended-coordinate addition law, used for
// doubling as well, because completeness is worth more here than the few
// microseconds a separate doubling would save.
//
// **Nothing here is constant-time.** A signature is produced on a console that
// nobody else is running code on, against a server that sees only the result.
// Timing is not in the threat model; the residual risks that are, are in
// docs/identity.md.

#include "core/util/types.hpp"

namespace mc::util {

inline constexpr usize kEd25519SeedSize = 32;
inline constexpr usize kEd25519PublicKeySize = 32;
inline constexpr usize kEd25519SignatureSize = 64;

// The public key for a seed. The seed is the private key: 32 bytes of real
// randomness, written once and kept on the card.
void ed25519PublicKey(const u8 seed[kEd25519SeedSize], u8 publicKey[kEd25519PublicKeySize]);

// RFC 8032's Ed25519 (not ph, not ctx): the message is hashed whole.
void ed25519Sign(const u8 seed[kEd25519SeedSize], const u8 publicKey[kEd25519PublicKeySize],
                 const u8* message, usize size, u8 signature[kEd25519SignatureSize]);

}  // namespace mc::util
