#include "core/map/map_sample.hpp"

#include "core/block/registry.hpp"

namespace mc::map {

bool showsOnMap(block::BlockId id)
{
    if (block::isAir(id)) {
        return false;
    }
    switch (block::renderOf(id)) {
    // The later `MapColor.airColor` set, reached through the property this tree
    // already has for it. See the note in map_sample.hpp.
    case block::RenderType::None:
    case block::RenderType::Torch:
    case block::RenderType::Fire:
    case block::RenderType::RedstoneWire:
    case block::RenderType::Rail:
    case block::RenderType::Lever:
        return false;
    default:
        return true;
    }
}

bool isMapWater(block::BlockId id)
{
    const block::BlockDef& def = block::def(id);
    return def.render == block::RenderType::Fluid && def.light == 0;
}

void sampleChunk(const world::ChunkColumn& column, MapChunkSample* out)
{
    constexpr int kTop = world::ChunkColumn::kHeight - 1;

    for (int z = 0; z < kChunkPixels; ++z) {
        for (int x = 0; x < kChunkPixels; ++x) {
            // The sample is x-major and the height map is z-major, which is the
            // whole of the trade in the header note: this line reads one and
            // writes the other, once per chunk.
            const int i = x * kChunkPixels + z;
            const int mapIndex = z * kChunkPixels + x;

            // Where later versions start: `getHeightValue()`, which is this
            // array, one above the highest block that stops light. Clamped
            // because heightMap is round-tripped from the file and a world
            // written by something else is not owed to be in range.
            int y = int(column.heightMap[mapIndex]);
            if (y > kTop) {
                y = kTop;
            }

            block::BlockId surface = block::kAir;
            for (; y >= 0; --y) {
                const block::BlockId id = column.block(x, y, z);
                if (showsOnMap(id)) {
                    surface = id;
                    break;
                }
            }

            // Nothing anywhere in the column. The pixel is sky, and `height`
            // being 0 is what the shading reads as flat.
            if (y < 0) {
                out->surface[i] = block::kAir;
                out->height[i] = 0;
                out->depth[i] = 0;
                continue;
            }

            out->surface[i] = surface;
            out->height[i] = u8(y);

            // The liquid-depth loop, which later versions run for the same
            // reason: shallow water and deep water are the same colour and are
            // told apart by how far down the floor is.
            int depth = 0;
            if (block::renderOf(surface) == block::RenderType::Fluid) {
                for (int probe = y; probe >= 0 && depth < 255; --probe) {
                    if (block::renderOf(column.block(x, probe, z))
                        != block::RenderType::Fluid) {
                        break;
                    }
                    ++depth;
                }
            }
            out->depth[i] = u8(depth);
        }
    }
}

}  // namespace mc::map
