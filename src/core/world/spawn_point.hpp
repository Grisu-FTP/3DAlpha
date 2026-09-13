#pragma once

// **Where a brand new world starts the player**, which in a1.1.2 is not
// (0, 64, 0) and is not the ground under it either.
//
// Three methods of `cn` between them decide it, and this is all three:
//
//   * `cn.g(int,int)` -- the block at the top of a column. It starts at y = 63
//     and climbs while the block *above* is not air, then reports the block it
//     stopped on. It never looks down, so a column whose 64 is air reports
//     whatever 63 holds -- which over an ocean is water and not the sea bed.
//   * `cn.f(int,int)` -- whether a column may be spawned on: `g(x, z)` has to
//     be **sand**. That one test is why every Alpha world starts on a beach.
//   * `cn`'s constructor, on the branch taken when there is no level.dat:
//     spawn starts at (0, 64, 0) and walks by `nextInt(64) - nextInt(64)` on
//     both axes until `f` accepts, with no limit on how long it looks.
//   * `cn.a()` -- run again on every respawn and every load: `if (spawnY <= 0)
//     spawnY = 64`, then nudge by `nextInt(8) - nextInt(8)` until `g` reports
//     anything but air. It is the weaker test of the two and it never moves y.
//
// **y is never searched for.** Both loops move x and z only; `spawnY` stays 64
// unless level.dat says otherwise. What puts the player on top of the ground is
// `kh.q()`, the lift out of the ground that runs when the body is built -- see
// `PlayerBody::liftOutOfGround`. Searching for a surface height here would be a
// different game: it would put the player on the mountain that Alpha drops them
// inside of and then lifts them out of.
//
// **The one deviation, and why.** The original's walk has no bound, and on this
// console each step of it is a chunk generated on the main thread while the
// player waits at a menu. `maxSteps` bounds it; running out keeps the position
// the walk had reached, which is what the unbounded loop would have returned had
// the next draw been the accepting one. It is reported, so a caller can say so.

#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::world {

struct LevelData;

// How the search reads the world it is choosing a point in. `blockAt` must
// answer 0 for anything outside 0..127 on y, as `cn.a(int,int,int)` does.
struct ColumnProbe {
    void* context = nullptr;
    int (*blockAt)(void* context, i32 x, int y, i32 z) = nullptr;
};

// `cn.g(int,int)`.
int topBlockAt(const ColumnProbe& probe, i32 x, i32 z);

// The constructor's walk. `*x` and `*z` carry the starting point in and the
// answer out. False when `maxSteps` ran out first, `*x` and `*z` still holding
// where it had got to.
bool walkToFreshSpawn(const ColumnProbe& probe, JavaRandom& random, i32* x, i32* z,
                      int maxSteps);

// `cn.a()`. Moves x and z off a column of air, and lifts a y at or below zero
// back to 64. False when `maxSteps` ran out.
bool nudgeSpawnOffAir(const ColumnProbe& probe, JavaRandom& random, i32* x, int* y, i32* z,
                      int maxSteps);

// Both of the above over a freshly generated world: makes a generator for
// `seed`, runs the constructor's walk from (0, 0), and writes the result into
// `level`. `snowCovered` is the level's own flag and has to be the one the
// world will be generated with, since it decides whether an ocean's top block
// is water or ice.
//
// **Generates terrain and nothing else** -- `nw.b`, which is what the original
// has to hand at this point too: population has not run for any of these chunks
// when the constructor walks over them. A tree that a later population pass
// stands on the accepted column can therefore leave the spawn under leaves, and
// a1.1.2 has exactly the same gap.
//
// Returns how many columns it had to generate, so a caller can log it; the
// spawn is written either way.
int chooseFreshSpawn(i64 seed, bool snowCovered, JavaRandom& random, LevelData* level,
                     int maxSteps = 512);

}  // namespace mc::world
