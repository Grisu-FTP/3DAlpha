#pragma once

// What the player dropped in `sdmc:/alpha/resources/`, sorted into the three
// pools a1.1.2 keeps.
//
// a1.1.2 never shipped its sounds. It downloaded them at runtime from
// `http://s3.amazonaws.com/MinecraftResources/` into `.minecraft/resources/`,
// walking an S3 listing and calling `Minecraft.installResource(key, file)` for
// each. That server has been gone for years, so the download is replaced by the
// one thing a player can still do: copy the folder across. The *layout* is the
// original's, unchanged, so a `resources/` directory taken from any alpha- or
// beta-era install works as-is -- see docs/assets.md.
//
// `installResource` splits the key at the **first** slash and switches on what
// is in front of it. There are exactly five categories and this is the whole of
// the routing:
//
//     sound/…      newsound/…   -> the sound pool      (random, digits stripped)
//     streaming/…               -> the streaming pool  (exact names, digits kept)
//     music/…      newmusic/…   -> the music pool      (random, digits stripped)
//
// Anything else is ignored, which matters more than it sounds: a folder copied
// off a modern install also carries `pack.mcmeta`, `icons/`, `sound3/` and
// `pe/`, and none of those are ours to interpret.
//
// **The category is consumed, not kept.** What goes into the pool is the path
// *after* it -- `sound/random/bow.ogg` is registered as `random/bow.ogg` and so
// keys as `random.bow`, which is the name the game asks for. Keeping the
// category would key it `sound.random.bow` and every effect lookup would miss.
//
// **The walk lists directories; it never reads a file.** Building the index
// costs one `listDirectory` per folder and nothing else, so it happens once at
// boot and holds only names and paths -- a few hundred short strings for a
// complete resources folder. Decoding happens later and only for what actually
// plays.
//
// An absent folder is the ordinary state, not an error: the pools come back
// empty, the music ticker finds nothing to start, and the game is silent. That
// is the documented behaviour and it is the same code path as a missing DSP
// firmware.

#include "core/audio/sound_pool.hpp"
#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string_view>

namespace mc::audio {

// Beside texture::kPacksDir, and for the same reason: a name the game knows
// rather than a path the player is asked for.
inline constexpr char kResourcesDir[] = "sdmc:/alpha/resources";

// A card can hold anything, and a runaway directory tree would otherwise be a
// hang at boot. Deep enough for `newsound/mob/…`, which is the deepest the
// original layout goes.
inline constexpr int kMaxResourceDepth = 6;

// Roughly ten times a complete original resources folder. Past this the walk
// stops and reports what it has, rather than growing without bound because
// somebody pointed the game at their music library.
inline constexpr usize kMaxResourceEntries = 4096;

struct ResourceIndex {
    SoundPool sounds{true};      // `sound/` and `newsound/`
    SoundPool streaming{false};  // `streaming/` -- records, asked for by name
    SoundPool music{true};       // `music/` and `newmusic/`

    usize total() const { return sounds.size() + streaming.size() + music.size(); }
    bool empty() const { return total() == 0; }
};

// Walks `root` and fills `out`. Returns false only if `root` could not be
// listed at all -- an empty or partly-recognised folder is success with an
// empty or partial index, because that is a card the player can fix and not a
// state the game should refuse to start in.
//
// Extensions are not filtered here. a1.1.2 registers whatever it is handed and
// picks a codec from the name at play time; a file we cannot decode fails then,
// once, rather than being second-guessed now.
bool indexResources(io::FileSystem& fs, std::string_view root, ResourceIndex* out);

}  // namespace mc::audio
