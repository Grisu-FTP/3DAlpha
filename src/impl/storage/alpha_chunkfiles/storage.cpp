#include "impl/storage/alpha_chunkfiles/storage.hpp"

#include "core/util/compress.hpp"
#include "impl/storage/alpha_chunkfiles/chunk_nbt.hpp"
#include "impl/storage/alpha_chunkfiles/level_dat.hpp"

#include <cstring>

namespace mc::alpha {
namespace {

using world::OpenResult;

constexpr char kLevelFile[] = "level.dat";
constexpr char kLockFile[] = "session.lock";

// session.lock is a single big-endian i64, written and read by hand rather than
// through the NBT layer: it is not NBT.
void writeBigEndian64(i64 value, u8* out)
{
    const u64 bits = u64(value);
    for (int i = 0; i < 8; ++i) {
        out[i] = u8(bits >> (56 - i * 8));
    }
}

i64 readBigEndian64(const u8* in)
{
    u64 bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8) | u64(in[i]);
    }
    return i64(bits);
}

// Appends "<worldDir>/<name>" into a ChunkPath buffer.
void joinPath(std::string_view dir, std::string_view name, ChunkPath* out)
{
    const usize total = dir.size() + 1 + name.size();
    if (total + 1 > kMaxChunkPathLength) {
        out->length = 0;
        out->text[0] = '\0';
        return;
    }
    std::memcpy(out->text, dir.data(), dir.size());
    out->text[dir.size()] = '/';
    std::memcpy(out->text + dir.size() + 1, name.data(), name.size());
    out->text[total] = '\0';
    out->length = total;
}

// Context for the two nested directory walks in forEachChunk.
struct ScanContext {
    io::FileSystem* fs;
    std::string_view worldDir;
    void* userContext;
    world::ChunkVisitor visit;
    bool stopped = false;
};

bool visitChunkFile(void* context, const io::DirEntry& entry)
{
    ScanContext& scan = *static_cast<ScanContext*>(context);
    if (entry.isDirectory) {
        return true;
    }

    i32 x = 0;
    i32 z = 0;
    if (!parseChunkFileName(entry.name, &x, &z)) {
        // A world folder holds whatever the user put there. Anything that is
        // not a chunk file name is simply not ours.
        return true;
    }

    if (!scan.visit(scan.userContext, x, z)) {
        scan.stopped = true;
        return false;
    }
    return true;
}

struct InnerDirContext {
    ScanContext* scan;
    ChunkPath outer;
};

bool visitInnerDir(void* context, const io::DirEntry& entry)
{
    InnerDirContext& inner = *static_cast<InnerDirContext*>(context);
    if (!entry.isDirectory) {
        return true;
    }

    ChunkPath path;
    joinPath(inner.outer.view(), entry.name, &path);
    if (path.length == 0) {
        return true;
    }

    inner.scan->fs->listDirectory(path.text, inner.scan, visitChunkFile);
    return !inner.scan->stopped;
}

bool visitOuterDir(void* context, const io::DirEntry& entry)
{
    ScanContext& scan = *static_cast<ScanContext*>(context);
    if (!entry.isDirectory) {
        return true;
    }

    InnerDirContext inner{&scan, {}};
    joinPath(scan.worldDir, entry.name, &inner.outer);
    if (inner.outer.length == 0) {
        return true;
    }

    // One directory is held open at a time on purpose: libctru's readdir caches
    // 32 entries of 552 bytes, so a nested walk that kept several open would
    // hold 17.7 KB per level. See docs/3ds-performance.md.
    scan.fs->listDirectory(inner.outer.text, &inner, visitInnerDir);
    return !scan.stopped;
}

}  // namespace

void AlphaChunkFileStorage::levelPath(ChunkPath* out) const
{
    joinPath(worldDir_, kLevelFile, out);
}

void AlphaChunkFileStorage::lockPath(ChunkPath* out) const
{
    joinPath(worldDir_, kLockFile, out);
}

bool AlphaChunkFileStorage::pathFor(i32 x, i32 z, ChunkPath* out) const
{
    return chunkFilePath(worldDir_, x, z, out);
}

bool AlphaChunkFileStorage::readAndInflate(const char* path, usize maxFile, usize maxNbt,
                                           std::vector<u8>* out)
{
    std::vector<u8> raw;
    if (!fs_.readFile(path, &raw, maxFile)) {
        return false;
    }
    out->clear();
    return zip::decompress(raw, *out, zip::Wrapper::Gzip, maxNbt);
}

bool AlphaChunkFileStorage::deflateAndWrite(const char* path, ConstByteSpan nbt)
{
    std::vector<u8> packed;
    if (!zip::compress(nbt, packed, zip::Wrapper::Gzip)) {
        return false;
    }
    return fs_.writeFileAtomic(path, packed);
}

bool AlphaChunkFileStorage::writeLock(i64 nowMillis)
{
    u8 bytes[kSessionLockBytes];
    writeBigEndian64(nowMillis, bytes);

    ChunkPath path;
    lockPath(&path);
    if (path.length == 0 || !fs_.writeFileAtomic(path.text, ConstByteSpan(bytes, sizeof(bytes)))) {
        return false;
    }
    lockValue_ = nowMillis;
    return true;
}

bool AlphaChunkFileStorage::refreshLock(i64 nowMillis)
{
    return open_ && writeLock(nowMillis);
}

bool AlphaChunkFileStorage::lockStillOurs()
{
    if (!open_) {
        return false;
    }

    ChunkPath path;
    lockPath(&path);
    if (path.length == 0) {
        return false;
    }

    std::vector<u8> bytes;
    if (!fs_.readFile(path.text, &bytes, kSessionLockBytes) ||
        bytes.size() != kSessionLockBytes) {
        return false;
    }
    return readBigEndian64(bytes.data()) == lockValue_;
}

OpenResult AlphaChunkFileStorage::open(std::string_view worldDir, i64 nowMillis)
{
    worldDir_.assign(worldDir);
    level_ = world::LevelData{};
    lastChunkDir_.clear();
    open_ = false;

    ChunkPath path;
    levelPath(&path);
    if (path.length == 0) {
        return OpenResult::IoError;
    }
    if (!fs_.exists(path.text)) {
        return OpenResult::NotAWorld;
    }

    std::vector<u8> nbt;
    if (!readAndInflate(path.text, kMaxLevelFileBytes, kMaxLevelNbtBytes, &nbt)) {
        return OpenResult::Corrupt;
    }
    if (!decodeLevelDat(nbt, &level_)) {
        return OpenResult::Corrupt;
    }

    if (!writeLock(nowMillis)) {
        return OpenResult::IoError;
    }
    open_ = true;

    // Claiming the lock and then reading it back is what detects a second
    // writer: if anything else is holding this world, the value we just wrote
    // will not be the one we read.
    if (!lockStillOurs()) {
        open_ = false;
        return OpenResult::Locked;
    }

    return OpenResult::Ok;
}

OpenResult AlphaChunkFileStorage::create(std::string_view worldDir, i64 seed, i64 nowMillis)
{
    worldDir_.assign(worldDir);
    level_ = world::LevelData{};
    lastChunkDir_.clear();
    open_ = false;

    ChunkPath path;
    levelPath(&path);
    if (path.length == 0) {
        return OpenResult::IoError;
    }
    if (fs_.exists(path.text)) {
        // Creating over an existing world would destroy it. The caller has to
        // decide to delete it first.
        return OpenResult::IoError;
    }

    if (!fs_.makeDirectories(worldDir_.c_str())) {
        return OpenResult::IoError;
    }

    level_.randomSeed = seed;
    level_.lastPlayed = nowMillis;
    open_ = true;

    if (!saveLevel() || !writeLock(nowMillis)) {
        open_ = false;
        return OpenResult::IoError;
    }
    return OpenResult::Ok;
}

bool AlphaChunkFileStorage::peekLevel(std::string_view worldDir, world::LevelData* out)
{
    ChunkPath path;
    joinPath(worldDir, kLevelFile, &path);
    if (path.length == 0 || !fs_.exists(path.text)) {
        return false;
    }

    std::vector<u8> nbt;
    if (!readAndInflate(path.text, kMaxLevelFileBytes, kMaxLevelNbtBytes, &nbt)) {
        return false;
    }

    // Into a local first: a half-decoded level is not something a caller should
    // be able to see, and *out may be an entry in a list that stays usable.
    world::LevelData level;
    if (!decodeLevelDat(nbt, &level)) {
        return false;
    }
    *out = std::move(level);
    return true;
}

bool AlphaChunkFileStorage::saveLevel()
{
    if (!open_) {
        return false;
    }

    std::vector<u8> nbt;
    if (!encodeLevelDat(level_, &nbt)) {
        return false;
    }

    ChunkPath path;
    levelPath(&path);
    return path.length != 0 && deflateAndWrite(path.text, nbt);
}

bool AlphaChunkFileStorage::close(i64 nowMillis)
{
    if (!open_) {
        return true;
    }
    level_.lastPlayed = nowMillis;
    const bool ok = saveLevel();
    open_ = false;
    lastChunkDir_.clear();
    return ok;
}

bool AlphaChunkFileStorage::hasChunk(i32 x, i32 z)
{
    ChunkPath path;
    return open_ && pathFor(x, z, &path) && fs_.exists(path.text);
}

bool AlphaChunkFileStorage::loadChunk(i32 x, i32 z, world::ChunkColumn* out)
{
    ChunkPath path;
    if (!open_ || !pathFor(x, z, &path)) {
        return false;
    }

    std::vector<u8> nbt;
    if (!readAndInflate(path.text, kMaxChunkFileBytes, kMaxChunkNbtBytes, &nbt)) {
        return false;
    }

    world::ChunkColumn chunk;
    if (!decodeChunk(nbt, &chunk)) {
        return false;
    }

    // The filename is authoritative. A file whose xPos/zPos disagree with where
    // it sits has been mangled by something, and loading it would put the wrong
    // terrain here and then save it back one folder further from correct.
    if (chunk.x != x || chunk.z != z) {
        return false;
    }

    *out = std::move(chunk);
    return true;
}

bool AlphaChunkFileStorage::ensureChunkDir(const ChunkPath& dir)
{
    if (lastChunkDir_ == dir.view()) {
        return true;
    }
    if (!fs_.makeDirectories(dir.text)) {
        return false;
    }
    lastChunkDir_.assign(dir.view());
    return true;
}

bool AlphaChunkFileStorage::saveChunk(const world::ChunkColumn& chunk)
{
    ChunkPath dir;
    ChunkPath path;
    if (!open_ || !chunkDirPath(worldDir_, chunk.x, chunk.z, &dir) ||
        !pathFor(chunk.x, chunk.z, &path)) {
        return false;
    }

    std::vector<u8> nbt;
    if (!encodeChunk(chunk, &nbt)) {
        return false;
    }

    return ensureChunkDir(dir) && deflateAndWrite(path.text, nbt);
}

bool AlphaChunkFileStorage::removeChunk(i32 x, i32 z)
{
    ChunkPath path;
    return open_ && pathFor(x, z, &path) && fs_.removeFile(path.text);
}

bool AlphaChunkFileStorage::forEachChunk(void* context, world::ChunkVisitor visit)
{
    if (!open_) {
        return false;
    }

    ScanContext scan{&fs_, worldDir_, context, visit, false};
    return fs_.listDirectory(worldDir_.c_str(), &scan, visitOuterDir);
}

}  // namespace mc::alpha
