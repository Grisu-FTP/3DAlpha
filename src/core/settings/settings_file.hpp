#pragma once

// sdmc:/3dalpha/3ds.ini -- the handful of choices that have to survive a power
// cycle.
//
// docs/assets.md has named this file since before anything wrote it. It is ours
// rather than the original's: a1.1.2's own `options.txt` is a different file
// with a different format, and when there is enough gameplay to have options
// worth writing there it gets its own reader. Keeping them apart means a 3DS
// setting never ends up in a file a PC copy of the game would read.
//
// `key=value`, one per line, `#` for a comment. **Saving rewrites the file from
// the keys below, so a key this build does not know is dropped**, which is
// worth knowing before a later version adds one and an older build eats it.
// An absent file is the first-boot state, not an error.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>

namespace mc::settings {

inline constexpr char kSettingsPath[] = "sdmc:/3dalpha/3ds.ini";

struct GameSettings {
    // 0 means "not chosen yet"; the menu fills in the per-model default it has
    // always used rather than this file inventing one for a console it cannot
    // see from here.
    int renderDistance = 0;

    // The pack's file name inside the packs folder, or empty for the built-in
    // Dev Art. A name rather than a path, so moving the card's 3dalpha folder
    // does not orphan the setting.
    std::string texturePack;

    // Seconds between autosaves; 0 is off, meaning the world is written when
    // the pause menu opens and when it is left, and at no other time.
    //
    // -1 means "not chosen yet", the same convention renderDistance uses: the
    // menu fills in kDefaultAutosaveSeconds rather than this file inventing a
    // number. A file written by an older build has no such key and gets it.
    int autosaveSeconds = -1;

    // Megabytes of chunk cache -- retained columns and the read-ahead band.
    // 0 means "not chosen yet"; the menu picks per model, because what is
    // spare depends on the heap split and that depends on the console.
    int chunkCacheMB = 0;

    // Whether the DSP is brought up at all. This is not the same thing as a
    // volume of zero and the difference is the point of the row: off means
    // `ndspInit` is never called and the decode thread is never spawned, which
    // is what docs/3ds-performance.md's options contract promises. -1 is "not
    // chosen yet" and the menu turns it on.
    int audio = -1;

    // 0..100, because that is what the original's own label prints --
    // `"Music: " + (v > 0 ? (int)(v * 100) + "%" : "OFF")` -- and because
    // parseInt is the only parser this file has. The conversion to the float
    // the gain arithmetic wants happens once, at the seam.
    //
    // -1 is "not chosen yet", the convention autosaveSeconds already uses, and
    // becomes 100: a1.1.2 defaults both volumes to 1.0F.
    int musicVolume = -1;
    int soundVolume = -1;

    // **Which player skin the arm of an empty hand is drawn with**, as
    // `SkinEntry::key`: empty for Default -- the active pack's `char.png`, or
    // the black silhouette when it has none -- `pack:<name>` for another pack's
    // skin, `file:<name.png>` for one in `skins/`. See
    // core/texture/skin_list.hpp for why it is a name and not a path.
    std::string skin;
};

// **Ours, not the original's.** a1.1.2 has no timed autosave: it writes a chunk
// when the chunk falls out of its 1024-slot cache, and everything else only on
// Save and quit to title. 45 seconds is a compromise between how much world a
// power-off can cost and how often a console with a card in it should be
// writing to it; generated columns do not wait for it in any case.
inline constexpr int kDefaultAutosaveSeconds = 45;

// False when there is no file, which is the ordinary first-boot state; *out is
// left at its defaults either way. A malformed line is skipped rather than
// failing the load -- a card can be edited on a PC.
bool loadSettings(io::FileSystem& fs, const char* path, GameSettings* out);

// Writes through writeFileAtomic, so a console switched off mid-save keeps the
// settings it had.
bool saveSettings(io::FileSystem& fs, const char* path, const GameSettings& settings);

}  // namespace mc::settings
