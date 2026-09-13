// `cn.g`, `cn.f`, `cn.a()` and the constructor's spawn walk. See
// core/world/spawn_point.hpp.

#include "blocks.hpp"
#include "core/util/java_random.hpp"
#include "core/world/level_data.hpp"
#include "core/world/spawn_point.hpp"
#include "framework.hpp"

#include <cstdio>
#include <vector>

using namespace mc;
using world::ColumnProbe;

namespace {

// A world of columns whose contents this test decides, so the two searches can
// be pinned without a generator anywhere near them.
struct FlatWorld {
    // Every column is air above `height` and `surface` at it, except the ones
    // named in `sandAt`, which get sand.
    int height = 64;
    int surface = int(mcver::Block::Stone);
    std::vector<std::pair<i32, i32>> sandAt;
    int reads = 0;

    bool isSand(i32 x, i32 z) const
    {
        for (const auto& at : sandAt) {
            if (at.first == x && at.second == z) {
                return true;
            }
        }
        return false;
    }

    int blockAt(i32 x, int y, i32 z)
    {
        ++reads;
        if (y < 0 || y > 127) {
            return 0;
        }
        if (y > height) {
            return 0;
        }
        if (y == height) {
            return isSand(x, z) ? int(mcver::Block::Sand) : surface;
        }
        return int(mcver::Block::Stone);
    }

    static int read(void* context, i32 x, int y, i32 z)
    {
        return static_cast<FlatWorld*>(context)->blockAt(x, y, z);
    }

    ColumnProbe probe() { return ColumnProbe{this, &FlatWorld::read}; }
};

}  // namespace

TEST(the_top_block_is_found_by_climbing_and_never_by_looking_down)
{
    FlatWorld world;
    world.height = 70;
    CHECK_EQ(world::topBlockAt(world.probe(), 0, 0), int(mcver::Block::Stone));

    // A column whose ground is below 63 reports whatever 63 holds, which is the
    // original's blind spot and not a rounding error: `g` starts at 63 and only
    // ever climbs.
    world.height = 40;
    CHECK_EQ(world::topBlockAt(world.probe(), 0, 0), 0);
}

TEST(the_fresh_spawn_walk_stops_on_sand_and_moves_both_axes)
{
    FlatWorld world;
    world.sandAt = {{0, 0}};
    JavaRandom random(1);
    i32 x = 0;
    i32 z = 0;
    CHECK(world::walkToFreshSpawn(world.probe(), random, &x, &z, 8));
    // Already standing on sand: accepted without a single draw.
    CHECK_EQ(int(x), 0);
    CHECK_EQ(int(z), 0);

    // Nowhere to accept: the walk runs out and keeps where it got to, which is
    // not where it started.
    FlatWorld stone;
    JavaRandom other(5);
    x = 0;
    z = 0;
    CHECK(!world::walkToFreshSpawn(stone.probe(), other, &x, &z, 6));
    CHECK(x != 0 || z != 0);
    // Six steps of nextInt(64) - nextInt(64) cannot have gone further than
    // that, whatever it drew.
    CHECK(x > -64 * 6 && x < 64 * 6);
    CHECK(z > -64 * 6 && z < 64 * 6);
}

TEST(the_nudge_moves_off_air_and_lifts_a_y_at_or_below_zero)
{
    FlatWorld world;
    // Nothing anywhere: every column reads as air, so the nudge runs out.
    world.height = -1;
    JavaRandom random(3);
    i32 x = 0;
    int y = 0;
    i32 z = 0;
    CHECK(!world::nudgeSpawnOffAir(world.probe(), random, &x, &y, &z, 4));
    CHECK_EQ(y, 64);

    // Ground everywhere: accepted where it stands, and a y that was already
    // above zero is left alone.
    FlatWorld ground;
    y = 71;
    x = 3;
    z = -9;
    CHECK(world::nudgeSpawnOffAir(ground.probe(), random, &x, &y, &z, 4));
    CHECK_EQ(y, 71);
    CHECK_EQ(int(x), 3);
    CHECK_EQ(int(z), -9);
}

TEST(a_fresh_world_spawns_on_sand_within_a_few_dozen_columns)
{
    const i64 seeds[] = {1LL, 12345LL, 42LL, -1LL, 987654321LL, -8765432109876LL};
    int worst = 0;
    for (i64 seed : seeds) {
        world::LevelData level;
        JavaRandom random(seed ^ 0x5DEECE66DLL);
        const int generated = world::chooseFreshSpawn(seed, false, random, &level);
        if (generated > worst) {
            worst = generated;
        }
        CHECK_EQ(level.spawnY, 64);
        std::printf("    seed %lld: spawn %d,%d after %d columns\n", (long long)seed,
                    int(level.spawnX), int(level.spawnZ), generated);
    }
    // The cost of this is paid once, on the menu thread, while a world is being
    // created. If it ever stops being a few dozen columns that is worth
    // knowing about before a player waits for it.
    CHECK(worst < 200);
}
