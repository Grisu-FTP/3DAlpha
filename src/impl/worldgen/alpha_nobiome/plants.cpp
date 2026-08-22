#include "impl/worldgen/alpha_nobiome/plants.hpp"

#include "blocks.hpp"
#include "core/block/registry.hpp"
#include "core/util/java_random.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kGrass = u8(mcver::Block::Grass);
constexpr u8 kDirt = u8(mcver::Block::Dirt);
constexpr u8 kSand = u8(mcver::Block::Sand);
constexpr u8 kWater = u8(mcver::Block::Water);
constexpr u8 kCactus = u8(mcver::Block::Cactus);
constexpr u8 kReed = u8(mcver::Block::SugarCane);

// By material rather than by id, exactly as the original does -- so flowing
// water counts as well as still, and ice does not.
bool isWaterMaterial(u8 id)
{
    return block::def(id).material == block::def(kWater).material;
}

// `Material.isSolid()`, which the block table already carries as its own column
// because it is not derivable from `opaque` or `fullCube`. Cactus asks this of
// its four horizontal neighbours.
bool isSolid(u8 id)
{
    return block::def(id).solid;
}

// **Two draws whose order is the whole point.**
//
// The original writes `x + rand.nextInt(n) - rand.nextInt(n)`, and Java
// evaluates that strictly left to right. **C++ does not**: the order in which
// the operands of `-` are evaluated is unspecified, even in C++17, so writing
// the expression the same way would let the compiler swap the two draws. It
// would still produce a plausible plant distribution, on one compiler, and a
// different world on another.
//
// Naming both draws is what fixes the order. Every place in this file that
// perturbs a coordinate goes through here.
i32 perturb(JavaRandom& random, i32 base, i32 bound)
{
    const i32 plus = random.nextInt(bound);
    const i32 minus = random.nextInt(bound);
    return base + plus - minus;
}

}  // namespace

bool reedCanStay(const PopulationView& view, i32 x, i32 y, i32 z)
{
    // `jm.g` just forwards to `jm.a`, so there is one predicate and not two.
    const u8 below = view.blockAt(x, y - 1, z);

    // Stacking on itself, checked before the ground test -- which is what lets
    // a reed grow three tall from a single shoreline block.
    if (below == kReed) {
        return true;
    }
    if (below != kGrass && below != kDirt) {
        return false;
    }

    // Water beside the block *below*, not beside the reed itself. A reed at the
    // waterline has its base level with the water, so testing at the reed's own
    // height would find air and refuse every time.
    if (isWaterMaterial(view.blockAt(x - 1, y - 1, z))) {
        return true;
    }
    if (isWaterMaterial(view.blockAt(x + 1, y - 1, z))) {
        return true;
    }
    if (isWaterMaterial(view.blockAt(x, y - 1, z - 1))) {
        return true;
    }
    if (isWaterMaterial(view.blockAt(x, y - 1, z + 1))) {
        return true;
    }
    return false;
}

bool cactusCanStay(const PopulationView& view, i32 x, i32 y, i32 z)
{
    // Any solid neighbour refuses, which is why cactus never grows against a
    // wall. Material solidity, not opacity: glass would block it too.
    if (isSolid(view.blockAt(x - 1, y, z))) {
        return false;
    }
    if (isSolid(view.blockAt(x + 1, y, z))) {
        return false;
    }
    if (isSolid(view.blockAt(x, y, z - 1))) {
        return false;
    }
    if (isSolid(view.blockAt(x, y, z + 1))) {
        return false;
    }

    const u8 below = view.blockAt(x, y - 1, z);
    return below == kCactus || below == kSand;
}

bool generateReeds(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z)
{
    for (int i = 0; i < 20; ++i) {
        const i32 px = perturb(random, x, 4);
        // **y is not perturbed.** Cactus perturbs all three; reeds take the
        // height the driver picked. Adding a y draw here would consume two
        // extra numbers per try, twenty times a chunk.
        const i32 pz = perturb(random, z, 4);

        if (view.blockAt(px, y, pz) != kAir) {
            continue;
        }

        // The water test happens here as well as inside canStay, and it is not
        // redundant: this one gates the *height draw*, so a spot with no water
        // beside it costs zero further random numbers. Folding the two together
        // would change the stream.
        const bool nearWater = isWaterMaterial(view.blockAt(px - 1, y - 1, pz)) ||
                               isWaterMaterial(view.blockAt(px + 1, y - 1, pz)) ||
                               isWaterMaterial(view.blockAt(px, y - 1, pz - 1)) ||
                               isWaterMaterial(view.blockAt(px, y - 1, pz + 1));
        if (!nearWater) {
            continue;
        }

        // Nested, so short reeds are much more likely than tall ones: two, plus
        // a draw bounded by a draw.
        const i32 height = 2 + random.nextInt(random.nextInt(3) + 1);
        for (i32 k = 0; k < height; ++k) {
            if (reedCanStay(view, px, y + k, pz)) {
                view.setBlock(px, y + k, pz, kReed);
            }
        }
    }
    return true;
}

bool generateCactus(PopulationView& view, JavaRandom& random, i32 x, i32 y, i32 z)
{
    for (int i = 0; i < 10; ++i) {
        // Note the bounds: 8 for the horizontals and 4 for the vertical, so a
        // cactus can land seven blocks away but only three up or down.
        const i32 px = perturb(random, x, 8);
        const i32 py = perturb(random, y, 4);
        const i32 pz = perturb(random, z, 8);

        if (view.blockAt(px, py, pz) != kAir) {
            continue;
        }

        // One shorter than a reed at the same roll -- `1 +` rather than `2 +`.
        const i32 height = 1 + random.nextInt(random.nextInt(3) + 1);
        for (i32 k = 0; k < height; ++k) {
            // Re-tested at every level rather than once, so a cactus stops
            // where it meets an obstruction instead of growing through it --
            // and note it keeps *going* after a refusal rather than breaking,
            // which is what the original does.
            if (cactusCanStay(view, px, py + k, pz)) {
                view.setBlock(px, py + k, pz, kCactus);
            }
        }
    }
    return true;
}

}  // namespace mc::worldgen
