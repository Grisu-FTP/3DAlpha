#pragma once

// Who this console is to AlphaComputer, and the one secret it keeps.
//
// **A friend code is an identifier, not a credential.** This was checked
// against libctru rather than assumed: `FRD_PrincipalIdToFriendCode` and
// `FRD_FriendCodeToPrincipalId` are a pure reversible pair, so anyone can mint
// a valid-looking code for any principal ID, and `PS_SignRsaSha256` signs with
// a key the *caller* hands it, so it attests to nothing. On a console running
// our own homebrew there is no attestation to be had at all.
//
// So identity is **staked, not proved**. The first console to claim an identity
// binds an Ed25519 public key to it; every login afterwards signs a nonce the
// server picked, and a console offering a different key is refused. The private
// half is 32 bytes on the card and never leaves it. The residual risk -- somebody
// squatting an identity whose owner has never connected -- is real, bounded and
// written down in AlphaComputer's docs/identity.md rather than papered over.
//
// **The key file is the account.** Delete it and this console is a stranger
// again; copy it to another console and that console is this player. Both of
// those are consequences worth knowing rather than bugs.

#include "core/io/file_system.hpp"
#include "core/util/ed25519.hpp"
#include "core/util/types.hpp"

#include <string>

namespace mc::net::ac {

// 32 raw bytes and nothing else: no header, no version, no checksum. A file
// whose whole contents are the secret is one a player can back up, move or
// destroy without being told how, and a format with a header is a format with a
// migration.
inline constexpr char kIdentityKeyPath[] = "sdmc:/alpha/identity.key";

// The check byte inside a 3DS friend code: `sha1(principalId as little-endian
// u32)[0] >> 1`. See core/util/sha1.hpp for why this is here and what it is
// worth, which is that it catches a typo and not a liar.
u8 friendCodeChecksum(u32 principalId);

// The 12-digit friend code for a principal ID, as
// `FRD_PrincipalIdToFriendCode` builds one: `(checksum << 32) | principalId`.
u64 friendCodeFor(u32 principalId);

// `3ds:000123456789`, the canonical form the server stores and the wire
// carries. Empty for principal ID 0, which is not a real account and which the
// server refuses -- so a console that could not answer is not given a
// half-formed identity to argue about.
std::string identityFor(u32 principalId);

// This console's key, read from the card or made and written there.
//
// `randomBytes` is the platform's entropy: `PS_GenerateRandomBytes` on a 3DS.
// It is asked for only when there is no file, so a console that already has an
// identity never needs it -- which matters because it is the one part of this
// that cannot be tested against a known answer.
using RandomFn = bool (*)(void* context, u8* out, usize size);

struct Identity {
    std::string name;  // `3ds:000123456789`
    u8 seed[util::kEd25519SeedSize] = {};
    u8 publicKey[util::kEd25519PublicKeySize] = {};

    // True when the key was made on this call rather than read. The screen says
    // so once: a new key means a new claim, and a claim can fail.
    bool created = false;
};

// False when there is no usable identity: principal ID 0, a key file that is
// the wrong length, or a card that could not be written to. `*error` is in
// words a player can act on.
bool loadOrCreateIdentity(io::FileSystem& fs, const char* keyPath, u32 principalId,
                          RandomFn random, void* randomContext, Identity* out,
                          std::string* error);

// ---------------------------------------------------------------------------

// The server URL as the Profile screen holds it, and the address that comes out
// of it.
//
// **A URL for a UDP port that is not in it.** What a player is given is the
// website -- `https://ac.grisu-ftp.de` -- because that is the thing they will
// be typing a link code into, and it is the only form of the address anybody
// publishes. The control plane is a separate UDP port on the same host, so the
// host name is what is taken from the URL and `kDefaultPort` is what is used
// unless the URL names one.
inline constexpr char kDefaultServerUrl[] = "https://ac.grisu-ftp.de";

struct ServerAddress {
    std::string host;
    u16 port = 0;
};

// Pulls the host and port out of a URL, or out of a bare `host` or `host:port`.
// A scheme, a path, a query and a fragment are all ignored; a bracketed IPv6
// literal is understood. False when there is no host left after that.
bool parseServerUrl(const std::string& url, ServerAddress* out);

}  // namespace mc::net::ac
