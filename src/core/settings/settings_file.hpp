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
};

// False when there is no file, which is the ordinary first-boot state; *out is
// left at its defaults either way. A malformed line is skipped rather than
// failing the load -- a card can be edited on a PC.
bool loadSettings(io::FileSystem& fs, const char* path, GameSettings* out);

// Writes through writeFileAtomic, so a console switched off mid-save keeps the
// settings it had.
bool saveSettings(io::FileSystem& fs, const char* path, const GameSettings& settings);

}  // namespace mc::settings
