#include "core/settings/world_settings.hpp"

#include "core/settings/ini.hpp"

#include <vector>

namespace mc::settings {

namespace {

// Larger than this and the file is not ours any more, whatever its name says.
constexpr usize kMaxBytes = 8 * 1024;

}  // namespace

const char* gamemodeToken(Gamemode mode)
{
    switch (mode) {
    case Gamemode::Survival:
        return "survival";
    case Gamemode::Creative:
        return "creative";
    case Gamemode::Spectator:
        break;
    }
    return "spectator";
}

const char* gamemodeLabel(Gamemode mode)
{
    switch (mode) {
    case Gamemode::Survival:
        return "Survival";
    case Gamemode::Creative:
        return "Creative";
    case Gamemode::Spectator:
        break;
    }
    return "Spectator";
}

bool gamemodeImplemented(Gamemode mode)
{
    // Spectator is not aspirational here: with no player body there is no
    // collision, no reach and no inventory, so free flight through the world is
    // the whole of what this mode means and the whole of what the game does.
    return mode == Gamemode::Spectator;
}

bool gamemodeFromToken(std::string_view token, Gamemode* out)
{
    if (token == "spectator") {
        *out = Gamemode::Spectator;
        return true;
    }
    if (token == "survival") {
        *out = Gamemode::Survival;
        return true;
    }
    if (token == "creative") {
        *out = Gamemode::Creative;
        return true;
    }
    return false;
}

std::string worldSettingsPath(std::string_view worldDir)
{
    std::string path(worldDir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += kWorldSettingsName;
    return path;
}

bool loadWorldSettings(io::FileSystem& fs, std::string_view worldDir, WorldSettings* out)
{
    *out = WorldSettings();

    const std::string path = worldSettingsPath(worldDir);
    std::vector<u8> bytes;
    if (!fs.readFile(path.c_str(), &bytes, kMaxBytes)) {
        return false;
    }

    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string_view key;
    std::string_view value;
    while (nextEntry(&text, &key, &value)) {
        if (key == "gamemode") {
            Gamemode mode = Gamemode::Spectator;
            if (gamemodeFromToken(value, &mode)) {
                *out = WorldSettings{mode};
            }
            // An unrecognised mode keeps the default and leaves the file
            // alone. A card can be edited on a PC, and a word this build does
            // not know is more likely a newer build's than a mistake.
            continue;
        }
        // Unknown keys are dropped, and saving will not write them back. Said
        // in the header so it is a decision rather than a surprise.
    }
    return true;
}

bool saveWorldSettings(io::FileSystem& fs, std::string_view worldDir,
                       const WorldSettings& settings)
{
    std::string text;
    text += "# 3DAlpha per-world settings. Not part of the Minecraft world format;\n";
    text += "# a real client ignores this file. Rewritten by the game.\n";
    text += "gamemode=";
    text += gamemodeToken(settings.gamemode);
    text += '\n';

    const std::string path = worldSettingsPath(worldDir);
    return fs.writeFileAtomic(
        path.c_str(),
        ConstByteSpan(reinterpret_cast<const u8*>(text.data()), text.size()));
}

}  // namespace mc::settings
