#pragma once

// The console's half of multiplayer: its socket service, and the name it logs
// in with.
//
// Everything else about a session is core -- see core/net/client_session.hpp --
// because the BSD socket calls it makes are the same on both targets. What is
// not the same is that a 3DS application gets those calls from the SOC service,
// which wants a page-aligned buffer of its own before the first `socket()`, and
// that the owner's name is the friend list's rather than a launcher's.

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

// The screen name in the console's friend list, made into something a1.1.2
// accepts as a player name -- see `net::usernameFrom` -- and "Player" when the
// friend service cannot say.
std::string loginName();

}  // namespace mc::ctr
