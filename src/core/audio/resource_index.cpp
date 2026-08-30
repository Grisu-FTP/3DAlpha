#include "core/audio/resource_index.hpp"

#include <string>
#include <vector>

namespace mc::audio {

namespace {

struct Walk {
    io::FileSystem* fs;
    ResourceIndex* out;
    std::string root;

    // The key as `installResource` would see it: the path below `root`, with
    // forward slashes, extension intact. `music/calm1.ogg`.
    std::string key;

    // Absolute path of the directory being listed.
    std::string path;

    int depth;
    bool stopped;
};

void walkDirectory(Walk& walk);

// `Minecraft.a(String, File)` -- installResource. The category is everything
// before the first slash and chooses the pool; **what is registered is the
// remainder**, not the whole key, and that distinction is the whole reason this
// returns two things.
//
// `substring(indexOf("/") + 1)` is what the original hands to the pool, so
// `sound/random/bow.ogg` is filed under `random.bow` and `music/calm1.ogg`
// under `calm`. Passing the full path instead would key them `sound.random.bow`
// and `music.calm` -- invisible for music, which is drawn from the flat list,
// and fatal for every effect, which is looked up by exactly the name the game
// asks for.
//
// Null for a key with no slash (a loose file directly in `resources/`) or an
// unknown category; both are ignored, as the original ignores them.
SoundPool* poolForKey(ResourceIndex& index, std::string_view key, std::string_view* name)
{
    const usize slash = key.find('/');
    if (slash == std::string_view::npos) {
        return nullptr;
    }
    const std::string_view category = key.substr(0, slash);
    *name = key.substr(slash + 1);

    if (category == "sound" || category == "newsound") {
        return &index.sounds;
    }
    if (category == "streaming") {
        return &index.streaming;
    }
    if (category == "music" || category == "newmusic") {
        return &index.music;
    }
    return nullptr;
}

bool visit(void* context, const io::DirEntry& entry)
{
    Walk& walk = *static_cast<Walk*>(context);

    // The visitor runs inside the platform's open directory handle, so the
    // recursion has to save and restore the strings it appends to rather than
    // build new ones per level -- and it must not list a child directory while
    // the parent's handle is still being iterated on. Collecting the child
    // names first and recursing after is what the second pass below does.
    const usize keyLength = walk.key.size();
    const usize pathLength = walk.path.size();

    if (!walk.key.empty()) {
        walk.key += '/';
    }
    walk.key += entry.name;
    walk.path += '/';
    walk.path += entry.name;

    if (entry.isDirectory) {
        if (walk.depth + 1 < kMaxResourceDepth) {
            ++walk.depth;
            walkDirectory(walk);
            --walk.depth;
        }
    } else {
        std::string_view name;
        if (SoundPool* pool = poolForKey(*walk.out, walk.key, &name)) {
            if (walk.out->total() < kMaxResourceEntries) {
                pool->add(name, walk.path);
            } else {
                walk.stopped = true;
            }
        }
    }

    walk.key.resize(keyLength);
    walk.path.resize(pathLength);
    return !walk.stopped;
}

void walkDirectory(Walk& walk)
{
    // Two passes, and the reason is the 3DS: `listDirectory` holds an open FS
    // handle for the duration of the visitor, and recursing into a child from
    // inside it would nest handles arbitrarily deep on a service that has a
    // small fixed number of them. So the names are copied out first -- a
    // directory's worth of short strings -- and the recursion happens after
    // the handle is closed.
    struct Level {
        std::vector<io::DirEntry> entries;
        bool stopped = false;
    } level;

    walk.fs->listDirectory(walk.path.c_str(), &level, [](void* ctx, const io::DirEntry& e) {
        Level& out = *static_cast<Level*>(ctx);
        if (out.entries.size() >= kMaxResourceEntries) {
            out.stopped = true;
            return false;
        }
        out.entries.push_back(e);
        return true;
    });

    for (const io::DirEntry& entry : level.entries) {
        if (!visit(&walk, entry)) {
            break;
        }
    }
}

}  // namespace

bool indexResources(io::FileSystem& fs, std::string_view root, ResourceIndex* out)
{
    if (out == nullptr) {
        return false;
    }
    *out = ResourceIndex{};

    const std::string rootPath(root);
    if (!fs.isDirectory(rootPath.c_str())) {
        // The ordinary first-boot state: no folder, no sound, no complaint.
        return false;
    }

    Walk walk{&fs, out, rootPath, std::string(), rootPath, 0, false};
    walkDirectory(walk);
    return true;
}

}  // namespace mc::audio
