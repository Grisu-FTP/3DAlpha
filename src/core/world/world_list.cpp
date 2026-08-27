#include "core/world/world_list.hpp"

#include "core/io/volume_info.hpp"
#include "core/util/fat_name.hpp"
#include "core/world/any_storage.hpp"

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

// The suffix a half-finished conversion carries. Skipping it is load-bearing
// rather than tidy: a staging directory mid-pack holds a perfectly valid
// `world.3dm`, so without this the list would offer it as a playable world and
// the recovery pass would find it already opened.
constexpr char kConvertingSuffix[] = ".converting";

bool endsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size()
           && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool collectDirectory(void* context, const io::DirEntry& entry)
{
    NameCollector& collector = *static_cast<NameCollector*>(context);
    if (!entry.isDirectory || entry.name[0] == '.' || endsWith(entry.name, kConvertingSuffix)) {
        return true;
    }
    collector.names.emplace_back(entry.name);
    return true;
}

// A world is three levels deep -- <world>/<xx>/<zz>/c.x.z.dat -- so this is
// generous rather than tight. It bounds the recursion against a directory tree
// that loops back on itself, which a card mounted on a PC can be made to have.
constexpr int kMaxDeleteDepth = 8;

// A single file inside a world that is larger than this is not something a copy
// should be allocating for. Chunk files are kilobytes; this is generous by
// three orders of magnitude and still bounds the damage.
constexpr usize kMaxCopyFileBytes = 64u << 20;

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
    // AnyStorage rather than the version's slot, because a packed world has no
    // level.dat to peek at and would otherwise be silently missing from the
    // list of the player's own worlds.
    AnyStorage storage(fs);

    out->reserve(collector.names.size());
    for (const std::string& name : collector.names) {
        WorldEntry entry;
        entry.name = name;
        entry.path = worldPath(savesDir, name);
        entry.format = detectFormat(fs, entry.path);

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
    // The rule lives in core/util/fat_name.cpp now: the pack importer needs the
    // same one for a pack file's name, and two copies of "which bytes will a
    // card accept" is two chances to get it subtly different.
    return util::sanitizeFatName(typed, out);
}

bool deleteWorld(io::FileSystem& fs, std::string_view worldDir)
{
    const std::string path(worldDir);
    if (detectFormat(fs, path) == WorldFormat::Unknown) {
        // Not a world in either shape. Refusing here is what stops a caller bug
        // from becoming an unbounded delete, so it is not merely a courtesy
        // check -- it is why this lives in core with a test on it.
        return false;
    }
    return removeTree(fs, path, 0);
}

bool removeTree(io::FileSystem& fs, std::string_view path)
{
    return removeTree(fs, std::string(path), 0);
}

namespace {

// Both walks below follow removeTree's shape: names are gathered inside the
// visitor and acted on after it returns, because on the console a file
// operation inside a directory walk is one IPC round trip nested inside
// another's iterator. The depth cap is the same one, for the same reason.
struct Children {
    std::vector<std::pair<std::string, bool>> entries;  // name, isDirectory
};

bool collectChildren(void* context, const io::DirEntry& entry)
{
    static_cast<Children*>(context)->entries.emplace_back(entry.name, entry.isDirectory);
    return true;
}

bool measureTree(io::FileSystem& fs, const std::string& path, u64 clusterSize,
                 WorldSize* out, int depth)
{
    if (depth > kMaxDeleteDepth) {
        return false;
    }
    Children found;
    if (!fs.listDirectory(path.c_str(), &found, collectChildren)) {
        return false;
    }
    ++out->directoryCount;
    // A directory costs a cluster of its own on FAT, and in the Alpha layout
    // there is a leaf directory per chunk for any world smaller than 64x64
    // chunks -- which makes the directories about half of what a world
    // occupies. Leaving them out would understate the cost by 2x.
    out->onDiskBytes += clusterSize;

    for (const auto& entry : found.entries) {
        const std::string child = path + "/" + entry.first;
        if (entry.second) {
            if (!measureTree(fs, child, clusterSize, out, depth + 1)) {
                return false;
            }
            continue;
        }
        usize bytes = 0;
        if (!fs.fileSize(child.c_str(), &bytes)) {
            continue;
        }
        ++out->fileCount;
        out->contentBytes += u64(bytes);
        out->onDiskBytes += io::onDiskSize(u64(bytes), clusterSize);
    }
    return true;
}

bool copyTree(io::FileSystem& fs, const std::string& source, const std::string& target,
              int depth)
{
    if (depth > kMaxDeleteDepth) {
        return false;
    }
    Children found;
    if (!fs.listDirectory(source.c_str(), &found, collectChildren)) {
        return false;
    }
    if (!fs.makeDirectories(target.c_str())) {
        return false;
    }

    std::vector<u8> bytes;
    for (const auto& entry : found.entries) {
        const std::string from = source + "/" + entry.first;
        const std::string to = target + "/" + entry.first;
        if (entry.second) {
            if (!copyTree(fs, from, to, depth + 1)) {
                return false;
            }
            continue;
        }
        bytes.clear();
        // The ceiling is the same one the chunk reader uses: a file on a card
        // the player also uses for other things could be any size, and a copy
        // is not a reason to allocate it.
        if (!fs.readFile(from.c_str(), &bytes, kMaxCopyFileBytes)) {
            return false;
        }
        if (!fs.writeFileAtomic(to.c_str(), ConstByteSpan(bytes.data(), bytes.size()))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool worldSize(io::FileSystem& fs, std::string_view worldDir, WorldSize* out)
{
    *out = WorldSize();

    io::VolumeInfo volume;
    // Unknown is a normal answer -- the host has no SD card. Then the two
    // numbers come out equal, which is honest: a footprint nobody can measure
    // is better reported as the content it holds than guessed at.
    const u64 clusterSize = io::queryVolumeInfo(std::string(worldDir).c_str(), &volume)
                                ? volume.clusterSize
                                : 0;
    return measureTree(fs, std::string(worldDir), clusterSize, out, 0);
}

bool copyWorld(io::FileSystem& fs, std::string_view sourceDir, std::string_view targetDir)
{
    const std::string source(sourceDir);
    const std::string target(targetDir);
    if (detectFormat(fs, source) == WorldFormat::Unknown) {
        return false;
    }
    // Never merge into something that is already there. The menu checks the
    // name against the world list before asking, but the card is also editable
    // on a PC and this is the check that cannot be raced.
    if (fs.exists(target.c_str())) {
        return false;
    }
    if (!copyTree(fs, source, target, 0)) {
        // A half-written copy is not something to leave on the card under a
        // name that looks like a world.
        removeTree(fs, target, 0);
        return false;
    }
    return true;
}

}  // namespace mc::world
