#include "framework.hpp"

#include "core/util/java_random.hpp"
#include "dungeon_vectors.hpp"
#include "impl/worldgen/alpha_nobiome/dungeon.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

#include <cstring>
#include <map>
#include <vector>

using namespace mc;
using namespace mc::test;
using worldgen::PopulationView;

namespace {

constexpr int kChunkBytes = 32768;

// The built scene, with the same rules genref uses: solid stone, a cavity, and
// a number of doorways punched through one wall.
//
// Unlike the tree scenes this carries no terrain at all -- a dungeon needs a
// room-shaped void inside solid rock, which real terrain almost never has, so
// the scene is constructed rather than probed and the fixture does not have to
// ship a patch of blocks.
struct Scene {
    std::vector<std::vector<u8>> storage;
    std::vector<u8*> pointers;
    PopulationView view;

    void build(i32 x, i32 z, int doorways)
    {
        const i32 originChunkX = (x >> 4) - 2;
        const i32 originChunkZ = (z >> 4) - 2;

        storage.assign(25, std::vector<u8>(usize(kChunkBytes), 0));
        pointers.clear();
        for (auto& column : storage) {
            pointers.push_back(column.data());
        }
        view.reset(originChunkX, originChunkZ, 5, 5, pointers.data());

        const i32 y = kDungeonY;

        for (i32 bx = x - kDungeonPatch; bx <= x + kDungeonPatch; ++bx) {
            for (i32 bz = z - kDungeonPatch; bz <= z + kDungeonPatch; ++bz) {
                for (i32 by = y - 6; by <= y + 10; ++by) {
                    view.setBlock(bx, by, bz, u8(1));
                }
            }
        }

        for (i32 bx = x - kCavityX; bx <= x + kCavityX; ++bx) {
            for (i32 bz = z - kCavityZ; bz <= z + kCavityZ; ++bz) {
                for (i32 by = y; by <= y + kCavityH; ++by) {
                    view.setBlock(bx, by, bz, u8(0));
                }
            }
        }

        for (int d = 0; d < doorways && d < 2 * kCavityZ; ++d) {
            const i32 bz = z - kCavityZ + d;
            for (i32 bx = x + kCavityX + 1; bx <= x + kCavityX + 3; ++bx) {
                view.setBlock(bx, y, bz, u8(0));
                view.setBlock(bx, y + 1, bz, u8(0));
            }
        }

        view.reset(originChunkX, originChunkZ, 5, 5, pointers.data());
        view.clearRefusals();
    }

    std::map<std::vector<i32>, u8> snapshot(i32 x, i32 z) const
    {
        std::map<std::vector<i32>, u8> out;
        for (i32 dx = -kDungeonPatch; dx <= kDungeonPatch; ++dx) {
            for (i32 dz = -kDungeonPatch; dz <= kDungeonPatch; ++dz) {
                for (i32 y = 0; y < 128; ++y) {
                    out[{dx, y, dz}] = view.blockAt(x + dx, y, z + dz);
                }
            }
        }
        return out;
    }
};

}  // namespace

// WorldGenDungeons against the jar's own: the blocks, the spawner's mob, every
// stack in every chest, and where the stream ended up.
TEST(dungeons_match_the_jar)
{
    for (int c = 0; c < kDungeonCaseCount; ++c) {
        const auto& want = kDungeonCases[c];

        Scene scene;
        scene.build(want.x, want.z, want.doorways);
        const std::map<std::vector<i32>, u8> before = scene.snapshot(want.x, want.z);

        JavaRandom random(want.rngSeed);
        worldgen::DungeonOutput out;
        const bool placed =
            worldgen::generateDungeon(scene.view, random, want.x, want.y, want.z, &out);

        // **The fingerprint first.** A dungeon's draw count varies with the
        // room size, how many chest attempts succeed, and -- the subtle one --
        // whether each loot roll produced an item, because a failed roll skips
        // the slot draw entirely.
        CHECK_EQ(random.nextLong(), want.afterDraw);
        CHECK_EQ(int(placed), int(want.placed));

        const std::map<std::vector<i32>, u8> after = scene.snapshot(want.x, want.z);

        std::map<std::vector<i32>, u8> expected;
        for (int i = 0; i < want.changeCount; ++i) {
            const auto& change = want.changes[i];
            expected[{change.dx, change.y, change.dz}] = change.id;
        }

        std::map<std::vector<i32>, u8> got;
        for (const auto& entry : after) {
            if (before.find(entry.first)->second != entry.second) {
                got[entry.first] = entry.second;
            }
        }

        CHECK_EQ(int(got.size()), want.changeCount);
        CHECK(got == expected);
        CHECK_EQ(int(scene.view.refusedOutOfWindow()), 0);

        if (!placed) {
            // A refused dungeon leaves nothing behind at all.
            CHECK_EQ(int(out.chests.size()), 0);
            continue;
        }

        // The spawner, at the room's exact centre.
        CHECK_EQ(out.spawner.x, want.x);
        CHECK_EQ(out.spawner.y, want.y);
        CHECK_EQ(out.spawner.z, want.z);
        CHECK(std::strcmp(out.spawner.mob, want.mob) == 0);

        // The chests, in the order the fixture walks them -- which is the
        // patch scan order, not the order they were placed.
        CHECK_EQ(int(out.chests.size()), want.chestCount);

        std::map<std::vector<i32>, int> chestIndex;
        for (int i = 0; i < want.chestCount; ++i) {
            chestIndex[{want.chests[i].dx, want.chests[i].y, want.chests[i].dz}] = i;
        }
        for (const worldgen::DungeonChest& chest : out.chests) {
            const std::vector<i32> at{chest.x - want.x, chest.y, chest.z - want.z};
            CHECK(chestIndex.count(at) == 1);
        }

        // Every stack, by chest and slot.
        std::map<std::vector<i32>, std::vector<i32>> wantStacks;
        for (int i = 0; i < want.stackCount; ++i) {
            const auto& stack = want.stacks[i];
            wantStacks[{stack.chest, stack.slot}] = {stack.id, stack.count, stack.damage};
        }

        std::map<std::vector<i32>, std::vector<i32>> gotStacks;
        for (const worldgen::DungeonChest& chest : out.chests) {
            const std::vector<i32> at{chest.x - want.x, chest.y, chest.z - want.z};
            const int index = chestIndex[at];
            for (const item::ItemStack& stack : chest.contents) {
                gotStacks[{index, i32(stack.slot)}] = {i32(stack.id), i32(stack.count),
                                                       i32(stack.damage)};
            }
        }

        CHECK_EQ(int(gotStacks.size()), want.stackCount);
        CHECK(gotStacks == wantStacks);
    }
}

// The fixture has to cover both outcomes and all three mobs, or the comparison
// above would be satisfied by a generator that refused everything.
TEST(the_dungeon_fixture_covers_placement_refusal_and_every_mob)
{
    int placed = 0;
    int refused = 0;
    int totalChanges = 0;
    int totalStacks = 0;
    bool sawSkeleton = false;
    bool sawZombie = false;
    bool sawSpider = false;
    int twoChestCases = 0;

    for (int c = 0; c < kDungeonCaseCount; ++c) {
        const auto& want = kDungeonCases[c];
        totalChanges += want.changeCount;
        totalStacks += want.stackCount;

        if (!want.placed) {
            ++refused;
            // A refused dungeon changes nothing -- the room is built only
            // after the opening count passes.
            CHECK_EQ(want.changeCount, 0);
            CHECK_EQ(want.chestCount, 0);
            continue;
        }

        ++placed;
        // A room is at least a 5x5x3 shell, so it moves a lot of blocks.
        CHECK(want.changeCount > 100);
        CHECK(want.chestCount >= 1);
        CHECK(want.chestCount <= 2);
        if (want.chestCount == 2) {
            ++twoChestCases;
        }

        if (std::strcmp(want.mob, "Skeleton") == 0) {
            sawSkeleton = true;
        } else if (std::strcmp(want.mob, "Zombie") == 0) {
            sawZombie = true;
        } else if (std::strcmp(want.mob, "Spider") == 0) {
            sawSpider = true;
        } else {
            CHECK(want.mob[0] == '\0');
            CHECK(false);  // an unnamed mob on a placed dungeon is a bug
        }
    }

    CHECK_EQ(totalChanges, kDungeonTotalChanges);
    CHECK_EQ(totalStacks, kDungeonTotalStacks);
    CHECK(placed >= 4);
    CHECK(refused >= 4);

    // All three mobs, which is what pins the picker's weighting -- Zombie
    // appears twice in a `nextInt(4)`, so it is half of all dungeons.
    CHECK(sawSkeleton);
    CHECK(sawZombie);
    CHECK(sawSpider);

    // And at least one room got its second chest, so the two-chest loop is
    // exercised rather than only its first iteration.
    CHECK(twoChestCases >= 1);
}

// The loot table on its own, driven far enough to reach every branch. This is
// a pure function of the stream, so it can be checked without a room -- and it
// is worth checking separately because the rare branches (a golden apple at one
// in eleven then one in a hundred) would otherwise never appear in a fixture of
// this size.
TEST(the_loot_table_reaches_every_branch)
{
    int counts[3000] = {};
    int empties = 0;

    JavaRandom random(20240101);
    for (int i = 0; i < 200000; ++i) {
        const item::ItemStack stack = worldgen::rollDungeonLoot(random);
        if (stack.empty()) {
            ++empties;
            continue;
        }
        CHECK(stack.id >= 0);
        CHECK(stack.id < 3000);
        CHECK(stack.count >= 1);
        CHECK(stack.count <= 4);
        ++counts[stack.id];
    }

    // The seven unconditional branches.
    CHECK(counts[329] > 0);  // saddle
    CHECK(counts[265] > 0);  // iron ingot
    CHECK(counts[297] > 0);  // bread
    CHECK(counts[296] > 0);  // wheat
    CHECK(counts[289] > 0);  // gunpowder
    CHECK(counts[287] > 0);  // string
    CHECK(counts[325] > 0);  // bucket

    // The three gated ones, including both records.
    CHECK(counts[322] > 0);   // golden apple, 1 in 1100
    CHECK(counts[331] > 0);   // redstone
    CHECK(counts[2256] > 0);  // record 1
    CHECK(counts[2257] > 0);  // record 2

    // **The empty rate is derived, not eyeballed.** Roll 10 always yields
    // nothing; 7 yields nothing unless a 1-in-100 lands, 8 unless a coin flip
    // does, 9 unless a 1-in-10 does. So
    //
    //     P(empty) = (1 + 99/100 + 1/2 + 9/10) / 11 = 0.3082
    //
    // which is 61,636 of 200,000. The first version of this case guessed "about
    // a fifth" and bounded it below 60,000, which failed -- the guess was
    // wrong, not the generator. Bounds are +-2% of the computed value, wide
    // enough for sampling noise and narrow enough that a wrong gate on any of
    // the three conditional rolls falls outside.
    CHECK(empties > 60400);
    CHECK(empties < 62900);

    // The saddle and the golden apple share a one-in-eleven first roll, but
    // the apple needs a second one-in-a-hundred, so it has to be far rarer.
    CHECK(counts[322] * 20 < counts[329]);
}

// The mob picker's weighting: four outcomes, three mobs, Zombie twice.
TEST(the_spawner_picker_weights_zombie_double)
{
    int skeleton = 0;
    int zombie = 0;
    int spider = 0;

    JavaRandom random(777);
    for (int i = 0; i < 40000; ++i) {
        const char* mob = worldgen::rollDungeonMob(random);
        if (std::strcmp(mob, "Skeleton") == 0) {
            ++skeleton;
        } else if (std::strcmp(mob, "Zombie") == 0) {
            ++zombie;
        } else if (std::strcmp(mob, "Spider") == 0) {
            ++spider;
        } else {
            CHECK(false);  // the fourth branch is unreachable
        }
    }

    CHECK_EQ(skeleton + zombie + spider, 40000);
    // Zombie is two of the four outcomes; the other two are one each. Loose
    // bounds, because this is checking the weighting and not the RNG.
    CHECK(zombie > skeleton + spider - 2000);
    CHECK(zombie < skeleton + spider + 2000);
    CHECK(skeleton > 8000);
    CHECK(spider > 8000);
}
