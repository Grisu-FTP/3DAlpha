#include "impl/worldgen/alpha_nobiome/dungeon.hpp"

#include "blocks.hpp"
#include "core/block/registry.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kCobblestone = u8(mcver::Block::Cobblestone);  // ly.x, 4
constexpr u8 kMossy = u8(mcver::Block::MossyCobblestone);   // ly.ap, 48
constexpr u8 kSpawner = u8(mcver::Block::MobSpawner);       // ly.at, 52
constexpr u8 kChest = u8(mcver::Block::Chest);              // ly.av, 54

// Item ids, read out of a loaded jar rather than remembered. `di.aS` is the
// item's own id field.
constexpr i16 kSaddle = 329;       // di.ay
constexpr i16 kIronIngot = 265;    // di.m
constexpr i16 kBread = 297;        // di.S
constexpr i16 kWheat = 296;        // di.R
constexpr i16 kGunpowder = 289;    // di.K
constexpr i16 kString = 287;       // di.I
constexpr i16 kBucket = 325;       // di.au
constexpr i16 kGoldenApple = 322;  // di.ar
constexpr i16 kRedstone = 331;     // di.aA
constexpr i16 kFirstRecord = 2256; // di.aQ, and di.c[2257] is the second

// `Material.isSolid()`, which is what `cn.f(...).a()` asks.
bool isSolid(const PopulationView& view, i32 x, i32 y, i32 z)
{
    return block::def(view.blockAt(x, y, z)).solid;
}

}  // namespace

item::ItemStack rollDungeonLoot(JavaRandom& random)
{
    item::ItemStack stack;

    // **One draw, always**, and then a branch. Rolls 7, 8 and 9 each spend a
    // second draw before deciding, and roll 10 spends none and yields nothing
    // -- so the number of draws depends on the first one, which is why the
    // stream fingerprint in the tests matters.
    const i32 roll = random.nextInt(11);

    switch (roll) {
    case 0:
        stack.id = kSaddle;
        stack.count = 1;
        return stack;
    case 1:
        stack.id = kIronIngot;
        stack.count = i8(random.nextInt(4) + 1);
        return stack;
    case 2:
        stack.id = kBread;
        stack.count = 1;
        return stack;
    case 3:
        stack.id = kWheat;
        stack.count = i8(random.nextInt(4) + 1);
        return stack;
    case 4:
        stack.id = kGunpowder;
        stack.count = i8(random.nextInt(4) + 1);
        return stack;
    case 5:
        stack.id = kString;
        stack.count = i8(random.nextInt(4) + 1);
        return stack;
    case 6:
        stack.id = kBucket;
        stack.count = 1;
        return stack;
    case 7:
        // One in a hundred, which makes a golden apple the rarest thing a
        // dungeon can hold by a wide margin.
        if (random.nextInt(100) == 0) {
            stack.id = kGoldenApple;
            stack.count = 1;
        }
        return stack;
    case 8:
        if (random.nextInt(2) == 0) {
            stack.id = kRedstone;
            stack.count = i8(random.nextInt(4) + 1);
        }
        return stack;
    case 9:
        // `di.c[di.aQ.id + nextInt(2)]` -- the two music discs, picked out of
        // the global item table by id rather than by a named field.
        if (random.nextInt(10) == 0) {
            stack.id = i16(kFirstRecord + random.nextInt(2));
            stack.count = 1;
        }
        return stack;
    default:
        // Roll 10. The original's final `return null`, and it draws nothing.
        return stack;
    }
}

const char* rollDungeonMob(JavaRandom& random)
{
    // **Zombie twice.** Four outcomes, three mobs -- so a zombie spawner is
    // half of all dungeons and skeleton and spider are a quarter each. Written
    // as the original writes it rather than collapsed, because collapsing it
    // would hide the weighting.
    switch (random.nextInt(4)) {
    case 0:
        return "Skeleton";
    case 1:
        return "Zombie";
    case 2:
        return "Zombie";
    case 3:
        return "Spider";
    default:
        return "";
    }
}

bool generateDungeon(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z,
                     DungeonOutput* out)
{
    constexpr i32 kHeight = 3;

    // Two to three, independently per axis, so a room is 5x5, 5x7 or 7x7 in
    // floor area. Drawn before anything is inspected, so a refused dungeon
    // still costs both.
    //
    // Swapping these two lines is an **equivalent mutant** and survives the
    // fixture, which is correct rather than a gap: both draws are the same
    // `nextInt(2) + 2` against the same stream, so exchanging them exchanges
    // two identical expressions. The room's *shape* would transpose only if
    // the two axes were drawn with different bounds, and they are not.
    const i32 spanX = random.nextInt(2) + 2;
    const i32 spanZ = random.nextInt(2) + 2;

    // **Pass one: is there a room-shaped hole here, with exactly one to five
    // openings in its wall?** The count is what makes dungeons sit against
    // caves rather than float in solid rock.
    i32 openings = 0;

    for (i32 px = x - spanX - 1; px <= x + spanX + 1; ++px) {
        for (i32 py = y - 1; py <= y + kHeight + 1; ++py) {
            for (i32 pz = z - spanZ - 1; pz <= z + spanZ + 1; ++pz) {
                const bool solid = isSolid(view, px, py, pz);

                // Floor and ceiling must be solid all the way across.
                if (py == y - 1 && !solid) {
                    return false;
                }
                if (py == y + kHeight + 1 && !solid) {
                    return false;
                }

                // On the wall ring, at the room's own floor level, an air cell
                // with air above it is a doorway.
                const bool onWall = px == x - spanX - 1 || px == x + spanX + 1 ||
                                    pz == z - spanZ - 1 || pz == z + spanZ + 1;
                if (onWall && py == y && view.blockAt(px, py, pz) == kAir &&
                    view.blockAt(px, py + 1, pz) == kAir) {
                    ++openings;
                }
            }
        }
    }

    if (openings < 1 || openings > 5) {
        return false;
    }

    // **Pass two: carve the room and build its shell.** Note the y loop runs
    // *downward* from the ceiling, which matters only for where the writes
    // land in time and not for the result -- but it is the original's order.
    for (i32 px = x - spanX - 1; px <= x + spanX + 1; ++px) {
        for (i32 py = y + kHeight; py >= y - 1; --py) {
            for (i32 pz = z - spanZ - 1; pz <= z + spanZ + 1; ++pz) {
                const bool onShell = px == x - spanX - 1 || py == y - 1 ||
                                     pz == z - spanZ - 1 || px == x + spanX + 1 ||
                                     py == y + kHeight + 1 || pz == z + spanZ + 1;
                if (!onShell) {
                    // The room's interior.
                    view.setBlock(px, py, pz, kAir);
                    continue;
                }

                // **The floor is special, and the order of these two tests is
                // the whole trick.** Below the room, a non-solid cell is
                // cleared to air rather than filled -- so a dungeon opening
                // onto a cave keeps the hole. Everywhere else on the shell, a
                // solid cell becomes cobblestone, and the floor gets mossy
                // cobble a quarter of the time.
                if (py >= 0 && !isSolid(view, px, py - 1, pz)) {
                    view.setBlock(px, py, pz, kAir);
                    continue;
                }
                if (!isSolid(view, px, py, pz)) {
                    continue;
                }
                if (py == y - 1 && random.nextInt(4) != 0) {
                    view.setBlock(px, py, pz, kMossy);
                } else {
                    view.setBlock(px, py, pz, kCobblestone);
                }
            }
        }
    }

    if (out != nullptr) {
        out->placed = true;
        out->chests.clear();
    }

    // **Up to two chests, three tries each.** The tries are not independent:
    // the inner loop breaks on success, so a chest that lands on its first try
    // costs one position roll and a chest that never lands costs three.
    for (i32 chest = 0; chest < 2; ++chest) {
        for (i32 attempt = 0; attempt < 3; ++attempt) {
            const i32 cx = x + random.nextInt(spanX * 2 + 1) - spanX;
            const i32 cy = y;
            const i32 cz = z + random.nextInt(spanZ * 2 + 1) - spanZ;

            if (view.blockAt(cx, cy, cz) != kAir) {
                continue;
            }

            // Exactly one solid neighbour, so a chest stands against a wall
            // rather than in the middle of the floor or wedged in a corner.
            i32 walls = 0;
            walls += isSolid(view, cx - 1, cy, cz) ? 1 : 0;
            walls += isSolid(view, cx + 1, cy, cz) ? 1 : 0;
            walls += isSolid(view, cx, cy, cz - 1) ? 1 : 0;
            walls += isSolid(view, cx, cy, cz + 1) ? 1 : 0;
            if (walls != 1) {
                continue;
            }

            view.setBlock(cx, cy, cz, kChest);

            // **Eight rolls into a 27-slot chest, and the slot is random**, so
            // two rolls can collide and the later one wins. That is why the
            // contents are built by slot rather than appended.
            item::ItemStack slots[27];
            for (int i = 0; i < 27; ++i) {
                slots[i].slot = i8(i);
            }

            for (i32 i = 0; i < 8; ++i) {
                const item::ItemStack loot = rollDungeonLoot(random);
                if (loot.empty()) {
                    // **The slot draw is skipped entirely when the roll
                    // produced nothing.** The original tests the item for null
                    // before calling setInventorySlotContents, so a failed roll
                    // costs no position draw -- getting this wrong shifts every
                    // subsequent item.
                    continue;
                }
                const i32 slot = random.nextInt(27);
                slots[usize(slot)].id = loot.id;
                slots[usize(slot)].count = loot.count;
                slots[usize(slot)].damage = loot.damage;
            }

            if (out != nullptr) {
                DungeonChest record;
                record.x = cx;
                record.y = cy;
                record.z = cz;
                for (const item::ItemStack& stack : slots) {
                    if (!stack.empty()) {
                        record.contents.push_back(stack);
                    }
                }
                out->chests.push_back(record);
            }
            break;
        }
    }

    // The spawner goes in last, at the room's exact centre.
    view.setBlock(x, y, z, kSpawner);
    const char* mob = rollDungeonMob(random);

    if (out != nullptr) {
        out->spawner.x = x;
        out->spawner.y = y;
        out->spawner.z = z;
        out->spawner.mob = mob;
    }

    return true;
}

}  // namespace mc::worldgen
