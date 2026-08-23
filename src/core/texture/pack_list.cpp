#include "core/texture/pack_list.hpp"

#include "core/texture/atlas_image.hpp"
#include "core/texture/zip_archive.hpp"

#include <algorithm>
#include <cstring>

namespace mc::texture {

// Read out of a real a1.1.2 client jar with `zipfile`, sorted by name. Not
// hand-written: every previous attempt to write a list like this from memory
// has been wrong in small places, and each wrong entry here is a pack silently
// reported as less complete than it is.
const char* const kA112Files[] = {
    "2char.png",
    "armor/chain_1.png",
    "armor/chain_2.png",
    "armor/cloth_1.png",
    "armor/cloth_2.png",
    "armor/diamond_1.png",
    "armor/diamond_2.png",
    "armor/gold_1.png",
    "armor/gold_2.png",
    "armor/iron_1.png",
    "armor/iron_2.png",
    "art/kz.png",
    "char.png",
    "clouds.png",
    "default.png",
    "dirt.png",
    "fluff.png",
    "grass.png",
    "gui/container.png",
    "gui/crafting.png",
    "gui/furnace.png",
    "gui/gui.png",
    "gui/icons.png",
    "gui/inventory.png",
    "gui/items.png",
    "gui/logo.png",
    "item/arrows.png",
    "item/boat.png",
    "item/cart.png",
    "item/door.png",
    "item/sign.png",
    "misc/gear.png",
    "misc/gearmiddle.png",
    "misc/vignette.png",
    "mob/chicken.png",
    "mob/cow.png",
    "mob/creeper.png",
    "mob/pig.png",
    "mob/saddle.png",
    "mob/sheep.png",
    "mob/sheep_fur.png",
    "mob/skeleton.png",
    "mob/slime.png",
    "mob/spider.png",
    "mob/spider_eyes.png",
    "mob/zombie.png",
    "particles.png",
    "rain.png",
    "rock.png",
    "shadow.png",
    "snow.png",
    "terrain.png",
    "terrain/moon.png",
    "terrain/sun.png",
    "title/black.png",
    "title/mojang.png",
    "water.png",
    "waterterrain.png",
};

const int kA112FileCount = int(sizeof(kA112Files) / sizeof(kA112Files[0]));

namespace {

constexpr char kDevArtName[] = "Dev Art";
constexpr char kTerrainName[] = "terrain.png";

// The read ceilings live in atlas_image.hpp, next to the reasoning about the
// console's heap.
using texture::kMaxPackBytes;

bool endsWithNoCase(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size()) {
        return false;
    }
    const usize offset = text.size() - suffix.size();
    for (usize i = 0; i < suffix.size(); ++i) {
        const char a = text[offset + i];
        const char lower = (a >= 'A' && a <= 'Z') ? char(a - 'A' + 'a') : a;
        if (lower != suffix[i]) {
            return false;
        }
    }
    return true;
}

// The names a listing collects before any of them is opened. Reading a file
// inside listDirectory's visitor would mean a file operation inside a directory
// walk, which on the console is one IPC round trip nested inside another's
// iterator. Same rule as core/world/world_list.cpp.
struct NameCollector {
    std::vector<std::pair<std::string, bool>> entries;  // name, isDirectory
};

bool collectDirectory(void* context, const io::DirEntry& entry)
{
    NameCollector& collector = *static_cast<NameCollector*>(context);
    if (entry.name[0] == '.') {
        return true;
    }
    collector.entries.emplace_back(entry.name, entry.isDirectory);
    return true;
}

// How much of the a1.1.2 layout a set of names covers.
int countKnown(const std::vector<std::string>& names)
{
    int found = 0;
    for (int i = 0; i < kA112FileCount; ++i) {
        for (const std::string& name : names) {
            if (name == kA112Files[i]) {
                ++found;
                break;
            }
        }
    }
    return found;
}

// Looks inside a zip: does it hold a terrain.png, and how much of the layout.
// Central directory only -- nothing is decompressed.
bool describeZip(io::FileSystem& fs, const std::string& path, PackEntry* entry)
{
    std::vector<u8> bytes;
    if (!fs.readFile(path.c_str(), &bytes, kMaxPackBytes)) {
        return false;
    }
    ZipArchive zip;
    if (zip.open(bytes) != ZipError::Ok) {
        return false;
    }

    std::vector<std::string> names;
    names.reserve(zip.entries().size());
    for (const ZipEntry& e : zip.entries()) {
        names.push_back(e.name);
    }
    entry->hasTerrain = zip.find(kTerrainName) != nullptr;
    entry->textureCount = countKnown(names);
    return entry->hasTerrain;
}

// The same for a directory pack. One listing per subdirectory the layout names,
// rather than a walk of everything the player happens to have put in there.
bool describeDirectory(io::FileSystem& fs, const std::string& path, PackEntry* entry)
{
    // The layout is two levels deep at most -- "gui/gui.png", "mob/cow.png" --
    // so the subdirectories are known rather than discovered.
    static const char* const kSubdirs[] = {"", "armor/", "art/", "gui/",
                                           "item/", "misc/", "mob/", "terrain/", "title/"};

    std::vector<std::string> names;
    for (const char* subdir : kSubdirs) {
        std::string dir = path;
        if (subdir[0] != '\0') {
            dir += '/';
            dir.append(subdir, std::strlen(subdir) - 1);  // without the trailing slash
        }
        NameCollector collector;
        if (!fs.listDirectory(dir.c_str(), &collector, collectDirectory)) {
            continue;
        }
        for (const auto& found : collector.entries) {
            if (!found.second) {
                names.push_back(std::string(subdir) + found.first);
            }
        }
    }

    entry->hasTerrain = std::find(names.begin(), names.end(), kTerrainName) != names.end();
    entry->textureCount = countKnown(names);
    return entry->hasTerrain;
}

}  // namespace

std::string packPath(std::string_view packsDir, std::string_view name)
{
    std::string path(packsDir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path.append(name);
    return path;
}

void listPacks(io::FileSystem& fs, std::string_view packsDir, std::vector<PackEntry>* out)
{
    out->clear();

    // Pinned first and always present. It needs nothing off the card, so a
    // console with no packs folder still has a pack -- which is what makes
    // this a selector rather than a thing that can leave the game untextured.
    PackEntry devArt;
    devArt.name = kDevArtName;
    devArt.builtIn = true;
    devArt.hasTerrain = true;
    out->push_back(std::move(devArt));

    const std::string dir(packsDir);
    NameCollector collector;
    if (!fs.listDirectory(dir.c_str(), &collector, collectDirectory)) {
        // No packs folder yet is the state a console is in before the first
        // import, so it is empty rather than an error.
        return;
    }

    std::sort(collector.entries.begin(), collector.entries.end(),
              [](const std::pair<std::string, bool>& a, const std::pair<std::string, bool>& b) {
                  return a.first < b.first;
              });

    for (const auto& found : collector.entries) {
        const bool isDirectory = found.second;
        if (!isDirectory && !endsWithNoCase(found.first, ".zip")) {
            continue;  // a jar, a readme, whatever else is in there
        }

        PackEntry entry;
        entry.name = found.first;
        entry.path = packPath(packsDir, found.first);

        const bool usable = isDirectory ? describeDirectory(fs, entry.path, &entry)
                                        : describeZip(fs, entry.path, &entry);
        if (!usable) {
            continue;
        }
        out->push_back(std::move(entry));
    }
}

void listJars(io::FileSystem& fs, const std::string* dirs, int dirCount,
              std::vector<JarEntry>* out)
{
    out->clear();

    for (int i = 0; i < dirCount; ++i) {
        NameCollector collector;
        if (!fs.listDirectory(dirs[i].c_str(), &collector, collectDirectory)) {
            continue;
        }
        for (const auto& found : collector.entries) {
            if (found.second || !endsWithNoCase(found.first, ".jar")) {
                continue;
            }
            JarEntry entry;
            entry.name = found.first;
            entry.path = packPath(dirs[i], found.first);

            // The size is what tells a 900 KB alpha jar apart from a 200 MB
            // modern one at a glance. Through fileSize rather than readFile:
            // measuring a card's worth of jars must not mean loading them all
            // into memory first. A jar that cannot be stat'd is not offered.
            if (!fs.fileSize(entry.path.c_str(), &entry.bytes)) {
                continue;
            }

            const bool duplicate =
                std::any_of(out->begin(), out->end(), [&](const JarEntry& seen) {
                    return seen.path == entry.path;
                });
            if (!duplicate) {
                out->push_back(std::move(entry));
            }
        }
    }

    std::sort(out->begin(), out->end(),
              [](const JarEntry& a, const JarEntry& b) { return a.name < b.name; });
}

}  // namespace mc::texture
