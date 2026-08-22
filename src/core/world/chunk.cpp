#include "core/world/chunk.hpp"

namespace mc::world {

void ChunkColumn::compact()
{
    for (Section& s : sections_) {
        s.compact();
    }
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
