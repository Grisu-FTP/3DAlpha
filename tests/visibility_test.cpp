#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/visibility.hpp"
#include "core/world/section.hpp"

using namespace mc;
using mesh::SectionVisibility;
using mesh::VisibilityScratch;
using world::Section;

namespace {

VisibilityScratch& scratch()
{
    static VisibilityScratch instance;
    return instance;
}

SectionVisibility visibilityOf(const Section& section)
{
    return mesh::computeVisibility(section, scratch());
}

constexpr u16 kStone = u16(mcver::Block::Stone);
constexpr u16 kGlass = u16(mcver::Block::Glass);

int connectionCount(const SectionVisibility& v)
{
    int n = 0;
    for (int a = 0; a < mesh::kFaceCount; ++a) {
        for (int b = a + 1; b < mesh::kFaceCount; ++b) {
            if (v.connects(a, b)) ++n;
        }
    }
    return n;
}

}  // namespace

TEST(the_pair_numbering_covers_every_pair_exactly_once)
{
    bool seen[mesh::kVisibilityPairCount] = {};
    int total = 0;

    for (int a = 0; a < mesh::kFaceCount; ++a) {
        for (int b = a + 1; b < mesh::kFaceCount; ++b) {
            const int bit = mesh::visibilityBit(a, b);
            CHECK(bit >= 0 && bit < mesh::kVisibilityPairCount);
            CHECK(!seen[bit]);
            seen[bit] = true;
            ++total;

            // The pair is unordered, so both directions must land on one bit.
            CHECK_EQ(mesh::visibilityBit(b, a), bit);
        }
    }

    CHECK_EQ(total, mesh::kVisibilityPairCount);
}

TEST(an_air_section_connects_everything_and_a_solid_one_connects_nothing)
{
    const Section air;
    CHECK(air.isUniformAir());
    CHECK_EQ(visibilityOf(air).mask(), mesh::kVisibilityAll);
    CHECK_EQ(connectionCount(visibilityOf(air)), 15);

    const Section stone(kStone);
    CHECK_EQ(visibilityOf(stone).mask(), u16(0));
    CHECK(visibilityOf(stone).opaqueToEverything());
    CHECK_EQ(connectionCount(visibilityOf(stone)), 0);
}

TEST(a_face_never_connects_to_itself)
{
    // The search would otherwise be free to re-enter the section it just left
    // by the face it left through, and never terminate.
    const Section air;
    for (int f = 0; f < mesh::kFaceCount; ++f) {
        CHECK(!visibilityOf(air).connects(f, f));
    }
}

TEST(a_straight_tunnel_connects_only_the_faces_it_opens_on)
{
    // Solid section with one shaft bored along Z at (8, 8). Anything entering
    // by -Z can leave by +Z and by nothing else -- this is the case that makes
    // caves cheap, and getting it wrong draws the world through a wall.
    Section section(kStone);
    for (int z = 0; z < Section::kSize; ++z) {
        section.setBlock(8, 8, z, 0);
    }

    const SectionVisibility v = visibilityOf(section);
    CHECK(v.connects(mesh::kFaceNegZ, mesh::kFacePosZ));
    CHECK_EQ(connectionCount(v), 1);

    CHECK(!v.connects(mesh::kFaceNegX, mesh::kFacePosX));
    CHECK(!v.connects(mesh::kFaceNegY, mesh::kFacePosY));
    CHECK(!v.connects(mesh::kFaceNegZ, mesh::kFacePosY));
}

TEST(two_tunnels_that_never_meet_do_not_connect_their_faces)
{
    // The whole point of flood-filling rather than testing faces for openness:
    // both the X shaft and the Y shaft reach the section's surface, but they
    // are separate volumes, so you cannot get from one to the other.
    Section section(kStone);
    for (int x = 0; x < Section::kSize; ++x) {
        section.setBlock(x, 2, 2, 0);
    }
    for (int y = 0; y < Section::kSize; ++y) {
        section.setBlock(12, y, 12, 0);
    }

    const SectionVisibility v = visibilityOf(section);
    CHECK(v.connects(mesh::kFaceNegX, mesh::kFacePosX));
    CHECK(v.connects(mesh::kFaceNegY, mesh::kFacePosY));
    CHECK_EQ(connectionCount(v), 2);

    CHECK(!v.connects(mesh::kFaceNegX, mesh::kFaceNegY));
    CHECK(!v.connects(mesh::kFacePosX, mesh::kFacePosY));
}

TEST(crossing_tunnels_connect_all_four_of_their_faces)
{
    Section section(kStone);
    for (int x = 0; x < Section::kSize; ++x) {
        section.setBlock(x, 8, 8, 0);
    }
    for (int z = 0; z < Section::kSize; ++z) {
        section.setBlock(8, 8, z, 0);
    }

    const SectionVisibility v = visibilityOf(section);
    // Four faces mutually reachable: 4 choose 2 = 6 pairs.
    CHECK_EQ(connectionCount(v), 6);
    CHECK(v.connects(mesh::kFaceNegX, mesh::kFacePosZ));
    CHECK(v.connects(mesh::kFaceNegZ, mesh::kFacePosX));
    CHECK(!v.connects(mesh::kFaceNegY, mesh::kFacePosY));
}

TEST(a_sealed_cavity_connects_nothing)
{
    // An air pocket that never reaches a wall must not open the section, or
    // every ore-sized hollow underground would let the search through.
    Section section(kStone);
    for (int x = 6; x <= 9; ++x) {
        for (int y = 6; y <= 9; ++y) {
            for (int z = 6; z <= 9; ++z) {
                section.setBlock(x, y, z, 0);
            }
        }
    }

    CHECK_EQ(visibilityOf(section).mask(), u16(0));
}

TEST(a_pocket_touching_one_wall_still_connects_nothing)
{
    // Reaching a single face is not a passage: you cannot enter and leave.
    Section section(kStone);
    for (int y = 6; y <= 9; ++y) {
        for (int z = 6; z <= 9; ++z) {
            section.setBlock(0, y, z, 0);
        }
    }

    CHECK_EQ(visibilityOf(section).mask(), u16(0));
}

TEST(transparent_blocks_are_passable_to_the_search)
{
    // The predicate has to be the mesher's: a glass tunnel is drawn through,
    // so the search must walk through it too. Disagreeing here shows up as
    // geometry that pops in and out as the camera moves.
    Section section(kStone);
    for (int z = 0; z < Section::kSize; ++z) {
        section.setBlock(8, 8, z, kGlass);
    }

    const SectionVisibility v = visibilityOf(section);
    CHECK(v.connects(mesh::kFaceNegZ, mesh::kFacePosZ));
    CHECK_EQ(connectionCount(v), 1);
}

TEST(a_uniform_transparent_section_is_open)
{
    // The uniform shortcut must consult opacity, not air. A section solid with
    // glass or leaves is see-through and has to stay traversable.
    const Section glass(kGlass);
    CHECK(glass.isUniform());
    CHECK_EQ(visibilityOf(glass).mask(), mesh::kVisibilityAll);
}

TEST(a_one_block_slab_of_air_touches_opposite_faces_at_once)
{
    // A single layer of air spans the section from -Y face to +Y face only if
    // the section is one block tall, which it is not -- but the layer does span
    // all four side faces. This is the case a naive "which wall is this cell
    // on" test gets wrong by assuming a cell touches at most one face.
    Section section(kStone);
    for (int x = 0; x < Section::kSize; ++x) {
        for (int z = 0; z < Section::kSize; ++z) {
            section.setBlock(x, 0, z, 0);
        }
    }

    const SectionVisibility v = visibilityOf(section);
    // -Y and the four sides: 5 faces mutually reachable = 10 pairs.
    CHECK_EQ(connectionCount(v), 10);
    CHECK(v.connects(mesh::kFaceNegX, mesh::kFacePosX));
    CHECK(v.connects(mesh::kFaceNegY, mesh::kFaceNegZ));
    CHECK(!v.connects(mesh::kFacePosY, mesh::kFaceNegY));
}
