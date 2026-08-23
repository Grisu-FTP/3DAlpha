#include "core/world/chunk.hpp"

#include <cstring>

namespace mc::world {

void ChunkColumn::compact()
{
    for (Section& s : sections_) {
        s.compact();
    }
}

ChunkColumn ChunkColumn::clone() const
{
    ChunkColumn copy(x, z);
    copy.terrainPopulated = terrainPopulated;
    copy.lastUpdate = lastUpdate;
    std::memcpy(copy.heightMap, heightMap, sizeof(heightMap));
    copy.preserved = preserved;
    copy.preservedRoot = preservedRoot;
    for (int sy = 0; sy < kSectionCount; ++sy) {
        copy.sections_[sy] = sections_[sy].clone();
    }
    return copy;
}

usize ChunkColumn::memoryUsage() const
{
    usize bytes = sizeof(ChunkColumn);
    for (const Section& s : sections_) {
        bytes += s.memoryUsage();
    }
    return bytes + preserved.memoryUsage() + preservedRoot.memoryUsage();
}

}  // namespace mc::world
