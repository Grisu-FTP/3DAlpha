// See skin_list.hpp.

#include "core/texture/skin_list.hpp"

#include "core/texture/atlas_image.hpp"

#include <algorithm>

namespace mc::texture {
namespace {

constexpr char kDefaultName[] = "Default";

// A skin's canonical width, and the two heights that can go with it. The width
// is what every coordinate in the format is relative to; an HD skin is a whole
// multiple of it.
constexpr int kSkinWidth = 64;

// The two texels that decide `detectSkinModel`. See the header: `u` 54 and 55
// are the last two columns of the right arm's unwrap, and a narrow arm's is
// only fourteen wide.
constexpr int kWideOnlyU = 54;
constexpr int kWideOnlyV = 20;

// The same rule `pack_list.cpp` uses, and duplicated rather than shared for the
// same reason its own copy is small: this is four lines and exporting it would
// be a header for four lines.
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

// Names first, files second. Same rule as pack_list.cpp: no file operation
// inside a listDirectory visitor.
struct NameCollector {
    std::vector<std::string> names;
};

bool collectFile(void* context, const io::DirEntry& entry)
{
    NameCollector& collector = *static_cast<NameCollector*>(context);
    if (entry.name[0] == '.' || entry.isDirectory) {
        return true;
    }
    collector.names.emplace_back(entry.name);
    return true;
}

// dir + "/" + leaf, tolerating a directory that already ends in a slash.
std::string join(std::string_view dir, std::string_view leaf)
{
    std::string path(dir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path.append(leaf);
    return path;
}

// "Steve.png" -> "Steve". The extension is noise on a screen that only lists
// PNGs, and the row is 220 pixels wide.
std::string withoutExtension(std::string_view name)
{
    const usize dot = name.rfind('.');
    return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
}

// Fills in what a decoded skin says about itself, or returns false when the
// bytes are not a skin this build can use.
bool describe(const std::vector<u8>& png, SkinEntry* entry)
{
    Image image;
    // A skin is a kilobyte or two. The default ceiling is a terrain.png's and
    // is far above anything here, so it is left alone rather than tightened to
    // a number that would have to be argued.
    if (decodePng(png, &image, kDefaultMaxPixels) != PngError::Ok) {
        return false;
    }
    // **The width has to divide 64 and the height has to be one of the two
    // shapes**, because every coordinate the model uses is a fraction of the
    // page and a skin of some other proportion is not a skin. A 64 x 64 is
    // accepted here and its lower half is simply never sampled -- see
    // `applyPlayerSkin`.
    if (image.width <= 0 || image.width % kSkinWidth != 0) {
        return false;
    }
    const int scale = image.width / kSkinWidth;
    if (image.height != 32 * scale && image.height != 64 * scale) {
        return false;
    }
    entry->width = image.width;
    entry->height = image.height;
    entry->model = detectSkinModel(image);
    return true;
}

}  // namespace

SkinModel detectSkinModel(const Image& image)
{
    if (image.width <= 0 || image.width % kSkinWidth != 0) {
        return SkinModel::Classic;
    }
    const int scale = image.width / kSkinWidth;
    // **Only a 64 x 64 can be narrow.** The format and the body type arrived
    // together, so a 64 x 32 is classic by construction and its arm's last two
    // columns are ordinary texels that may well be transparent by accident.
    if (image.height != 64 * scale) {
        return SkinModel::Classic;
    }
    const int x = kWideOnlyU * scale;
    const int y = kWideOnlyV * scale;
    if (x >= image.width || y >= image.height) {
        return SkinModel::Classic;
    }
    const u8 alpha = image.rgba[(usize(y) * usize(image.width) + usize(x)) * 4 + 3];
    return alpha == 0 ? SkinModel::Slim : SkinModel::Classic;
}

int findSkin(const std::vector<SkinEntry>& skins, std::string_view key)
{
    if (key.empty()) {
        return 0;
    }
    for (int i = 0; i < int(skins.size()); ++i) {
        if (skins[usize(i)].key == key) {
            return i;
        }
    }
    // The card no longer has it -- deleted, renamed, or a different card
    // entirely. Default rather than nothing.
    return 0;
}

bool skinPathForKey(std::string_view key, std::string_view packsDir, std::string_view skinsDir,
                    std::string* path, bool* fromPack)
{
    constexpr std::string_view kPackPrefix = "pack:";
    constexpr std::string_view kFilePrefix = "file:";
    if (key.substr(0, kPackPrefix.size()) == kPackPrefix) {
        *path = join(packsDir, key.substr(kPackPrefix.size()));
        *fromPack = true;
        return true;
    }
    if (key.substr(0, kFilePrefix.size()) == kFilePrefix) {
        *path = join(skinsDir, key.substr(kFilePrefix.size()));
        *fromPack = false;
        return true;
    }
    // Default, or a key from a build that knows a source this one does not.
    return false;
}

void listSkins(io::FileSystem& fs, const std::vector<PackEntry>& packs,
               std::string_view skinsDir, std::vector<SkinEntry>* out)
{
    out->clear();

    // Pinned first and always present, for the reason Dev Art is pinned first
    // on the pack list: this has to be a selector rather than a thing that can
    // leave the player with no arm.
    SkinEntry standard;
    standard.name = kDefaultName;
    standard.source = SkinSource::Default;
    out->push_back(std::move(standard));

    for (const PackEntry& pack : packs) {
        // Dev Art has no files at all, so `hasSkin` is false for it and this
        // needs no special case.
        if (!pack.hasSkin) {
            continue;
        }
        std::vector<u8> png;
        if (readPackFile(fs, pack.path, kSkinFile, &png) != PackError::Ok) {
            continue;
        }
        SkinEntry entry;
        entry.name = pack.name;
        entry.key = "pack:" + pack.name;
        entry.path = pack.path;
        entry.source = SkinSource::Pack;
        if (!describe(png, &entry)) {
            continue;
        }
        out->push_back(std::move(entry));
    }

    const std::string dir(skinsDir);
    NameCollector collector;
    if (!fs.listDirectory(dir.c_str(), &collector, collectFile)) {
        // No skins folder yet is the state a card is in before a player puts
        // one there, so it is empty rather than an error.
        return;
    }
    std::sort(collector.names.begin(), collector.names.end());

    for (const std::string& name : collector.names) {
        if (!endsWithNoCase(name, ".png")) {
            continue;
        }
        std::string path = join(dir, name);
        std::vector<u8> png;
        if (!fs.readFile(path.c_str(), &png, kMaxPackBytes)) {
            continue;
        }
        SkinEntry entry;
        entry.name = withoutExtension(name);
        entry.key = "file:" + name;
        entry.path = std::move(path);
        entry.source = SkinSource::File;
        if (!describe(png, &entry)) {
            continue;
        }
        out->push_back(std::move(entry));
    }
}

}  // namespace mc::texture
