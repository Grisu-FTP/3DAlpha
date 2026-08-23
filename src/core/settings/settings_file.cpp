#include "core/settings/settings_file.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace mc::settings {

namespace {

// A settings file that is larger than this has been replaced with something
// else, and reading it would be reading whatever that is.
constexpr usize kMaxBytes = 64 * 1024;

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty()
           && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// A decimal integer, or false. Deliberately not atoi: "12abc" is a line
// somebody mistyped, and taking the 12 out of it silently is worse than
// ignoring the line.
//
// A leading '-' is accepted because the settings that mean "not chosen yet"
// say so with -1, and a value the game writes has to be one the game can read
// back -- otherwise the file round-trips through the parse failure rather than
// through the parser, which works by accident and stops working the moment
// anything else reads it.
bool parseInt(std::string_view text, int* out)
{
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 9) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = negative ? -value : value;
    return true;
}

}  // namespace

bool loadSettings(io::FileSystem& fs, const char* path, GameSettings* out)
{
    *out = GameSettings();

    std::vector<u8> bytes;
    if (!fs.readFile(path, &bytes, kMaxBytes)) {
        return false;
    }

    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty()) {
        const usize newline = text.find('\n');
        std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);

        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const usize equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));

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

    text += "texture_pack=";
    text += settings.texturePack;
    text += '\n';

    return fs.writeFileAtomic(path, ConstByteSpan(reinterpret_cast<const u8*>(text.data()),
                                                  text.size()));
}

}  // namespace mc::settings
