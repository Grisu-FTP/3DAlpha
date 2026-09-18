#include "core/world/world_transfer.hpp"

#include "core/world/world_format.hpp"
#include "core/world/world_list.hpp"

#include <algorithm>

namespace mc::world {

namespace {

// The same bound `removeTree` uses, for the same reason: it stops a directory
// tree that loops back on itself -- which a card mounted on a PC can be made to
// have -- from recursing forever.
constexpr int kMaxWalkDepth = 8;

// Gathered inside the visitor and acted on after it returns, because on the
// console a file operation inside a directory walk is one IPC round trip nested
// inside another's iterator. The same shape as world_list.cpp's walks.
struct Children {
    std::vector<std::pair<std::string, bool>> entries;  // name, isDirectory
};

bool collectChildren(void* context, const io::DirEntry& entry)
{
    static_cast<Children*>(context)->entries.emplace_back(entry.name, entry.isDirectory);
    return true;
}

bool walk(io::FileSystem& fs, const std::string& dir, const std::string& prefix,
          TransferManifest* out, int depth)
{
    if (depth > kMaxWalkDepth) {
        return false;
    }
    Children found;
    if (!fs.listDirectory(dir.c_str(), &found, collectChildren)) {
        return false;
    }

    for (const auto& entry : found.entries) {
        const std::string child = dir + "/" + entry.first;
        const std::string relative = prefix.empty() ? entry.first : prefix + "/" + entry.first;
        if (entry.second) {
            if (!walk(fs, child, relative, out, depth + 1)) {
                return false;
            }
            continue;
        }

        // A name the far end could not be told about is a reason to refuse the
        // whole world rather than to send it short: a world that arrived
        // missing a file is worse than one that never started.
        if (!safeRelativePath(relative)) {
            return false;
        }

        usize bytes = 0;
        if (!fs.fileSize(child.c_str(), &bytes)) {
            return false;
        }
        if (u64(bytes) > kMaxTransferFileBytes) {
            return false;
        }
        out->totalBytes += u64(bytes);
        if (out->totalBytes > kMaxTransferBytes
            || out->files.size() >= usize(kMaxTransferFiles)) {
            return false;
        }
        out->files.push_back(TransferFile{relative, u64(bytes)});
    }
    return true;
}

}  // namespace

bool safeRelativePath(std::string_view path)
{
    if (path.empty() || path.size() > kMaxTransferPath) {
        return false;
    }
    if (path.front() == '/' || path.back() == '/') {
        return false;
    }

    usize start = 0;
    while (start <= path.size()) {
        const usize slash = path.find('/', start);
        const usize end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(start, end - start);

        // An empty component is "a//b", which a card would resolve to "a/b" --
        // and a path that means something different after resolution is not one
        // to write from.
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        for (const char c : part) {
            const unsigned char byte = static_cast<unsigned char>(c);
            // Backslash is not a separator here, but it is one on the PC the
            // card gets read on, so it is refused rather than written through.
            if (byte < 0x20 || byte == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?'
                || c == '"' || c == '<' || c == '>' || c == '|') {
                return false;
            }
        }
        // "foo." and "foo " are names Windows will not create; a card is read on
        // a PC as often as on a console.
        if (part.back() == '.' || part.back() == ' ') {
            return false;
        }

        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool buildTransferManifest(io::FileSystem& fs, std::string_view worldDir,
                           TransferManifest* out)
{
    *out = TransferManifest();

    const std::string dir(worldDir);
    // A folder that is not a world is not something to offer under a world's
    // name, and this is the same guard `copyWorld` and `deleteWorld` use.
    if (detectFormat(fs, dir) == WorldFormat::Unknown) {
        return false;
    }
    if (!walk(fs, dir, std::string(), out, 0)) {
        *out = TransferManifest();
        return false;
    }
    if (out->files.empty()) {
        return false;
    }

    // Sorted so the order is the world's and not readdir's: two consoles
    // transferring the same world send the same bytes in the same order, which
    // is what makes a transfer something a test can reproduce.
    std::sort(out->files.begin(), out->files.end(),
              [](const TransferFile& a, const TransferFile& b) { return a.path < b.path; });
    return true;
}

ImportStaging::ImportStaging(io::FileSystem& fs, std::string_view savesDir)
    : fs_(fs), savesDir_(savesDir)
{
    staging_ = savesDir_ + "/" + kImportStagingName;
}

bool ImportStaging::begin()
{
    discard();
    bytesWritten_ = 0;
    filesWritten_ = 0;
    return fs_.makeDirectories(staging_.c_str());
}

bool ImportStaging::beginFile(std::string_view path, u64 size)
{
    // Checked before anything is touched. Everything below this line writes,
    // and this is the line that says where.
    if (!safeRelativePath(path) || size > kMaxTransferFileBytes) {
        return false;
    }
    if (open_) {
        return false;
    }

    const std::string full = staging_ + "/" + std::string(path);
    const usize slash = full.rfind('/');
    if (slash != std::string::npos) {
        if (!fs_.makeDirectories(full.substr(0, slash).c_str())) {
            return false;
        }
    }

    // Positional rather than whole-file: a region container is megabytes and
    // buffering one before it touches the card would put it on a heap this
    // console does not have to spare.
    file_ = fs_.openRandomAccess(full.c_str(), true);
    if (file_ == nullptr) {
        return false;
    }
    declared_ = size;
    received_ = 0;
    open_ = true;
    return true;
}

bool ImportStaging::writeChunk(const u8* data, usize size)
{
    if (!open_) {
        return false;
    }
    if (u64(size) > declared_ - received_) {
        return false;
    }
    if (size != 0 && !file_->writeAt(received_, ConstByteSpan(data, size))) {
        return false;
    }
    received_ += u64(size);
    bytesWritten_ += u64(size);
    return true;
}

bool ImportStaging::endFile()
{
    if (!open_) {
        return false;
    }
    const bool whole = received_ == declared_;
    // Flushed rather than merely closed: the world is committed with a rename a
    // moment later, and a rename that beats the payload to the card is exactly
    // the crash that leaves a world-shaped hole.
    const bool flushed = file_->flush();
    file_.reset();
    open_ = false;
    if (!whole || !flushed) {
        return false;
    }
    ++filesWritten_;
    return true;
}

bool ImportStaging::commit(std::string_view name)
{
    if (open_) {
        return false;
    }
    // What arrived has to be a world before it is given a world's name. A
    // sender that sent a folder of nothing in particular produces a refusal
    // here, not a row on the list that cannot be opened.
    if (detectFormat(fs_, staging_) == WorldFormat::Unknown) {
        return false;
    }

    const std::string target = worldPath(savesDir_, name);
    // Never merge into something already there. The menu checks the name
    // against the world list before asking, but the card is also editable on a
    // PC and this is the check that cannot be raced.
    if (fs_.exists(target.c_str())) {
        return false;
    }
    return fs_.rename(staging_.c_str(), target.c_str());
}

void ImportStaging::discard()
{
    if (open_) {
        file_.reset();
        open_ = false;
    }
    if (fs_.exists(staging_.c_str())) {
        removeTree(fs_, staging_);
    }
}

void discardStagedImport(io::FileSystem& fs, std::string_view savesDir)
{
    const std::string staging = std::string(savesDir) + "/" + kImportStagingName;
    if (fs.exists(staging.c_str())) {
        removeTree(fs, staging);
    }
}

}  // namespace mc::world
