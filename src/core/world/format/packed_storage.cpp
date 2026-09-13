#include "core/world/format/packed_storage.hpp"

#include "core/util/compress.hpp"

#include "version_slots.hpp"

#include <cstdio>
#include <utility>

namespace mc::world::format {

namespace {

// The same eight big-endian bytes the folder format writes, so a world keeps
// one lock story across a conversion.
constexpr usize kSessionLockBytes = 8;
constexpr char kLevelBlobPath[] = "level.dat";

void writeBigEndian64(i64 value, u8* out)
{
    const u64 bits = u64(value);
    for (int i = 0; i < 8; ++i) {
        out[i] = u8(bits >> (56 - 8 * i));
    }
}

i64 readBigEndian64(const u8* bytes)
{
    u64 bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8) | u64(bytes[i]);
    }
    return i64(bits);
}

}  // namespace

PackedStorage::~PackedStorage()
{
    closeRegions();
}

std::string PackedStorage::pathFor(const char* name) const
{
    std::string path = worldDir_;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += name;
    return path;
}

OpenResult PackedStorage::open(std::string_view worldDir, i64 nowMillis)
{
    worldDir_.assign(worldDir);
    open_ = false;
    manifest_.clear();

    const std::string manifestPath = pathFor(kManifestName);
    if (!fs_.exists(manifestPath.c_str())) {
        return OpenResult::NotAWorld;
    }
    if (!manifest_.load(fs_, manifestPath.c_str())) {
        return OpenResult::Corrupt;
    }

    // level.dat lives in the manifest's blob rather than on disk -- see
    // manifest.hpp for why leaving it on disk would be dangerous.
    ConstByteSpan stored;
    if (!manifest_.file(kLevelBlobPath, &stored)) {
        return OpenResult::Corrupt;
    }
    scratch_.clear();
    if (!zip::decompress(stored, scratch_, mcver::LevelCodec::kWrapper,
                         mcver::LevelCodec::kMaxDecodedBytes)) {
        return OpenResult::Corrupt;
    }
    if (!mcver::LevelCodec::decode(scratch_, &level_)) {
        return OpenResult::Corrupt;
    }

    open_ = true;
    if (!writeLock(nowMillis)) {
        open_ = false;
        return OpenResult::IoError;
    }
    if (!lockStillOurs()) {
        open_ = false;
        return OpenResult::Locked;
    }
    return OpenResult::Ok;
}

OpenResult PackedStorage::create(std::string_view worldDir, i64 seed, i64 nowMillis)
{
    worldDir_.assign(worldDir);
    open_ = false;
    manifest_.clear();

    const std::string manifestPath = pathFor(kManifestName);
    // Refuses rather than overwriting, exactly as the folder backend refuses a
    // directory that already holds a level.dat.
    if (fs_.exists(manifestPath.c_str())) {
        return OpenResult::IoError;
    }
    if (!fs_.makeDirectories(worldDir_.c_str())) {
        return OpenResult::IoError;
    }

    level_ = LevelData();
    level_.randomSeed = seed;
    level_.lastPlayed = nowMillis;

    open_ = true;
    if (!saveLevel() || !writeLock(nowMillis)) {
        open_ = false;
        return OpenResult::IoError;
    }
    return OpenResult::Ok;
}

bool PackedStorage::peekLevel(std::string_view worldDir, LevelData* out)
{
    // **Does not touch this object's state**, so it may be called on a storage
    // already open on another world -- the world list does exactly that.
    std::string path(worldDir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += kManifestName;

    ManifestSummary summary;
    if (!Manifest::peek(fs_, path.c_str(), &summary)) {
        return false;
    }
    // Only what the list draws. Reading the whole level would mean loading the
    // blob and inflating it, which is the cost this exists to avoid.
    *out = LevelData();
    out->lastPlayed = summary.lastPlayed;
    out->randomSeed = summary.randomSeed;
    return true;
}

bool PackedStorage::readLevel(std::string_view worldDir, LevelData* out)
{
    // Into a local manifest and a local buffer, so this object's open world --
    // if it has one -- is untouched, the same promise peekLevel makes.
    std::string path(worldDir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += kManifestName;

    Manifest manifest;
    if (!manifest.load(fs_, path.c_str())) {
        return false;
    }
    ConstByteSpan stored;
    if (!manifest.file(kLevelBlobPath, &stored)) {
        return false;
    }

    std::vector<u8> plain;
    if (!zip::decompress(stored, plain, mcver::LevelCodec::kWrapper,
                         mcver::LevelCodec::kMaxDecodedBytes)) {
        return false;
    }

    LevelData level;
    if (!mcver::LevelCodec::decode(plain, &level)) {
        return false;
    }
    *out = std::move(level);
    return true;
}

bool PackedStorage::saveLevel()
{
    if (!open_) {
        return false;
    }
    scratch_.clear();
    if (!mcver::LevelCodec::encode(level_, &scratch_)) {
        return false;
    }
    payload_.clear();
    if (!zip::compress(scratch_, payload_, mcver::LevelCodec::kWrapper)) {
        return false;
    }
    if (!manifest_.replaceFile(kLevelBlobPath, payload_)) {
        return false;
    }

    u32 chunks = 0;
    u32 regions = 0;
    for (const Slot& slot : regions_) {
        if (slot.region != nullptr && slot.region->isOpen()) {
            chunks += slot.region->chunkCount();
            ++regions;
        }
    }
    // Only the regions currently open can be counted cheaply, so a count that
    // is already on record wins: it came from a conversion or a previous close
    // and covers the whole world, where this covers what is loaded.
    if (chunks > manifest_.summary.chunkCount) {
        manifest_.summary.chunkCount = chunks;
    }
    if (regions > manifest_.summary.regionCount) {
        manifest_.summary.regionCount = regions;
    }
    manifest_.summary.lastPlayed = level_.lastPlayed;
    manifest_.summary.randomSeed = level_.randomSeed;

    return manifest_.save(fs_, pathFor(kManifestName).c_str());
}

bool PackedStorage::close(i64 nowMillis)
{
    if (!open_) {
        return false;
    }
    level_.lastPlayed = nowMillis;
    const bool committed = closeRegions();
    const bool saved = saveLevel();
    open_ = false;
    worldDir_.clear();
    return committed && saved;
}

RegionFile* PackedStorage::regionFor(i32 chunkX, i32 chunkZ, bool create)
{
    const i32 rx = regionCoord(chunkX);
    const i32 rz = regionCoord(chunkZ);

    for (Slot& slot : regions_) {
        if (slot.region != nullptr && slot.region->isOpen() && slot.x == rx && slot.z == rz) {
            slot.used = ++clock_;
            return slot.region.get();
        }
    }

    // An empty slot if there is one, otherwise the least recently used.
    Slot* victim = &regions_[0];
    for (Slot& slot : regions_) {
        if (slot.region == nullptr || !slot.region->isOpen()) {
            victim = &slot;
            break;
        }
        if (slot.used < victim->used) {
            victim = &slot;
        }
    }

    if (victim->region != nullptr && victim->region->isOpen()) {
        // Evicting commits: dropping a region with staged writes would lose
        // exactly the columns the player just walked away from.
        victim->region->close();
    }
    if (victim->region == nullptr) {
        victim->region.reset(new RegionFile());
    }

    const std::string path = regionPath(worldDir_, rx, rz);
    if (!victim->region->open(fs_, path.c_str(), rx, rz, create)) {
        return nullptr;
    }
    victim->x = rx;
    victim->z = rz;
    victim->used = ++clock_;
    return victim->region.get();
}

bool PackedStorage::closeRegions()
{
    bool ok = true;
    for (Slot& slot : regions_) {
        if (slot.region != nullptr && slot.region->isOpen()) {
            ok = slot.region->close() && ok;
        }
        slot.region.reset();
    }
    return ok;
}

bool PackedStorage::hasChunk(i32 x, i32 z)
{
    if (!open_) {
        return false;
    }
    RegionFile* region = regionFor(x, z, false);
    return region != nullptr && region->has(x, z);
}

bool PackedStorage::loadChunk(i32 x, i32 z, ChunkColumn* out)
{
    if (!open_) {
        return false;
    }
    RegionFile* region = regionFor(x, z, false);
    if (region == nullptr) {
        return false;
    }
    payload_.clear();
    if (!region->read(x, z, &payload_)) {
        return false;
    }
    scratch_.clear();
    if (!zip::decompress(payload_, scratch_, mcver::ChunkCodec::kWrapper,
                         mcver::ChunkCodec::kMaxDecodedBytes)) {
        return false;
    }

    ChunkColumn loaded(x, z);
    if (!mcver::ChunkCodec::decode(scratch_, &loaded)) {
        return false;
    }
    // The coordinate the caller asked for wins over the one inside the payload,
    // the same way the folder backend treats the file name as authoritative:
    // where the chunk *is* is a fact about the container, not about the bytes.
    loaded.x = x;
    loaded.z = z;
    *out = std::move(loaded);
    return true;
}

bool PackedStorage::saveChunk(const ChunkColumn& chunk)
{
    if (!open_) {
        return false;
    }
    scratch_.clear();
    if (!mcver::ChunkCodec::encode(chunk, &scratch_)) {
        return false;
    }
    payload_.clear();
    if (!zip::compress(scratch_, payload_, mcver::ChunkCodec::kWrapper)) {
        return false;
    }
    RegionFile* region = regionFor(chunk.x, chunk.z, true);
    if (region == nullptr) {
        return false;
    }
    return region->write(chunk.x, chunk.z, payload_);
}

bool PackedStorage::removeChunk(i32 x, i32 z)
{
    if (!open_) {
        return false;
    }
    RegionFile* region = regionFor(x, z, false);
    if (region == nullptr) {
        return true;  // no region, so nothing to remove
    }
    return region->erase(x, z);
}

bool PackedStorage::commit()
{
    if (!open_) {
        return false;
    }
    bool ok = true;
    for (Slot& slot : regions_) {
        if (slot.region != nullptr && slot.region->isOpen()) {
            ok = slot.region->commit() && ok;
        }
    }
    return ok;
}

bool PackedStorage::writeLock(i64 nowMillis)
{
    u8 bytes[kSessionLockBytes];
    writeBigEndian64(nowMillis, bytes);
    if (!fs_.writeFileAtomic(pathFor("session.lock").c_str(),
                             ConstByteSpan(bytes, sizeof(bytes)))) {
        return false;
    }
    lockValue_ = nowMillis;
    return true;
}

bool PackedStorage::refreshLock(i64 nowMillis)
{
    return open_ && writeLock(nowMillis);
}

bool PackedStorage::lockStillOurs()
{
    if (!open_) {
        return false;
    }
    std::vector<u8> bytes;
    if (!fs_.readFile(pathFor("session.lock").c_str(), &bytes, kSessionLockBytes)
        || bytes.size() != kSessionLockBytes) {
        return false;
    }
    return readBigEndian64(bytes.data()) == lockValue_;
}

namespace {

struct WalkContext {
    void* context = nullptr;
    ChunkVisitor visit = nullptr;
    bool stopped = false;
};

bool walkRegion(void* context, i32 x, i32 z)
{
    auto& walk = *static_cast<WalkContext*>(context);
    if (!walk.visit(walk.context, x, z)) {
        walk.stopped = true;
        return false;
    }
    return true;
}

struct ScanContext {
    std::vector<std::pair<i32, i32>> regions;
};

bool collectRegionFile(void* context, const io::DirEntry& entry)
{
    if (entry.isDirectory) {
        return true;
    }
    int rx = 0;
    int rz = 0;
    // `r.<rx>.<rz>.3dr`, and nothing else. sscanf's %n tells us the whole name
    // was consumed, so `r.0.0.3dr.bak` is not mistaken for a region.
    int consumed = 0;
    if (std::sscanf(entry.name, "r.%d.%d.3dr%n", &rx, &rz, &consumed) == 2
        && entry.name[consumed] == '\0') {
        static_cast<ScanContext*>(context)->regions.emplace_back(i32(rx), i32(rz));
    }
    return true;
}

}  // namespace

bool PackedStorage::forEachChunk(void* context, ChunkVisitor visit)
{
    if (!open_) {
        return false;
    }
    // Names are collected inside the visitor and the files opened after it
    // returns: on the console a file operation nested inside a directory walk
    // is one IPC round trip inside another's iterator.
    ScanContext scan;
    if (!fs_.listDirectory(worldDir_.c_str(), &scan, collectRegionFile)) {
        return false;
    }

    WalkContext walk;
    walk.context = context;
    walk.visit = visit;
    for (const std::pair<i32, i32>& coords : scan.regions) {
        RegionFile* region = regionFor(coords.first * kRegionChunks,
                                       coords.second * kRegionChunks, false);
        if (region == nullptr) {
            continue;
        }
        region->forEachChunk(&walk, walkRegion);
        if (walk.stopped) {
            break;
        }
    }
    return true;
}

u64 PackedStorage::chunkGroupKey(i32 x, i32 z) const
{
    // A whole region is one group, which is what makes an existence check cost
    // one directory read for 1,024 chunks. **Both halves are 32 bits**: region
    // coordinates reach +/-62,500 in a legal a1.1.2 world, and a narrower key
    // would collide inside it -- which would make the cache answer "absent" for
    // a chunk that exists, and generate new terrain over it.
    return (u64(u32(regionCoord(x))) << 32) | u64(u32(regionCoord(z)));
}

bool PackedStorage::listChunkGroup(i32 x, i32 z, void* context, ChunkVisitor visit)
{
    if (!open_) {
        return false;
    }
    RegionFile* region = regionFor(x, z, false);
    if (region == nullptr) {
        // No region file: an unexplored corner of the world. An empty group is
        // an answer, not a failure -- the same contract the folder backend has
        // for a leaf directory that does not exist.
        return true;
    }
    WalkContext walk;
    walk.context = context;
    walk.visit = visit;
    return region->forEachChunk(&walk, walkRegion);
}

}  // namespace mc::world::format
