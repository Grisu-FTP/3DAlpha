#include "core/world/any_storage.hpp"

namespace mc::world {

format::PackedStorage& AnyStorage::packed()
{
    if (packed_ == nullptr) {
        packed_.reset(new format::PackedStorage(fs_));
    }
    return *packed_;
}

OpenResult AnyStorage::open(std::string_view worldDir, i64 nowMillis)
{
    // The folder's shape is the only thing asked. Nothing is stored and nothing
    // is registered, so a world dropped onto the card from a PC opens on sight
    // and the mode cannot drift from what is actually there.
    format_ = detectFormat(fs_, worldDir);
    switch (format_) {
    case WorldFormat::Packed:
        return packed().open(worldDir, nowMillis);
    case WorldFormat::Folder:
        return folder_.open(worldDir, nowMillis);
    case WorldFormat::Unknown:
        break;
    }
    return OpenResult::NotAWorld;
}

OpenResult AnyStorage::create(std::string_view worldDir, i64 seed, i64 nowMillis,
                              WorldFormat format)
{
    format_ = format;
    switch (format_) {
    case WorldFormat::Packed:
        return packed().create(worldDir, seed, nowMillis);
    case WorldFormat::Folder:
        return folder_.create(worldDir, seed, nowMillis);
    case WorldFormat::Unknown:
        break;
    }
    return OpenResult::NotAWorld;
}

bool AnyStorage::peekLevel(std::string_view worldDir, LevelData* out)
{
    // Deliberately independent of what this object currently has open -- the
    // world list peeks every world on the card through one storage.
    switch (detectFormat(fs_, worldDir)) {
    case WorldFormat::Packed:
        return packed().peekLevel(worldDir, out);
    case WorldFormat::Folder:
        return folder_.peekLevel(worldDir, out);
    case WorldFormat::Unknown:
        break;
    }
    return false;
}

bool AnyStorage::readLevel(std::string_view worldDir, LevelData* out)
{
    switch (detectFormat(fs_, worldDir)) {
    case WorldFormat::Packed:
        return packed().readLevel(worldDir, out);
    case WorldFormat::Folder:
        // The folder backend's peek already reads and gunzips the whole file
        // -- there is no cheaper answer for it -- so the two are the same
        // call. Named separately anyway, because which fields a caller may
        // trust is the difference and it must not depend on the format.
        return folder_.peekLevel(worldDir, out);
    case WorldFormat::Unknown:
        break;
    }
    return false;
}

bool AnyStorage::close(i64 nowMillis)
{
    const bool ok = format_ == WorldFormat::Packed ? packed().close(nowMillis)
                                                   : folder_.close(nowMillis);
    format_ = WorldFormat::Unknown;
    return ok;
}

bool AnyStorage::isOpen() const
{
    if (format_ == WorldFormat::Packed) {
        return packed_ != nullptr && packed_->isOpen();
    }
    return folder_.isOpen();
}

std::string_view AnyStorage::worldDir() const
{
    if (format_ == WorldFormat::Packed) {
        return packed_ != nullptr ? packed_->worldDir() : std::string_view();
    }
    return folder_.worldDir();
}

LevelData& AnyStorage::level()
{
    return format_ == WorldFormat::Packed ? packed().level() : folder_.level();
}

const LevelData& AnyStorage::level() const
{
    if (format_ == WorldFormat::Packed && packed_ != nullptr) {
        return packed_->level();
    }
    return folder_.level();
}

bool AnyStorage::saveLevel()
{
    return format_ == WorldFormat::Packed ? packed().saveLevel() : folder_.saveLevel();
}

bool AnyStorage::hasChunk(i32 x, i32 z)
{
    return format_ == WorldFormat::Packed ? packed().hasChunk(x, z) : folder_.hasChunk(x, z);
}

bool AnyStorage::loadChunk(i32 x, i32 z, ChunkColumn* out)
{
    return format_ == WorldFormat::Packed ? packed().loadChunk(x, z, out)
                                          : folder_.loadChunk(x, z, out);
}

bool AnyStorage::saveChunk(const ChunkColumn& chunk)
{
    return format_ == WorldFormat::Packed ? packed().saveChunk(chunk) : folder_.saveChunk(chunk);
}

bool AnyStorage::removeChunk(i32 x, i32 z)
{
    return format_ == WorldFormat::Packed ? packed().removeChunk(x, z)
                                          : folder_.removeChunk(x, z);
}

bool AnyStorage::commit()
{
    return format_ == WorldFormat::Packed ? packed().commit() : folder_.commit();
}

bool AnyStorage::refreshLock(i64 nowMillis)
{
    return format_ == WorldFormat::Packed ? packed().refreshLock(nowMillis)
                                          : folder_.refreshLock(nowMillis);
}

bool AnyStorage::lockStillOurs()
{
    return format_ == WorldFormat::Packed ? packed().lockStillOurs() : folder_.lockStillOurs();
}

bool AnyStorage::forEachChunk(void* context, ChunkVisitor visit)
{
    return format_ == WorldFormat::Packed ? packed().forEachChunk(context, visit)
                                          : folder_.forEachChunk(context, visit);
}

u64 AnyStorage::chunkGroupKey(i32 x, i32 z) const
{
    if (format_ == WorldFormat::Packed && packed_ != nullptr) {
        return packed_->chunkGroupKey(x, z);
    }
    return folder_.chunkGroupKey(x, z);
}

bool AnyStorage::listChunkGroup(i32 x, i32 z, void* context, ChunkVisitor visit)
{
    return format_ == WorldFormat::Packed ? packed().listChunkGroup(x, z, context, visit)
                                          : folder_.listChunkGroup(x, z, context, visit);
}

}  // namespace mc::world
