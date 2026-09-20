#pragma once

// sdmc:/alpha/3ds.ini -- the handful of choices that have to survive a power
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
#include "core/settings/control_scheme.hpp"
#include "core/settings/online_privacy.hpp"
#include "core/util/types.hpp"

#include <string>

namespace mc::settings {

inline constexpr char kSettingsPath[] = "sdmc:/alpha/3ds.ini";

struct GameSettings {
    // 0 means "not chosen yet"; the menu fills in the per-model default it has
    // always used rather than this file inventing one for a console it cannot
    // see from here.
    int renderDistance = 0;

    // The pack's file name inside the packs folder, or empty for the built-in
    // Dev Art. A name rather than a path, so moving the card's alpha folder
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

    // How fast the view turns, as the percentage a1.1.2's own slider prints:
    // 0..200, and 100 is the rate this port had before the row existed. -1 is
    // "not chosen yet", the convention `autosaveSeconds` already uses, and
    // becomes `kDefaultSensitivity`. See core/settings/sensitivity.hpp for the
    // curve and for why 100% is exactly unity.
    int lookSensitivity = -1;

    // **Which stick walks and which one turns** -- see
    // core/settings/control_scheme.hpp for the three pairings and why there
    // are three.
    //
    // This one cannot use the "0 means not chosen yet" convention the numbers
    // above use, because every value of the enum is a real answer, so the
    // absence is carried in its own flag. Absent is first boot, or a file an
    // older build wrote: the menu fills in the scheme named after the console
    // it is running on, rather than this file inventing one for a model it
    // cannot see from here.
    bool controlSchemeChosen = false;
    ControlScheme controlScheme = ControlScheme::New3DS;

    // **Where the Profile screen connects.** The website's URL, because that is
    // the form of the address anybody publishes and the thing a player types a
    // link code into; the control port is a UDP port on the same host that
    // appears in no URL. Empty means the default -- see
    // `net::ac::kDefaultServerUrl` -- rather than "no server", so that a file
    // written by a build that predates this row still reaches somewhere.
    std::string serverUrl;

    // **How far a world this console opens on the internet reaches.** Asked on
    // the way into hosting rather than in Options, because it is a question
    // about the session being opened -- but remembered here, because a player
    // who has answered it once should not have to answer it again to host the
    // same world tomorrow. See core/settings/online_privacy.hpp.
    OnlinePrivacy onlinePrivacy = OnlinePrivacy::CodeOnly;

    // **The account this console was last told it is on**, which is a cache of
    // the server's answer and not a setting. It is here so that the Profile
    // screen has something to draw the instant it opens, before a socket exists
    // -- a screen that must connect before it can say anything is a screen that
    // makes a player wait to be told nothing has changed. Empty means "not
    // linked, as far as this console last knew".
    std::string accountHandle;
    std::string accountName;

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
