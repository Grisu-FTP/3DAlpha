// `cn.g`, `cn.f`, `cn.a()` and the constructor's walk. See spawn_point.hpp.

#include "core/world/spawn_point.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "core/world/level_data.hpp"
#include "version_slots.hpp"  // generated; see tools/configure.py

#include <memory>
#include <vector>

namespace mc::world {

namespace {

constexpr int kSpawnBlock = int(mcver::Block::Sand);

// The generator's own column, and the last one it was asked for. The walk moves
// by at most 63 blocks a step, so consecutive probes land in the same chunk
// often enough to be worth the one-entry cache and never often enough to be
// worth more than that.
struct TerrainProbe {
    mcver::WorldGen* gen = nullptr;
    std::vector<u8> blocks = std::vector<u8>(usize(mcver::kWorldGenChunkBlocks));
    i32 haveX = 0;
    i32 haveZ = 0;
    bool have = false;
    int generated = 0;

    static i32 chunkOf(i32 value) { return value >> 4; }

    int blockAt(i32 x, int y, i32 z)
    {
        if (y < 0 || y > 127) {
            return 0;
        }
        const i32 cx = chunkOf(x);
        const i32 cz = chunkOf(z);
        if (!have || cx != haveX || cz != haveZ) {
            gen->generateColumn(cx, cz, blocks.data());
            haveX = cx;
            haveZ = cz;
            have = true;
            ++generated;
        }
        const int lx = int(x - cx * 16);
        const int lz = int(z - cz * 16);
        return int(blocks[usize((lx * 16 + lz) * 128 + y)]);
    }

    static int read(void* context, i32 x, int y, i32 z)
    {
        return static_cast<TerrainProbe*>(context)->blockAt(x, y, z);
    }
};

}  // namespace

int topBlockAt(const ColumnProbe& probe, i32 x, i32 z)
{
    // `int y = 63; while (getBlockId(x, y + 1, z) != 0) y++; return
    // getBlockId(x, y, z);` -- upward only, and starting at 63 rather than at
    // the sea level 64 one block above it.
    int y = 63;
    while (probe.blockAt(probe.context, x, y + 1, z) != 0) {
        ++y;
    }
    return probe.blockAt(probe.context, x, y, z);
}

bool walkToFreshSpawn(const ColumnProbe& probe, JavaRandom& random, i32* x, i32* z, int maxSteps)
{
    for (int step = 0; step < maxSteps; ++step) {
        if (topBlockAt(probe, *x, *z) == kSpawnBlock) {
            return true;
        }
        // **Two draws each, subtracted**, so the walk is centred on where it
        // already is and can stand still. Both axes move on every step,
        // including the one whose draw came back zero.
        *x += i32(random.nextInt(64) - random.nextInt(64));
        *z += i32(random.nextInt(64) - random.nextInt(64));
    }
    return topBlockAt(probe, *x, *z) == kSpawnBlock;
}

bool nudgeSpawnOffAir(const ColumnProbe& probe, JavaRandom& random, i32* x, int* y, i32* z,
                      int maxSteps)
{
    if (*y <= 0) {
        *y = 64;
    }
    for (int step = 0; step < maxSteps; ++step) {
        if (topBlockAt(probe, *x, *z) != 0) {
            return true;
        }
        *x += i32(random.nextInt(8) - random.nextInt(8));
        *z += i32(random.nextInt(8) - random.nextInt(8));
    }
    return topBlockAt(probe, *x, *z) != 0;
}

int chooseFreshSpawn(i64 seed, bool snowCovered, JavaRandom& random, LevelData* level,
                     int maxSteps)
{
    if (level == nullptr) {
        return 0;
    }
    mcver::WorldGenOptions options;
    options.snowCovered = snowCovered;
    // **The two Extra Settings are deliberately left off here.** Neither can
    // change the shape of the ground -- one floors a vein's bounding box, the
    // other lays bedrock at y = 0 -- so neither can change which column has
    // sand on top, and passing them would only make this search depend on
    // settings a player can still change afterwards.

    // ~300 KB of octave tables. Never a local; see chunk_provider.hpp.
    auto gen = std::make_unique<mcver::WorldGen>(seed, options);
    auto probe = std::make_unique<TerrainProbe>();
    probe->gen = gen.get();

    const ColumnProbe column{probe.get(), &TerrainProbe::read};

    i32 x = 0;
    i32 z = 0;
    walkToFreshSpawn(column, random, &x, &z, maxSteps);

    level->spawnX = x;
    level->spawnY = 64;
    level->spawnZ = z;
    return probe->generated;
}

}  // namespace mc::world
