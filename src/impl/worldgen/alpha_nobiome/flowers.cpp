#include "impl/worldgen/alpha_nobiome/flowers.hpp"

#include "blocks.hpp"
#include "core/block/registry.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kGrass = u8(mcver::Block::Grass);
constexpr u8 kDirt = u8(mcver::Block::Dirt);
constexpr u8 kFarmland = u8(mcver::Block::Farmland);

// The same two-named-draws helper the reeds and cactus need, and for the same
// reason: `x + rand.nextInt(n) - rand.nextInt(n)` is left-to-right in Java and
// unsequenced in C++, so writing it as one expression would let the compiler
// swap the draws and quietly generate a different world.
i32 perturb(JavaRandom& random, i32 base, i32 bound)
{
    const i32 plus = random.nextInt(bound);
    const i32 minus = random.nextInt(bound);
    return base + plus - minus;
}

}  // namespace

bool flowerCanStay(const PopulationView& view, i32 x, i32 y, i32 z)
{
    // Bright enough, **or** open to the sky. Not both: a flower in a cave lit
    // by nothing but lava still counts, and one on a shaded hillside that can
    // see sky counts too.
    if (view.lightAt(x, y, z) < 8 && !view.canSeeSky(x, y, z)) {
        return false;
    }
    const u8 below = view.blockAt(x, y - 1, z);
    return below == kGrass || below == kDirt || below == kFarmland;
}

bool mushroomCanStay(const PopulationView& view, i32 x, i32 y, i32 z)
{
    // **The opposite test, and there is no sky clause at all.** A mushroom
    // refuses anywhere brighter than 13, which in a freshly generated chunk
    // means it cannot be planted anywhere open -- everything at or above the
    // height map reads 15. Caves and the shade under a canopy are the whole of
    // its habitat.
    //
    // Mutating the bound to 12 fails the fixture; mutating it to 14 does not,
    // and that second one is an **equivalent mutant** rather than a gap. A cell
    // a mushroom could stand in has to be air, and no air cell can read 14:
    // the height map stops at the first block with any opacity at all, so the
    // cell at height - 1 is never air, and reaching an air cell costs that
    // block's opacity plus one more. 15 minus at least two is at most 13. The
    // only other value a plantable cell can hold is 15, at or above the height
    // map. So 14 never occurs and the two bounds are the same program.
    if (view.lightAt(x, y, z) > 13) {
        return false;
    }

    // Any opaque cube, where a flower insists on grass, dirt or farmland.
    //
    // **`opaqueCube`, not `opaque`.** The original reads
    // `Block.opaqueCubeLookup`, an array filled in the Block constructor,
    // where the mesher calls the live `isOpaqueCube()`. For leaves the two
    // disagree -- the live answer is `!fancyGraphics` and a1.1.2 defaults to
    // fancy -- so a1.1.2 will grow a mushroom on a leaf block and using
    // `opaque` here would refuse it. See core/block/block_def.hpp.
    return block::def(view.blockAt(x, y - 1, z)).opaqueCube;
}

bool generatePlants(PopulationView& view, JavaRandom& random, u8 plantId, i32 x, i32 y, i32 z)
{
    // Which predicate applies is decided by the id, exactly as the original
    // decides it by which Block subclass the id maps to.
    const bool mushroom =
        plantId == u8(mcver::Block::BrownMushroom) || plantId == u8(mcver::Block::RedMushroom);

    for (int i = 0; i < 64; ++i) {
        // Note the bounds differ per axis: 8 horizontally, 4 vertically. A
        // patch can spread seven blocks sideways but only three up or down.
        const i32 px = perturb(random, x, 8);
        const i32 py = perturb(random, y, 4);
        const i32 pz = perturb(random, z, 8);

        // Air first, and it is checked before the predicate -- so a try that
        // lands in rock costs nothing beyond the three draws above. Neither
        // branch draws, so unlike the reeds this generator consumes exactly
        // 384 numbers whatever it finds.
        if (view.blockAt(px, py, pz) != kAir) {
            continue;
        }
        if (mushroom ? mushroomCanStay(view, px, py, pz) : flowerCanStay(view, px, py, pz)) {
            view.setBlock(px, py, pz, plantId);
        }
    }
    return true;
}

}  // namespace mc::worldgen
