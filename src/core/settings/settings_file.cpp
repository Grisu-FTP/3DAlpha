#include "core/settings/settings_file.hpp"

#include "core/settings/ini.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace mc::settings {

namespace {

// A settings file that is larger than this has been replaced with something
// else, and reading it would be reading whatever that is.
constexpr usize kMaxBytes = 64 * 1024;

}  // namespace

bool loadSettings(io::FileSystem& fs, const char* path, GameSettings* out)
{
    *out = GameSettings();

    std::vector<u8> bytes;
    if (!fs.readFile(path, &bytes, kMaxBytes)) {
        return false;
    }

    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string_view key;
    std::string_view value;
    while (nextEntry(&text, &key, &value)) {
        if (key == "render_distance") {
            int distance = 0;
            if (parseInt(value, &distance)) {
                out->renderDistance = distance;
            }
            continue;
        }
        if (key == "autosave_seconds") {
            int seconds = 0;
            if (parseInt(value, &seconds)) {
                out->autosaveSeconds = seconds;
            }
            continue;
        }
        if (key == "chunk_cache_mb") {
            int megabytes = 0;
            if (parseInt(value, &megabytes)) {
                out->chunkCacheMB = megabytes;
            }
            continue;
        }
        if (key == "audio") {
            int enabled = 0;
            if (parseInt(value, &enabled)) {
                out->audio = enabled;
            }
            continue;
        }
        if (key == "music_volume") {
            int volume = 0;
            if (parseInt(value, &volume)) {
                out->musicVolume = volume;
            }
            continue;
        }
        if (key == "sound_volume") {
            int volume = 0;
            if (parseInt(value, &volume)) {
                out->soundVolume = volume;
            }
            continue;
        }
        if (key == "texture_pack") {
            // A pack name is a file name and must stay one: a value with a
            // separator in it would let an edited ini point the loader outside
            // the packs folder.
            if (value.find('/') == std::string_view::npos
                && value.find('\\') == std::string_view::npos) {
                out->texturePack.assign(value);
            }
            continue;
        }
        if (key == "skin") {
            // **The same guard `texture_pack` has, and for the same reason.**
            // A skin key is a prefix and a *file name*; a separator in it would
            // let an edited ini point the loader anywhere on the card.
            if (value.find('/') == std::string_view::npos
                && value.find('\\') == std::string_view::npos) {
                out->skin.assign(value);
            }
            continue;
        }
        // Unknown keys are dropped, and saving will not write them back. Said
        // in the header so it is a decision rather than a surprise.
    }
    return true;
}

bool saveSettings(io::FileSystem& fs, const char* path, const GameSettings& settings)
{
    std::string text;
    text += "# 3DAlpha settings. Rewritten by the game; unknown keys are not kept.\n";

    char line[128];
    std::snprintf(line, sizeof(line), "render_distance=%d\n", settings.renderDistance);
    text += line;

    std::snprintf(line, sizeof(line), "autosave_seconds=%d\n", settings.autosaveSeconds);
    text += line;
    std::snprintf(line, sizeof(line), "chunk_cache_mb=%d\n", settings.chunkCacheMB);
    text += line;

    std::snprintf(line, sizeof(line), "audio=%d\n", settings.audio);
    text += line;
    std::snprintf(line, sizeof(line), "music_volume=%d\n", settings.musicVolume);
    text += line;
    std::snprintf(line, sizeof(line), "sound_volume=%d\n", settings.soundVolume);
    text += line;

    text += "texture_pack=";
    text += settings.texturePack;
    text += '\n';

    text += "skin=";
    text += settings.skin;
    text += '\n';

    return fs.writeFileAtomic(path, ConstByteSpan(reinterpret_cast<const u8*>(text.data()),
                                                  text.size()));
}

}  // namespace mc::settings
