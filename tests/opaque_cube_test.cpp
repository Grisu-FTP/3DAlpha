#include "framework.hpp"

#include "core/block/registry.hpp"
#include "opaque_cube_vectors.hpp"

using namespace mc;
using mc::test::kOpaqueCube;

// `Block.opaqueCubeLookup` as a loaded jar reports it, against our own column.
//
// The two columns this pins apart are easy to conflate and the consequence is
// invisible: `opaque` is what `isOpaqueCube()` answers at runtime and drives
// face culling in the mesher, while `opaqueCube` is the array world generation
// reads. They differ for leaves, because the live answer is `!fancyGraphics`
// and a1.1.2's video options default to fancy.
TEST(the_block_table_matches_the_jars_opaque_cube_array)
{
    for (int id = 0; id < 256; ++id) {
        const block::BlockDef& def = block::def(block::BlockId(id));
        if (!kOpaqueCube[id].defined) {
            // Ids this version never constructs. Air is the one that is still
            // `known` to us -- it has no Block object in the jar at all, which
            // is why the array says nothing about it -- and everything else
            // gets our deliberately visible unknown block.
            CHECK(id == 0 || !def.known);
            continue;
        }
        CHECK(def.known);
        CHECK_EQ(int(def.opaqueCube), int(kOpaqueCube[id].cached));
    }
}

// And the claim that the two columns are not interchangeable, stated as a fact
// about this version rather than as a comment. If a future extraction quietly
// made them equal, every mushroom on a leaf block would stop generating and
// nothing else would say so.
//
// The case above already pins `opaqueCube` to the jar value by value, so this
// only has to show the other column is genuinely different -- two blocks here,
// leaves and the double slab, for two unrelated reasons.
TEST(opaque_and_opaque_cube_disagree_exactly_where_the_jar_does)
{
    int disagreements = 0;
    for (int id = 0; id < 256; ++id) {
        const block::BlockDef& def = block::def(block::BlockId(id));
        if (!def.known) {
            continue;
        }
        if (def.opaque != def.opaqueCube) {
            ++disagreements;
        }
    }
    CHECK(disagreements > 0);
}
