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
    // Spectator is not aspirational here: free flight through the world with no
    // body is the whole of what the mode means and the whole of what it does.
    //
    // **Creative joined it at M3 step 3**, with a body that collides, reaches,
    // breaks and places. **Survival joined at step 4**, once the rules it adds
    // on top of that body were all there: health and death, fall and fire and
    // drowning, break progress and harvest drops, tool wear and spent stacks,
    // and the crafting grids, the furnace and the chest. It was held back
    // until then because a selectable mode that played like Creative would
    // have been a label that lies. See docs/todo-m3.md step 4.
    //
    // Kept as a function, and asked by the menus, so a mode added later starts
    // greyed out rather than half-working.
    switch (mode) {
    case Gamemode::Spectator:
    case Gamemode::Creative:
    case Gamemode::Survival:
        return true;
    }
    return false;
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

const char* panoramaAnchorToken(PanoramaAnchor anchor)
{
    switch (anchor) {
    case PanoramaAnchor::Origin:
        return "origin";
    case PanoramaAnchor::Player:
        return "player";
    case PanoramaAnchor::Spawn:
        break;
    }
    return "spawn";
}

const char* panoramaAnchorLabel(PanoramaAnchor anchor)
{
    switch (anchor) {
    case PanoramaAnchor::Origin:
        return "Block 0, 0";
    case PanoramaAnchor::Player:
        return "Where you logged out";
    case PanoramaAnchor::Spawn:
        break;
    }
    return "World spawn";
}

bool panoramaAnchorFromToken(std::string_view token, PanoramaAnchor* out)
{
    if (token == "spawn") {
        *out = PanoramaAnchor::Spawn;
        return true;
    }
    if (token == "origin") {
        *out = PanoramaAnchor::Origin;
        return true;
    }
    if (token == "player") {
        *out = PanoramaAnchor::Player;
        return true;
    }
    return false;
}

const char* difficultyToken(Difficulty level)
{
    switch (level) {
    case Difficulty::Peaceful:
        return "peaceful";
    case Difficulty::Easy:
        return "easy";
    case Difficulty::Hard:
        return "hard";
    case Difficulty::Normal:
        break;
    }
    return "normal";
}

const char* difficultyLabel(Difficulty level)
{
    switch (level) {
    case Difficulty::Peaceful:
        return "Peaceful";
    case Difficulty::Easy:
        return "Easy";
    case Difficulty::Hard:
        return "Hard";
    case Difficulty::Normal:
        break;
    }
    return "Normal";
}

bool difficultyFromToken(std::string_view token, Difficulty* out)
{
    if (token == "peaceful") {
        *out = Difficulty::Peaceful;
        return true;
    }
    if (token == "easy") {
        *out = Difficulty::Easy;
        return true;
    }
    if (token == "normal") {
        *out = Difficulty::Normal;
        return true;
    }
    if (token == "hard") {
        *out = Difficulty::Hard;
        return true;
    }
    return false;
}

const char* boolToken(bool value)
{
    return value ? "true" : "false";
}

bool boolFromToken(std::string_view token, bool* out)
{
    if (token == "true" || token == "1" || token == "yes") {
        *out = true;
        return true;
    }
    if (token == "false" || token == "0" || token == "no") {
        *out = false;
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
                out->gamemode = mode;
            }
            // An unrecognised mode keeps the default and leaves the file
            // alone. A card can be edited on a PC, and a word this build does
            // not know is more likely a newer build's than a mistake.
            continue;
        }
        if (key == "difficulty") {
            Difficulty level = Difficulty::Normal;
            if (difficultyFromToken(value, &level)) {
                out->difficulty = level;
            }
            continue;
        }
        if (key == "fix_ore_generation") {
            boolFromToken(value, &out->fixOreGeneration);
            continue;
        }
        if (key == "fix_bedrock_hole") {
            boolFromToken(value, &out->fixBedrockHole);
            continue;
        }
        if (key == "improved_fence_placement") {
            boolFromToken(value, &out->improvedFencePlacement);
            continue;
        }
        if (key == "texture_pack") {
            // Kept verbatim, including a name this console has no pack for:
            // the card may have been carried from one that does, and a pack
            // that is missing today is a row the menu draws as missing rather
            // than a choice to forget on the player's behalf.
            out->texturePack.assign(value);
            continue;
        }
        if (key == "panorama_anchor") {
            panoramaAnchorFromToken(value, &out->panoramaAnchor);
            continue;
        }
        if (key == "panorama_tile_x") {
            int tile = 0;
            if (parseInt(value, &tile)) {
                out->panoramaTileX = i32(tile);
            }
            continue;
        }
        if (key == "panorama_tile_z") {
            int tile = 0;
            if (parseInt(value, &tile)) {
                out->panoramaTileZ = i32(tile);
            }
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
    text += "difficulty=";
    text += difficultyToken(settings.difficulty);
    text += '\n';

    // The Extra Settings rows. Written unconditionally rather than only when
    // they differ from the default, so a player who opens this file on a PC
    // sees the whole set and can tell an off switch from a key this build did
    // not know about.
    text += "fix_ore_generation=";
    text += boolToken(settings.fixOreGeneration);
    text += '\n';
    text += "fix_bedrock_hole=";
    text += boolToken(settings.fixBedrockHole);
    text += '\n';
    text += "improved_fence_placement=";
    text += boolToken(settings.improvedFencePlacement);
    text += '\n';
    text += "texture_pack=";
    text += settings.texturePack;
    text += '\n';
    text += "panorama_anchor=";
    text += panoramaAnchorToken(settings.panoramaAnchor);
    text += '\n';
    text += "panorama_tile_x=";
    text += std::to_string((long long)settings.panoramaTileX);
    text += '\n';
    text += "panorama_tile_z=";
    text += std::to_string((long long)settings.panoramaTileZ);
    text += '\n';

    const std::string path = worldSettingsPath(worldDir);
    return fs.writeFileAtomic(
        path.c_str(),
        ConstByteSpan(reinterpret_cast<const u8*>(text.data()), text.size()));
}

}  // namespace mc::settings
