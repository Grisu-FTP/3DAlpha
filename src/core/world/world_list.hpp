#pragma once

// The saves folder as a list, for the world-select screen.
//
// **Nothing here opens a world**, and that is the whole reason it exists.
// `Storage::open()` claims session.lock and `close()` rewrites level.dat, so a
// menu that opened every world to read its name would re-stamp LastPlayed on
// worlds the player only scrolled past, and would collide with a lock that
// something else holds. The listing reads level.dat and stops -- see
// `Storage::peekLevel`.
//
// It is core rather than platform code for the usual reason: the console cannot
// run a unit test, and every rule here (which directories count, what a name
// may contain, what order the rows come in, what "delete" is allowed to reach)
// is a rule worth testing.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

struct WorldEntry {
    std::string name;  // the directory name, which is also what the player sees
    std::string path;  // savesDir + "/" + name
    i64 lastPlayed = 0;
    i64 seed = 0;
};

// Every directory under savesDir whose level.dat decodes, most recently played
// first, ties broken by name so the order never depends on what readdir
// happened to hand back.
//
// A directory with no level.dat is not a world and is skipped in silence -- a
// saves folder lives on a card the player also uses for other things. One with
// a level.dat that will not decode is skipped too: showing a row that cannot be
// opened is worse than not showing it, and the file is still there for a
// maintainer to look at.
void listWorlds(io::FileSystem& fs, std::string_view savesDir, std::vector<WorldEntry>* out);

// savesDir + "/" + name, so the caller does not have to know the separator.
std::string worldPath(std::string_view savesDir, std::string_view name);

// The first "World<n>" not already taken, which is what a1.1.2's five fixed
// slots are called and therefore what a player expects to see offered.
std::string defaultWorldName(const std::vector<WorldEntry>& existing);

// What a player typed, made safe to be a directory name on a FAT card.
//
// Drops every character FAT forbids and every control character, collapses runs
// of spaces, and trims leading and trailing spaces and dots -- Windows will not
// create "foo." and a card is read on a PC as often as on a console. Returns
// false if nothing usable is left, which the caller reports rather than
// silently inventing a name.
bool sanitizeWorldName(std::string_view typed, std::string* out);

// Deletes a world directory and everything under it.
//
// **Refuses anything that is not a world.** The path must hold a level.dat, so
// a bug in the caller that hands this the saves folder itself, or the SD root,
// removes nothing. This is the only destructive operation in the project and
// the guard is the point of it being here rather than inline in the menu.
bool deleteWorld(io::FileSystem& fs, std::string_view worldDir);

}  // namespace mc::world
