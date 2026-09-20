#pragma once

// The packs folder as a list, for the texture-pack screen.
//
// Modelled on core/world/world_list.hpp, including its rule that no file
// operation runs inside a listDirectory visitor -- on the console that would be
// one IPC round trip nested inside another's iterator.
//
// It is core rather than platform code for the usual reason: the console cannot
// run a unit test, and every rule here (what counts as a pack, what order the
// rows come in, what a pack is called) is a rule worth testing.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::texture {

// Where packs and import sources live on the card.
inline constexpr char kPacksDir[] = "sdmc:/alpha/packs";

// The 58 PNG names in a real a1.1.2 client jar, read out of it rather than
// copied from a wiki. This is the single source of truth for the "n of 58"
// count on the pack screen and for the layout table in docs/assets.md.
//
// Three things in it are worth noticing, because docs/assets.md had all three
// wrong before the jar was opened: `misc/` holds only three files; water,
// grass, dirt, rock, snow, rain, particles, fluff and shadow are at the **root**
// rather than under `misc/`; and there is no `grasscolor.png` or
// `foliagecolor.png` at all -- a fourth independent confirmation that a1.1.2
// tints nothing.
extern const char* const kA112Files[];
extern const int kA112FileCount;

struct PackEntry {
    std::string name;  // what the player sees
    std::string path;  // empty for the built-in Dev Art
    bool builtIn = false;
    // How many of kA112Files the pack carries. Read from the zip's central
    // directory only -- no entry is decompressed and no PNG is decoded, so
    // listing a folder of packs costs one file read each.
    int textureCount = 0;
    bool hasTerrain = false;

    // **Whether the pack carries a player skin**, which is `char.png` at its
    // root. Read from the same name list the count above comes from, so it
    // costs nothing extra -- and it is what the skin screen lists packs from
    // without opening any of them a second time. See core/texture/skin_list.hpp.
    bool hasSkin = false;
};

// Dev Art pinned first, then every pack in packsDir sorted by name.
//
// A pack is a `.zip` holding a terrain.png, or a directory holding one. A zip
// that will not open is skipped in silence, the same way a directory without a
// level.dat is not a world -- the packs folder lives on a card the player also
// uses for other things.
void listPacks(io::FileSystem& fs, std::string_view packsDir, std::vector<PackEntry>* out);

// packsDir + "/" + name.
std::string packPath(std::string_view packsDir, std::string_view name);

// A jar the importer could be pointed at.
struct JarEntry {
    std::string name;
    std::string path;
    usize bytes = 0;
};

// Every `*.jar` directly inside each of the given directories, sorted by name,
// with duplicates by path removed. The menu passes the packs folder and the
// alpha folder above it, because a player who dropped a download onto the
// card has not necessarily put it in the right place or renamed it.
void listJars(io::FileSystem& fs, const std::string* dirs, int dirCount,
              std::vector<JarEntry>* out);

}  // namespace mc::texture
