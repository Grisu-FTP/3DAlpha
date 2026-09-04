// Which way a block ends up facing when a player puts it down.
//
// `Block.onBlockPlaced`, checked against a real jar for every block crossed
// with every face of the block clicked. The surprise the fixture settled is
// that **the player's heading almost never matters**: in a1.1.2 stairs,
// furnaces, ladders and buttons all orient from the struck face alone, and only
// the lever consults where the player is standing.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "framework.hpp"
#include "placement_vectors.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::block::placementMetadata;

TEST(a_placed_block_faces_the_way_the_jar_says_for_every_block_and_face)
{
    for (int i = 0; i < test::kPlacementCaseCount; ++i) {
        const test::PlacementCase& c = test::kPlacementCases[i];
        for (int face = 0; face < 6; ++face) {
            // Heading 0, which is what the shipped table holds; the fixture's
            // other headings only ever differ for the lever.
            const int want = c.metadata[face][0];
            if (want < 0) {
                // The block deleted itself rather than being placed there. Our
                // table stores 0 for those, and the placement path refuses them
                // before it ever asks.
                continue;
            }
            CHECK_EQ(int(placementMetadata(BlockId(c.id), face)), want);
        }
    }
}

TEST(the_struck_face_is_what_orients_a_block_and_the_players_heading_is_not)
{
    // The measured claim, asserted so it cannot quietly stop being true: exactly
    // one block in a1.1.2 answers differently for different player headings.
    CHECK_EQ(test::kPlacementYawVaryingCount, 1);
    // And a good dozen do care about the face, so the table is not vacuous.
    CHECK(test::kPlacementSideVaryingCount >= 10);
}

TEST(a_torch_takes_the_wall_it_was_clicked_onto)
{
    // The visible case, spelled out rather than left inside the sweep. The
    // metadata numbers are the game's own and come from the fixture above.
    const BlockId torch = BlockId(mcver::Block::Torch);
    CHECK_EQ(int(placementMetadata(torch, mesh::kFacePosY)), 5);   // stood on a floor
    CHECK(placementMetadata(torch, mesh::kFaceNegZ) != 5);         // hung on a wall
    CHECK(placementMetadata(torch, mesh::kFacePosX)
          != placementMetadata(torch, mesh::kFaceNegX));           // and on which wall
}

TEST(a_block_that_does_not_care_gets_metadata_zero)
{
    for (int face = 0; face < 6; ++face) {
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::Stone), face)), 0);
        CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::Dirt), face)), 0);
    }
    // And an id this build has never heard of does not read past the table.
    CHECK_EQ(int(placementMetadata(BlockId(mcver::kPlacementTableSize - 1), 0)), 0);
    CHECK_EQ(int(placementMetadata(BlockId(mcver::Block::Stone), 99)), 0);
}
