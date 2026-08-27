#pragma once

// `<world>/3dalpha.ini` -- the settings that belong to one world and that the
// Alpha level format has nowhere to put.
//
// **Why a file of our own, beside a world rather than inside it.** A setting
// like gamemode has two homes it must not have:
//
//   * Not `level.dat`. A key no version of a1.1.2 ever wrote would travel back
//     to a PC copy of the world, where the real game would carry it around
//     without meaning it. Round-tripping other people's tags verbatim is a
//     promise this project makes (see core/nbt/preserved.hpp); inventing tags
//     of our own is the other side of that promise.
//   * Not `3ds.ini`. That file is the console's, and a per-world setting kept
//     there would be one value shared by every world on the card.
//
// So it is a plain file in the world folder, which a real Minecraft client
// never reads: the Alpha loader looks for `level.dat` and the base36 chunk
// tree and enumerates nothing else. An unknown sibling file is inert -- it is
// not read, not rewritten and not deleted -- which is exactly the "does not
// interfere" this needs.
//
// **Absent is the normal case, not an error.** Every world that already exists
// has no such file, and so does every Alpha save copied in from a PC. Absent
// means defaults, and the file is written when something changes rather than
// on sight, so merely listing worlds never writes to the card.
//
// The format is the `key=value` shape `3ds.ini` uses, sharing its parser --
// see core/settings/ini.hpp. Saving rewrites the file from the keys below, so
// **a key this build does not know is dropped**.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>

namespace mc::settings {

// The file's name inside a world folder. The same in both storage formats: it
// is ours, so unlike a stray file from a third-party tool the packer does not
// have to stash it -- it stays a plain readable file on both sides, which is
// also what lets the world list read a gamemode without opening a container.
inline constexpr char kWorldSettingsName[] = "3dalpha.ini";

// **a1.1.2 has no gamemode at all** -- there is one way to play and the word is
// not in the client. These are the names a player coming from a later version
// expects to find, and only one of them means anything yet.
enum class Gamemode {
    Spectator,
    Survival,
    Creative,
};

struct WorldSettings {
    // Spectator, because it is the only one implemented and because it is what
    // the game already does: there is no player body yet, so movement is free
    // flight with no collision. Defaulting to Survival would be a label that
    // lies about the behaviour behind it.
    Gamemode gamemode = Gamemode::Spectator;
};

// The token written to the file. Stable across builds -- a word rather than an
// ordinal, so a file written by a later build that adds a mode is readable
// instead of being a number that silently means something else here.
const char* gamemodeToken(Gamemode mode);

// What the menu draws.
const char* gamemodeLabel(Gamemode mode);

// **False for everything but Spectator today.** Survival and Creative are
// offered and drawn disabled rather than hidden: a row that is missing reads as
// an oversight, one that is greyed out reads as a plan. They become selectable
// when M3 gives the player a body to collide with.
bool gamemodeImplemented(Gamemode mode);

// False when the token is not one of the three; the caller keeps its default.
bool gamemodeFromToken(std::string_view token, Gamemode* out);

// `<worldDir>/3dalpha.ini`.
std::string worldSettingsPath(std::string_view worldDir);

// False when there is no file, which is the ordinary state of every world that
// predates this feature; *out is left at its defaults either way. An
// unrecognised value is ignored rather than failing the load.
bool loadWorldSettings(io::FileSystem& fs, std::string_view worldDir, WorldSettings* out);

// Writes through writeFileAtomic, so a console switched off mid-save keeps the
// settings it had. Creates nothing but the file: the world folder is already
// there by the time anyone can change a setting in it.
bool saveWorldSettings(io::FileSystem& fs, std::string_view worldDir,
                       const WorldSettings& settings);

}  // namespace mc::settings
