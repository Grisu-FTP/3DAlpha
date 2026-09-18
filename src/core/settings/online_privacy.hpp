#pragma once

// **Who may reach a world this console opens on the internet.** Three answers,
// asked once, before the world is even chosen -- because the answer decides
// what the rendezvous server is told when the session is registered, and that
// happens before anybody can join.
//
// It is not the same question local wireless asks. A session in a room is
// bounded by the room: whoever can hear the beacon is already somebody the
// player could see. A session on the internet is reachable by anyone who is
// told how to reach it, so the host says how far that goes.
//
// **All three are invitations, not open doors.** Every one of them registers
// the session *unlisted* -- see `onlinePrivacyLocked` -- so a world on
// somebody's console is never a public server that strangers walk into. What
// differs is who is handed the way in.
//
// It is core rather than menu code for the same reason `control_scheme.hpp`
// is: it is written to a file, it is a rule rather than a drawing, and a host
// can check the words it round-trips through.

#include "core/util/types.hpp"

#include <string_view>

namespace mc::settings {

enum class OnlinePrivacy : u8 {
    // The six characters the host reads out are the only way in. Works today
    // and needs nothing of the server but a session to register.
    CodeOnly = 0,
    // Only consoles the host is friends with, and no code at all.
    FriendsOnly,
    // Friends walk in from their list; anybody else needs the code.
    FriendsAndCode,
};

inline constexpr int kOnlinePrivacyCount = 3;

// **The two friend answers need a friend list, and a console cannot see one.**
// AlphaComputer stores friendships -- they are made on its website -- but
// protocol 2 has no message that asks for them and no join it refuses on their
// account, so a session registered as friends-only would be a session nobody
// could enter. The rows are offered and refused rather than hidden, which is
// what says the answer is "not yet" instead of "never". See docs/online-play.md.
bool onlinePrivacyNeedsFriendList(OnlinePrivacy privacy);

// Whether the session is kept out of the server's public browser. True for all
// three, and a function rather than a constant because it is the wire's
// `locked` flag and the reason it is what it is belongs next to the answer.
bool onlinePrivacyLocked(OnlinePrivacy privacy);

// The word written to `3ds.ini`. A word rather than an ordinal, the choice
// `controlSchemeToken` makes and for the same reason: a file written by a
// later build that adds an answer is readable here instead of being a number
// that silently means a different one.
const char* onlinePrivacyToken(OnlinePrivacy privacy);

// False when the word is not one of the three; the caller keeps its default.
bool onlinePrivacyFromToken(std::string_view token, OnlinePrivacy* out);

// What the row draws, and the line under it.
const char* onlinePrivacyLabel(OnlinePrivacy privacy);
const char* onlinePrivacyNote(OnlinePrivacy privacy);

}  // namespace mc::settings
