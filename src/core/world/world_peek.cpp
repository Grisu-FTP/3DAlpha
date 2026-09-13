#include "core/world/world_peek.hpp"

#include "core/util/compress.hpp"

#include "version_slots.hpp"

#include <utility>

namespace mc::world {

namespace {

constexpr char kLevelFile[] = "level.dat";

std::string joinPath(std::string_view dir, const char* name)
{
    std::string path(dir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += name;
    return path;
}

}  // namespace

bool WorldPeek::open(std::string_view worldDir)
{
    close();
    worldDir_.assign(worldDir);
    format_ = detectFormat(fs_, worldDir_);
    level_ = LevelData{};

    switch (format_) {
    case WorldFormat::Folder:
        open_ = openFolder();
        break;
    case WorldFormat::Packed:
        open_ = openPacked();
        break;
    case WorldFormat::Unknown:
        open_ = false;
        break;
    }
    return open_;
}

void WorldPeek::close()
{
    // Destroying a RegionFile drops its handle and deliberately commits
    // nothing -- see its destructor -- and nothing here ever staged a write.
    for (Region& region : regions_) {
        region = Region{};
    }
    open_ = false;
    format_ = WorldFormat::Unknown;
    raw_.clear();
    inflated_.clear();
}

bool WorldPeek::openFolder()
{
    const std::string path = joinPath(worldDir_, kLevelFile);
    raw_.clear();
    if (!fs_.readFile(path.c_str(), &raw_, mcver::LevelCodec::kMaxPayloadBytes)) {
        return false;
    }
    inflated_.clear();
    if (!zip::decompress(raw_, inflated_, mcver::LevelCodec::kWrapper,
                         mcver::LevelCodec::kMaxDecodedBytes)) {
        return false;
    }
    return mcver::LevelCodec::decode(inflated_, &level_);
}

bool WorldPeek::openPacked()
{
    const std::string path = joinPath(worldDir_, format::kManifestName);
    format::Manifest manifest;
    if (!manifest.load(fs_, path.c_str())) {
        return false;
    }
    ConstByteSpan stored;
    if (!manifest.file(kLevelFile, &stored)) {
        return false;
    }
    inflated_.clear();
    if (!zip::decompress(stored, inflated_, mcver::LevelCodec::kWrapper,
                         mcver::LevelCodec::kMaxDecodedBytes)) {
        return false;
    }
    return mcver::LevelCodec::decode(inflated_, &level_);
}

PeekRead WorldPeek::loadChunk(i32 x, i32 z, ChunkColumn* out)
{
    if (!open_ || out == nullptr) {
        return PeekRead::Failed;
    }
    return format_ == WorldFormat::Packed ? loadPackedChunk(x, z, out)
                                          : loadFolderChunk(x, z, out);
}

PeekRead WorldPeek::loadFolderChunk(i32 x, i32 z, ChunkColumn* out)
{
    mcver::ChunkLayout::Path path;
    if (!mcver::ChunkLayout::filePath(worldDir_, x, z, &path) || path.length == 0) {
        return PeekRead::Failed;
    }
    if (!fs_.exists(path.text)) {
        return PeekRead::Absent;
    }
    raw_.clear();
    if (!fs_.readFile(path.text, &raw_, mcver::ChunkCodec::kMaxPayloadBytes)) {
        return PeekRead::Failed;
    }
    inflated_.clear();
    if (!zip::decompress(raw_, inflated_, mcver::ChunkCodec::kWrapper,
                         mcver::ChunkCodec::kMaxDecodedBytes)) {
        return PeekRead::Failed;
    }
    ChunkColumn chunk(x, z);
    if (!mcver::ChunkCodec::decode(inflated_, &chunk)) {
        return PeekRead::Failed;
    }
    // The file name is authoritative, as it is for the storage: a file whose
    // own coordinates disagree with where it sits is not this chunk.
    if (chunk.x != x || chunk.z != z) {
        return PeekRead::Failed;
    }
    *out = std::move(chunk);
    return PeekRead::Ok;
}

PeekRead WorldPeek::loadPackedChunk(i32 x, i32 z, ChunkColumn* out)
{
    Region* region = regionFor(x, z);
    if (region == nullptr) {
        return PeekRead::Failed;
    }
    if (region->absent || !region->file->has(x, z)) {
        return PeekRead::Absent;
    }
    raw_.clear();
    if (!region->file->read(x, z, &raw_)) {
        return PeekRead::Failed;
    }
    inflated_.clear();
    if (!zip::decompress(raw_, inflated_, mcver::ChunkCodec::kWrapper,
                         mcver::ChunkCodec::kMaxDecodedBytes)) {
        return PeekRead::Failed;
    }
    ChunkColumn chunk(x, z);
    if (!mcver::ChunkCodec::decode(inflated_, &chunk)) {
        return PeekRead::Failed;
    }
    chunk.x = x;
    chunk.z = z;
    *out = std::move(chunk);
    return PeekRead::Ok;
}

namespace {

// What travels through RegionFile::readMany's void*: the peek doing the
// inflating and the caller's own visitor waiting on the other side.
struct BatchContext {
    WorldPeek* peek = nullptr;
    void* context = nullptr;
    WorldPeek::PeekVisitor visit = nullptr;
    bool stopped = false;
};

}  // namespace

bool WorldPeek::decodeOne(void* context, i32 chunkX, i32 chunkZ, ConstByteSpan payload)
{
    BatchContext& batch = *static_cast<BatchContext*>(context);
    WorldPeek& self = *batch.peek;

    // An empty payload is a chunk the region does not hold, and a damaged one is
    // one chunk: both are reported as null so the caller's count still comes
    // out, and neither ends the batch.
    ChunkColumn column(chunkX, chunkZ);
    bool ok = !payload.empty();
    if (ok) {
        self.inflated_.clear();
        ok = zip::decompress(payload, self.inflated_, mcver::ChunkCodec::kWrapper,
                             mcver::ChunkCodec::kMaxDecodedBytes)
             && mcver::ChunkCodec::decode(self.inflated_, &column);
    }
    if (ok) {
        column.x = chunkX;
        column.z = chunkZ;
    }
    if (!batch.visit(batch.context, chunkX, chunkZ, ok ? &column : nullptr)) {
        batch.stopped = true;
        return false;
    }
    return true;
}

bool WorldPeek::loadChunks(const i32* xs, const i32* zs, usize count, void* context,
                           PeekVisitor visit)
{
    if (!open_ || visit == nullptr || (count != 0 && (xs == nullptr || zs == nullptr))) {
        return false;
    }

    // **A folder world has nothing to coalesce.** Its cost is the open and the
    // close, one pair per chunk, and no ordering of the requests removes one of
    // them. So this is the same loop the caller would have written, kept here
    // only so the caller does not have to know which format it is looking at.
    if (format_ != WorldFormat::Packed) {
        ChunkColumn column;
        for (usize i = 0; i < count; ++i) {
            const bool ok = loadFolderChunk(xs[i], zs[i], &column) == PeekRead::Ok;
            if (!visit(context, xs[i], zs[i], ok ? &column : nullptr)) {
                return true;
            }
        }
        return true;
    }

    // Grouped by region, because a batch read is a property of one container.
    // A linear sweep per group rather than a sort: the window a caller asks for
    // spans four regions at the very most, and this way the request arrays stay
    // the caller's and are never reordered.
    grouped_.assign(count, 0);
    for (usize i = 0; i < count; ++i) {
        if (grouped_[i] != 0) {
            continue;
        }
        const i32 rx = format::regionCoord(xs[i]);
        const i32 rz = format::regionCoord(zs[i]);
        groupX_.clear();
        groupZ_.clear();
        for (usize j = i; j < count; ++j) {
            if (grouped_[j] != 0 || format::regionCoord(xs[j]) != rx
                || format::regionCoord(zs[j]) != rz) {
                continue;
            }
            grouped_[j] = 1;
            groupX_.push_back(xs[j]);
            groupZ_.push_back(zs[j]);
        }

        Region* region = regionFor(xs[i], zs[i]);
        if (region == nullptr) {
            return false;
        }
        if (region->absent) {
            // An unexplored corner. Every chunk of it is absent, and the caller
            // is owed an answer for each: a group that is nothing but this must
            // still be able to finish.
            bool stopped = false;
            for (usize j = 0; j < groupX_.size() && !stopped; ++j) {
                stopped = !visit(context, groupX_[j], groupZ_[j], nullptr);
            }
            if (stopped) {
                return true;
            }
            continue;
        }
        BatchContext batch;
        batch.peek = this;
        batch.context = context;
        batch.visit = visit;
        if (!region->file->readMany(groupX_.data(), groupZ_.data(), groupX_.size(), &batch_,
                                    &batch, &decodeOne)) {
            return false;
        }
        if (batch.stopped) {
            return true;
        }
    }
    return true;
}

WorldPeek::Region* WorldPeek::regionFor(i32 chunkX, i32 chunkZ)
{
    const i32 rx = format::regionCoord(chunkX);
    const i32 rz = format::regionCoord(chunkZ);

    for (Region& region : regions_) {
        if (region.known && region.x == rx && region.z == rz) {
            region.used = ++clock_;
            return &region;
        }
    }

    Region* victim = &regions_[0];
    for (Region& region : regions_) {
        if (!region.known) {
            victim = &region;
            break;
        }
        if (region.used < victim->used) {
            victim = &region;
        }
    }
    *victim = Region{};
    victim->x = rx;
    victim->z = rz;
    victim->used = ++clock_;

    const std::string path = format::regionPath(worldDir_, rx, rz);
    usize bytes = 0;
    if (!fs_.exists(path.c_str()) || !fs_.fileSize(path.c_str(), &bytes) || bytes == 0) {
        // Missing is an unexplored corner. Empty is a region somebody created
        // and never committed to, which holds no chunks -- and opening it would
        // write a header into it, which is exactly what this class must not do.
        victim->known = true;
        victim->absent = true;
        return victim;
    }

    victim->file.reset(new format::RegionFile());
    if (!victim->file->open(fs_, path.c_str(), rx, rz, /*create=*/false)) {
        victim->file.reset();
        return nullptr;
    }
    victim->known = true;
    return victim;
}

}  // namespace mc::world
