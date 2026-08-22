#include "impl/worldgen/alpha_nobiome/populate.hpp"

#include "blocks.hpp"
#include "core/block/registry.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/chunk_provider.hpp"
#include "impl/worldgen/alpha_nobiome/dungeon.hpp"
#include "impl/worldgen/alpha_nobiome/flowers.hpp"
#include "impl/worldgen/alpha_nobiome/liquids.hpp"
#include "impl/worldgen/alpha_nobiome/ore.hpp"
#include "impl/worldgen/alpha_nobiome/plants.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"
#include "impl/worldgen/alpha_nobiome/trees.hpp"
#include "impl/worldgen/alpha_nobiome/big_tree.hpp"

namespace mc::worldgen {

PopulationSideEffects::PopulationSideEffects() = default;
PopulationSideEffects::~PopulationSideEffects() = default;
PopulationSideEffects::PopulationSideEffects(PopulationSideEffects&&) noexcept = default;
PopulationSideEffects& PopulationSideEffects::operator=(PopulationSideEffects&&) noexcept = default;

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kIce = u8(mcver::Block::Ice);
constexpr u8 kSnowLayer = u8(mcver::Block::SnowLayer);
constexpr u8 kDandelion = u8(mcver::Block::Dandelion);
constexpr u8 kRose = u8(mcver::Block::Rose);
constexpr u8 kBrownMushroom = u8(mcver::Block::BrownMushroom);
constexpr u8 kRedMushroom = u8(mcver::Block::RedMushroom);
constexpr u8 kFlowingWater = u8(mcver::Block::FlowingWater);
constexpr u8 kFlowingLava = u8(mcver::Block::FlowingLava);

// One ore pass: `count` attempts, each picking a spot with y drawn from
// `nextInt(yBound)` and growing a vein of `veinSize`.
struct OrePass {
    int count;
    i32 yBound;
    i32 veinSize;
    u8 blockId;
};

// Read off the driver's bytecode, in its order. Clay is first and is a
// different generator; the rest are all WorldGenMinable.
constexpr OrePass kOrePasses[] = {
    {20, 128, 32, u8(mcver::Block::Dirt)},
    {10, 128, 32, u8(mcver::Block::Gravel)},
    {20, 128, 16, u8(mcver::Block::CoalOre)},
    {20, 64, 8, u8(mcver::Block::IronOre)},
    {2, 32, 8, u8(mcver::Block::GoldOre)},
    {8, 16, 7, u8(mcver::Block::RedstoneOre)},
    {1, 16, 7, u8(mcver::Block::DiamondOre)},
};

// `World.getTopSolidOrLiquidBlock` -- scans down from 127 for the first block
// whose material blocks movement or is a liquid, and answers one above it.
//
// **Not the height map.** The height map stops at the first block with any
// opacity at all, so it stops on leaves and on water; this one keeps going
// through anything a player could walk into and answers the standing height.
// Snow uses this, trees use the height map, and they disagree under a canopy.
i32 topSolidOrLiquid(const PopulationView& view, i32 x, i32 z)
{
    for (i32 y = 127; y > 0; --y) {
        const u8 id = view.blockAt(x, y, z);
        if (id == kAir) {
            continue;
        }
        const block::BlockDef& def = block::def(id);
        // `Material.blocksMovement()` is our `solid` column, verified against
        // the jar id by id; `Material.isLiquid()` is the fluid render type,
        // which in a1.1.2 is exactly the four water and lava ids.
        if (def.solid || def.render == block::RenderType::Fluid) {
            return y + 1;
        }
    }
    return -1;
}

}  // namespace

void populateChunk(ChunkProvider& provider, PopulationView& view, i32 chunkX, i32 chunkZ,
                   PopulationSideEffects* sideEffects)
{
    JavaRandom& random = provider.random();
    const i64 worldSeed = provider.seed();

    const i32 blockX = chunkX * 16;
    const i32 blockZ = chunkZ * 16;

    // The population seed. Truncating division and wrapping multiplication --
    // see the header.
    random.setSeed(worldSeed);
    const i64 strideX = (random.nextLong() / 2) * 2 + 1;
    const i64 strideZ = (random.nextLong() / 2) * 2 + 1;
    const u64 mixed = u64(i64(chunkX)) * u64(strideX) + u64(i64(chunkZ)) * u64(strideZ);
    random.setSeed(i64(mixed ^ u64(worldSeed)));

    // ---- dungeons -------------------------------------------------------
    // Eight tries, and **offset by +8 like everything except the ores**.
    for (int i = 0; i < 8; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;

        DungeonOutput out;
        if (generateDungeon(view, random, x, y, z, sideEffects != nullptr ? &out : nullptr) &&
            sideEffects != nullptr) {
            sideEffects->spawners.push_back(out.spawner);
            for (DungeonChest& chest : out.chests) {
                sideEffects->chests.push_back(std::move(chest));
            }
        }
    }

    // ---- clay -----------------------------------------------------------
    // **No +8 offset**, unlike the dungeons above it. Clay and the seven ore
    // passes below all address the chunk's own corner -- confirmed in the
    // bytecode, where the clay loop pushes `iload 4` and the perturbation with
    // no `bipush 8` between them.
    //
    // Adding a +8 here survives the fixture, and that is a **gap rather than
    // an equivalence**: clay only replaces sand touching water, and none of the
    // six fixture chunks has a sandy shoreline in the shifted square, so the
    // pass places nothing either way. It would show the moment a case with a
    // beach were added. Recorded rather than papered over -- the same is true
    // of the brown-mushroom roll, which never fires in these six chunks.
    for (int i = 0; i < 10; ++i) {
        const i32 x = blockX + random.nextInt(16);
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16);
        generateClayPatch(view, random, 32, x, y, z);
    }

    // ---- ores -----------------------------------------------------------
    for (const OrePass& pass : kOrePasses) {
        for (int i = 0; i < pass.count; ++i) {
            const i32 x = blockX + random.nextInt(16);
            const i32 y = random.nextInt(pass.yBound);
            const i32 z = blockZ + random.nextInt(16);
            generateOreVein(view, random, pass.blockId, pass.veinSize, x, y, z);
        }
    }

    // ---- trees ----------------------------------------------------------
    // **One generator instance for the whole chunk**, which is what the driver
    // does -- the `new oa()` and `new ej()` are both outside the loop -- and
    // which matters because `ej` carries its height limit from one tree to the
    // next. See BigTreeState.
    BigTreeState bigTreeState;
    const TreeBatch batch = treeBatchFor(provider.treeDensity(), random, blockX, blockZ);
    for (int i = 0; i < batch.count; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 z = blockZ + random.nextInt(16) + 8;
        // **The height map, not the top solid block** -- so a tree planted
        // where one already stands starts at the canopy, which is what makes
        // forests thicken rather than spread.
        const i32 y = i32(view.heightAt(x, z));
        if (batch.big) {
            generateBigTree(view, random, bigTreeState, x, y, z);
        } else {
            generateTree(view, random, x, y, z);
        }
    }

    // ---- flowers and mushrooms -----------------------------------------
    // Dandelions twice unconditionally; the other three each behind their own
    // roll, and **the roll happens before the position draws**, so a refused
    // plant costs one number rather than four.
    for (int i = 0; i < 2; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generatePlants(view, random, kDandelion, x, y, z);
    }
    if (random.nextInt(2) == 0) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generatePlants(view, random, kRose, x, y, z);
    }
    if (random.nextInt(4) == 0) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generatePlants(view, random, kBrownMushroom, x, y, z);
    }
    if (random.nextInt(8) == 0) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generatePlants(view, random, kRedMushroom, x, y, z);
    }

    // ---- reeds and cactus ----------------------------------------------
    for (int i = 0; i < 10; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generateReeds(view, random, x, y, z);
    }
    for (int i = 0; i < 1; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(128);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generateCactus(view, random, x, y, z);
    }

    // ---- liquid springs -------------------------------------------------
    // Water fifty times and lava twenty, and **the two y distributions are
    // different**: water is two nested draws and lava is three, so lava is
    // pushed much further down.
    for (int i = 0; i < 50; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(random.nextInt(120) + 8);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generateLiquidSpring(view, kFlowingWater, x, y, z);
    }
    for (int i = 0; i < 20; ++i) {
        const i32 x = blockX + random.nextInt(16) + 8;
        const i32 y = random.nextInt(random.nextInt(random.nextInt(112) + 8) + 8);
        const i32 z = blockZ + random.nextInt(16) + 8;
        generateLiquidSpring(view, kFlowingLava, x, y, z);
    }

    // ---- snow -----------------------------------------------------------
    // A 16x16 sweep over the chunk, **offset by +8 like the plants**, so it
    // covers the same quarter-shifted square the rest of population works on
    // rather than the chunk itself. Draws nothing.
    if (provider.options().snowCovered) {
        for (i32 dx = 0; dx < 16; ++dx) {
            for (i32 dz = 0; dz < 16; ++dz) {
                const i32 x = blockX + dx + 8;
                const i32 z = blockZ + dz + 8;
                const i32 y = topSolidOrLiquid(view, x, z);
                if (y <= 0 || y >= 128) {
                    continue;
                }
                if (view.blockAt(x, y, z) != kAir) {
                    continue;
                }
                const u8 below = view.blockAt(x, y - 1, z);
                if (!block::def(below).solid) {
                    continue;
                }
                // **Never on ice**, which is the one material excluded by
                // name. Snow over a frozen lake would look right and is not
                // what the original does.
                if (below == kIce) {
                    continue;
                }
                view.setBlock(x, y, z, kSnowLayer);
            }
        }
    }
}

}  // namespace mc::worldgen
