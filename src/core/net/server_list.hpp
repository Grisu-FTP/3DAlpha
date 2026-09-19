#pragma once

// The multiplayer server list on the card, the address a player types, and the
// name the client logs in with.
//
// a1.1.2 has neither a list nor a name to choose: its Multiplayer screen
// (`gc`) is one text field whose last value goes to `options.txt` as
// `lastServer`, and the name is whatever the launcher passed. A console has no
// launcher, and retyping an address on a touch keyboard every time is the one
// thing a list exists to spare, so both of these are ours.
//
// The list is a plain text file beside 3ds.ini, one `name=` line opening each
// entry and an `address=` line under it, so a player can write one on a PC.
// A `port=` line under that is optional and defaults to 25565, and an
// `address=` that still carries `host:port` -- which is what every list written
// before the port had a row of its own looks like -- is split on the way in.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::net {

inline constexpr char kServerListPath[] = "sdmc:/3dalpha/servers.txt";
inline constexpr u16 kDefaultPort = 25565;

// A friend-list screen name is ten UTF-16 units; this is the ceiling a name
// is cut to whatever its source.
inline constexpr int kMaxUsernameChars = 16;

// **The ceiling for a name AlphaComputer gave out**, which is its own cap on a
// display name. Only 3DAlpha consoles ever see one: it is what an internet
// session plays under, and what a host lets a guest's Hello carry.
inline constexpr int kMaxDisplayNameChars = 24;

// What the client calls itself when there is no usable name at all.
inline constexpr char kFallbackUsername[] = "Player";

struct ServerEntry {
    std::string name;

    // **The host alone**, with no `:port` on the end. It may still be typed
    // with one -- `loadServerList` and the Add Server screen both split that
    // off into `port` -- because a player reading an address off a forum post
    // has it in one piece and should not have to take it apart.
    std::string address;

    // 25565 unless the file or the player says otherwise, which is what a
    // server run with no `server-port` set listens on.
    u16 port = kDefaultPort;
};

// False when the file is missing or unreadable, which leaves `*out` empty --
// an absent list is the ordinary first-run case, not an error.
bool loadServerList(io::FileSystem& fs, const char* path, std::vector<ServerEntry>* out);
bool saveServerList(io::FileSystem& fs, const char* path, const std::vector<ServerEntry>& list);

// `host`, `host:port`, `[v6-literal]` or `[v6-literal]:port`. A bare IPv6
// literal -- more than one colon and no brackets -- is taken as a host with
// the default port. False for an empty host or a port outside 1..65535.
//
// `*port` is left at `fallback` when the address carries none, so a caller
// that already has a port of its own -- an entry's `port` field -- keeps it
// instead of being reset to 25565 by an address that said nothing about it.
bool parseAddress(std::string_view address, std::string* host, u16* port,
                  u16 fallback = kDefaultPort);

// 1..65535 as a `u16`, for the Port row's keyboard. False leaves `*out` alone,
// which is how a typed port that is not one keeps the row on what it had.
bool parsePort(std::string_view text, u16* out);

// A UTF-16 name, as FRD hands one over, as UTF-8. Stops at the first zero unit.
std::string utf16ToUtf8(const u16* units, usize maxUnits);

// The name to log in with, made from whatever the console calls its owner.
//
// Kept as close to that as the game allows: characters the a1.1.2 font cannot
// draw become '_', and so does anything that would break the server's side of
// a name -- a space splits it in two for every console command that takes one
// (`op`, `kick`, `tp`), and a path separator or a colon puts it outside the
// players folder it is saved under. A name with nothing left in it but
// underscores is no name, and falls back to `kFallbackUsername`. Cut to
// `maxChars` characters.
std::string usernameFrom(std::string_view utf8, int maxChars = kMaxUsernameChars);

}  // namespace mc::net
