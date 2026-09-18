#pragma once

// The console's half of multiplayer: its socket service, and the name it logs
// in with.
//
// Everything else about a session is core -- see core/net/client_session.hpp --
// because the BSD socket calls it makes are the same on both targets. What is
// not the same is that a 3DS application gets those calls from the SOC service,
// which wants a page-aligned buffer of its own before the first `socket()`, and
// that the owner's name is the friend list's rather than a launcher's.

#include "core/util/types.hpp"

#include <string>

namespace mc::ctr {

// Brings the SOC service up the first time it is asked, and leaves it up for
// the process. False with `*error` in words a player can act on.
bool startNetwork(std::string* error);

// At process exit.
void stopNetwork();

// Whether the console has a network address. Only worth asking when something
// has already failed -- see the note in network.cpp.
bool haveAddress();

// **This console's own address**, in host byte order, or 0 when it has none
// yet. `gethostid()`, which on a 3DS is the console's IP and on a PC is not an
// address at all -- which is exactly why this is here and not in core.
//
// A datagram socket has to bind *this* rather than `INADDR_ANY`: `SOCU:Bind`
// refuses the wildcard, and libctru's own sockets example binds `gethostid()`
// for that reason.
u32 localAddress();

// The screen name in the console's friend list, made into something a1.1.2
// accepts as a player name -- see `net::usernameFrom` -- and "Player" when the
// friend service cannot say.
std::string loginName();

// The friend-list name as it actually is, unmapped, for AlphaComputer's
// `platform_name` -- which is what a console is called there until it is linked
// to an account. Empty when the friend service cannot say, which is ordinary:
// `FRD_GetMyScreenName` fails outright on a console whose friend account was
// never set up. The server has its own answer for that (`Player#A1B2`), so an
// empty string is reported as absent rather than papered over here.
std::string friendScreenName();

// **This console's number**, which is the identity it plays under online.
//
// The friend list's principal ID when there is one. When there is not -- and
// there often is not -- the device ID, which is a different number about the
// same console: it is namespaced the same way, it is stable, and it is what
// keeps a console without a friend account from being unable to play at all.
// Zero when neither service answers, which is the one case with no identity.
//
// **It is an identifier and not a credential**, whichever of the two it came
// from. See core/net/ac_identity.hpp for what is done about that.
u32 principalId();

// `PS_GenerateRandomBytes`, in the shape core/net/ac_identity.hpp asks for.
// Called once in a console's life, when its key is made.
bool randomBytes(void* context, u8* out, usize size);

}  // namespace mc::ctr
