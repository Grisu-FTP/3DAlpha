#include "core/world/world_list.hpp"

#include "version_slots.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mc::world {

namespace {

// The names the listing collects before any of them is opened. Reading a
// level.dat inside listDirectory's visitor would mean a file operation inside a
// directory walk, which on the console is one IPC round trip nested inside
// another's iterator.
struct NameCollector {
    std::vector<std::string> names;
};

bool collectDirectory(void* context, const io::DirEntry& entry)
{
    NameCollector& collector = *static_cast<NameCollector*>(context);
    if (!entry.isDirectory || entry.name[0] == '.') {
        return true;
    }
    collector.names.emplace_back(entry.name);
    return true;
}

// A world is three levels deep -- <world>/<xx>/<zz>/c.x.z.dat -- so this is
// generous rather than tight. It bounds the recursion against a directory tree
// that loops back on itself, which a card mounted on a PC can be made to have.
constexpr int kMaxDeleteDepth = 8;

bool removeTree(io::FileSystem& fs, const std::string& path, int depth)
{
    if (depth > kMaxDeleteDepth) {
        return false;
    }

    struct Children {
        std::vector<std::pair<std::string, bool>> entries;  // name, isDirectory
    } found;

    const bool isDir = fs.listDirectory(
        path.c_str(), &found, [](void* context, const io::DirEntry& entry) {
            static_cast<Children*>(context)->entries.emplace_back(entry.name,
                                                                  entry.isDirectory);
            return true;
        });

    if (!isDir) {
        // Not a directory, or one that cannot be opened. Either way the only
        // thing left to try is removing it as a file.
        return fs.removeFile(path.c_str());
    }

    bool ok = true;
    for (const auto& entry : found.entries) {
        const std::string child = path + "/" + entry.first;
        if (entry.second) {
            ok = removeTree(fs, child, depth + 1) && ok;
        } else {
            ok = fs.removeFile(child.c_str()) && ok;
        }
    }
    // The directory itself last, and its result is part of the answer: a
    // removal that left the folder behind did not delete the world.
    return fs.removeDirectory(path.c_str()) && ok;
}

}  // namespace

void listWorlds(io::FileSystem& fs, std::string_view savesDir, std::vector<WorldEntry>* out)
{
    out->clear();

    const std::string saves(savesDir);
    NameCollector collector;
    if (!fs.listDirectory(saves.c_str(), &collector, collectDirectory)) {
        // No saves folder yet is the state a console is in before the first
        // world is made, so it is empty rather than an error.
        return;
    }

    // One storage object for the whole walk. peekLevel touches none of its
    // state, so this is a place to hang the file system and nothing more.
    mcver::Storage storage(fs);

    out->reserve(collector.names.size());
    for (const std::string& name : collector.names) {
        WorldEntry entry;
        entry.name = name;
        entry.path = worldPath(savesDir, name);

        LevelData level;
        if (!storage.peekLevel(entry.path, &level)) {
            continue;
        }
        entry.lastPlayed = level.lastPlayed;
        entry.seed = level.randomSeed;
        out->push_back(std::move(entry));
    }

    std::sort(out->begin(), out->end(), [](const WorldEntry& a, const WorldEntry& b) {
        if (a.lastPlayed != b.lastPlayed) {
            return a.lastPlayed > b.lastPlayed;
        }
        return a.name < b.name;
    });
}

std::string worldPath(std::string_view savesDir, std::string_view name)
{
    std::string path(savesDir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path.append(name);
    return path;
}

std::string defaultWorldName(const std::vector<WorldEntry>& existing)
{
    char candidate[32];
    // The bound is the count plus one: with n worlds taken, one of World1 ..
    // World(n+1) is always free.
    for (usize n = 1; n <= existing.size() + 1; ++n) {
        std::snprintf(candidate, sizeof(candidate), "World%u", unsigned(n));
        bool taken = false;
        for (const WorldEntry& entry : existing) {
            if (entry.name == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
    return "World";
}

bool sanitizeWorldName(std::string_view typed, std::string* out)
{
    out->clear();
    for (const char c : typed) {
        const u8 byte = u8(c);
        // FAT's reserved set, plus every control character and DEL. Bytes above
        // 0x7F are kept: the card's long-name entries are UTF-16 and libctru
        // transcodes, so an accented world name is fine.
        if (byte < 0x20 || byte == 0x7F) {
            continue;
        }
        if (std::strchr("\\/:*?\"<>|", c) != nullptr) {
            continue;
        }
        if (c == ' ' && (out->empty() || out->back() == ' ')) {
            continue;  // no leading name and no doubled spaces
        }
        out->push_back(c);
        if (out->size() >= 63) {
            break;
        }
    }

    // Trailing spaces and dots: FAT stores them, Windows refuses to open what
    // it made, and a name that is only dots is "." or ".." to every listing
    // there is.
    while (!out->empty() && (out->back() == ' ' || out->back() == '.')) {
        out->pop_back();
    }
    while (!out->empty() && out->front() == '.') {
        out->erase(out->begin());
    }
    return !out->empty();
}

bool deleteWorld(io::FileSystem& fs, std::string_view worldDir)
{
    const std::string path(worldDir);
    LevelData level;
    mcver::Storage storage(fs);
    if (!storage.peekLevel(path, &level)) {
        // Not a world. Refusing here is what stops a caller bug from becoming
        // an unbounded delete, so it is not merely a courtesy check.
        return false;
    }
    return removeTree(fs, path, 0);
}

}  // namespace mc::world
