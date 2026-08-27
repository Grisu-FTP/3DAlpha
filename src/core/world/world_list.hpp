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

#include "core/world/world_format.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

struct WorldEntry {
    std::string name;  // the directory name, which is also what the player sees
    std::string path;  // savesDir + "/" + name
    i64 lastPlayed = 0;
    i64 seed = 0;
    // Which shape the folder is in. Read off the disk with the rest of the
    // listing, because it is free there -- the same `exists` calls that decide
    // whether this is a world at all decide which kind it is.
    WorldFormat format = WorldFormat::Unknown;
};

// What a world costs, which is two different numbers.
//
// `contentBytes` is what the files hold. `onDiskBytes` is what the card gives
// up, every file rounded to a whole cluster. They are far apart in the folder
// format -- a 2,917-byte chunk in a 16 KB cluster, plus a cluster for the leaf
// directory it is alone in -- and that gap is the entire argument for packing,
// so both are reported and the screen shows both.
struct WorldSize {
    u64 contentBytes = 0;
    u64 onDiskBytes = 0;
    u32 fileCount = 0;
    u32 directoryCount = 0;
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

// Removes a directory and everything under it, with no check of what it is.
//
// `deleteWorld` is the one to call for a world; this is for the converter,
// which has to clear a staging directory that is deliberately *not* a world.
bool removeTree(io::FileSystem& fs, std::string_view path);

// Adds up what a world occupies. **Walks the whole tree**, which for a folder
// world means a stat per chunk file across up to 4,096 leaf directories -- so
// it is called for one world when the player asks, never for every row of a
// list.
//
// The cluster size comes from io::queryVolumeInfo; with no answer available
// `onDiskBytes` equals `contentBytes` rather than being guessed at from an
// assumed cluster that a measured card has already contradicted.
bool worldSize(io::FileSystem& fs, std::string_view worldDir, WorldSize* out);

// Copies a world directory to a new path, file for file.
//
// Preserves whichever format the source is in: a copy is a backup, not a
// conversion. Refuses if the destination already exists, so it can never merge
// into somebody else's world.
bool copyWorld(io::FileSystem& fs, std::string_view sourceDir, std::string_view targetDir);

}  // namespace mc::world
