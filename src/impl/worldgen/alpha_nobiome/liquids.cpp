#include "impl/worldgen/alpha_nobiome/liquids.hpp"

#include "blocks.hpp"
#include "impl/worldgen/alpha_nobiome/population_view.hpp"

namespace mc::worldgen {

namespace {

constexpr u8 kAir = u8(mcver::Block::Air);
constexpr u8 kStone = u8(mcver::Block::Stone);

}  // namespace

bool generateLiquidSpring(PopulationView& view, u8 liquidId, i32 x, i32 y, i32 z)
{
    // Sandwiched between two stone blocks. Both are early returns in the
    // original, before anything else is looked at.
    if (view.blockAt(x, y + 1, z) != kStone) {
        return false;
    }
    if (view.blockAt(x, y - 1, z) != kStone) {
        return false;
    }

    // The centre may be air or stone and nothing else. Written as the original
    // writes it -- a non-zero test, then a stone test -- rather than folded
    // into one comparison, because the two branches read the block separately
    // and a reader checking this against the bytecode should find the same
    // shape.
    const u8 centre = view.blockAt(x, y, z);
    if (centre != kAir && centre != kStone) {
        return false;
    }

    // **Two independent counts over the same four neighbours, not one pass with
    // an else.** A neighbour that is neither stone nor air -- dirt, gravel, an
    // ore -- counts toward neither, so it cannot stand in for the single
    // opening. That is what keeps springs out of the middle of a dirt patch,
    // and it is the case a hand-written fixture would most easily miss.
    int stoneSides = 0;
    stoneSides += view.blockAt(x - 1, y, z) == kStone ? 1 : 0;
    stoneSides += view.blockAt(x + 1, y, z) == kStone ? 1 : 0;
    stoneSides += view.blockAt(x, y, z - 1) == kStone ? 1 : 0;
    stoneSides += view.blockAt(x, y, z + 1) == kStone ? 1 : 0;

    int airSides = 0;
    airSides += view.blockAt(x - 1, y, z) == kAir ? 1 : 0;
    airSides += view.blockAt(x + 1, y, z) == kAir ? 1 : 0;
    airSides += view.blockAt(x, y, z - 1) == kAir ? 1 : 0;
    airSides += view.blockAt(x, y, z + 1) == kAir ? 1 : 0;

    // Three walls and one way out.
    //
    // **Both equalities are provably relaxable, and are kept exact anyway.**
    // Mutating either to `>=` survives the whole 494-case truth table, and that
    // is not a gap in the fixture: with only four sides, `stoneSides >= 3`
    // together with `airSides == 1` forces `stoneSides == 3`, and
    // `stoneSides == 3` leaves one side over so `airSides >= 1` forces
    // `airSides == 1`. The two mutants are equivalent programs. They are still
    // written the way the bytecode writes them, because the equivalence is a
    // property of "four" and would quietly stop holding if this were ever
    // reused for a shape with more neighbours.
    //
    // Mutations that are *not* equivalent -- `== 4`, dropping one of the four
    // neighbour reads, skipping the block-above check -- are all caught.
    if (stoneSides == 3 && airSides == 1) {
        // The original calls setBlockWithNotify here, where every other
        // population generator calls the plain setBlock. The difference is a
        // neighbour notification, and for a fluid that only *schedules* a tick
        // -- no block changes as a result, so the chunk that gets written to
        // disk is the same either way and one setBlock covers both. The
        // distinction is recorded because it stops being cosmetic the moment
        // fluid ticking exists.
        view.setBlock(x, y, z, liquidId);
    }

    return true;
}

}  // namespace mc::worldgen
