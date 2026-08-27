#pragma once

// The version's payload codecs, named so a storage backend that is *not* this
// slot can still speak this version's chunk and level bytes.
//
// The packed container is version-agnostic on purpose -- it moves opaque
// payloads and knows nothing about NBT, gzip or world height, which is what
// would let it back a real McRegion later. But *playing* a packed world means
// turning a payload into a ChunkColumn, and that is this version's business.
// Binding these beside `Storage` in slot.hpp is how core reaches them without
// core naming an implementation: `mcver::ChunkCodec` is an alias the generated
// version_slots.hpp supplies, exactly like `mcver::Storage` is.
//
// The wrapper constants are load-bearing for the "stored verbatim" promise.
// The packed backend unwraps a payload to play it and re-wraps to save it, but
// a **conversion never touches the wrapper at all** -- the source file's bytes
// go in and come back out unchanged, which is what makes an exact restoration
// provable rather than hopeful.

#include "core/util/compress.hpp"
#include "impl/storage/alpha_chunkfiles/chunk_nbt.hpp"
#include "impl/storage/alpha_chunkfiles/chunk_path.hpp"
#include "impl/storage/alpha_chunkfiles/level_dat.hpp"
#include "impl/storage/alpha_chunkfiles/storage.hpp"

namespace mc::alpha {

struct AlphaChunkCodec {
    // What an Alpha chunk file carries on disk.
    static constexpr zip::Wrapper kWrapper = zip::Wrapper::Gzip;
    static constexpr usize kMaxPayloadBytes = kMaxChunkFileBytes;
    static constexpr usize kMaxDecodedBytes = kMaxChunkNbtBytes;

    static bool decode(ConstByteSpan nbt, world::ChunkColumn* out)
    {
        return decodeChunk(nbt, out);
    }

    static bool encode(const world::ChunkColumn& chunk, std::vector<u8>* out)
    {
        return encodeChunk(chunk, out);
    }
};

// Where this format puts a chunk, and how to tell one of its files apart from
// somebody else's. The converter needs both directions -- packing has to know
// which files are chunks, unpacking has to put them back where a real client
// will look -- and neither is core's business to know.
struct AlphaChunkLayout {
    using Path = ChunkPath;

    static bool filePath(std::string_view worldDir, i32 x, i32 z, Path* out)
    {
        return chunkFilePath(worldDir, x, z, out);
    }

    static bool directoryPath(std::string_view worldDir, i32 x, i32 z, Path* out)
    {
        return chunkDirPath(worldDir, x, z, out);
    }

    static bool parseFileName(std::string_view fileName, i32* x, i32* z)
    {
        return parseChunkFileName(fileName, x, z);
    }
};

struct AlphaLevelCodec {
    static constexpr zip::Wrapper kWrapper = zip::Wrapper::Gzip;
    static constexpr usize kMaxPayloadBytes = kMaxLevelFileBytes;
    static constexpr usize kMaxDecodedBytes = kMaxLevelNbtBytes;

    static bool decode(ConstByteSpan nbt, world::LevelData* out)
    {
        return decodeLevelDat(nbt, out);
    }

    static bool encode(const world::LevelData& level, std::vector<u8>* out)
    {
        return encodeLevelDat(level, out);
    }
};

}  // namespace mc::alpha
