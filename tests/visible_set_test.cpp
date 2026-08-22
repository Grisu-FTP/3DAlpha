#include "framework.hpp"

#include "core/render/visible_set.hpp"

#include <set>

using namespace mc;
using mesh::SectionVisibility;
using render::SectionField;
using render::VisibleSection;
using render::VisibleSet;

namespace {

constexpr int kSectionsY = SectionField::kSectionsY;

// A frustum that accepts everything, so traversal tests measure the visibility
// graph alone. A zero matrix produces exactly this -- see frustum_test.
Frustum openFrustum()
{
    Frustum f;
    f.setFromViewProjection(Mat4{}, ClipRange::NegativeOneToOne);
    return f;
}

void fillColumn(SectionField& field, i32 cx, i32 cz, u16 mask, bool meshed)
{
    SectionVisibility masks[kSectionsY];
    for (int sy = 0; sy < kSectionsY; ++sy) {
        masks[sy] = SectionVisibility(mask);
    }
    field.setColumn(cx, cz, masks);
    for (int sy = 0; sy < kSectionsY; ++sy) {
        // Slot 0 stands in for "holds geometry"; the walk only ever compares
        // against the two sentinels.
        field.setMeshSlot(cx, sy, cz, meshed ? u16(0) : SectionField::kNoMesh);
    }
}

// Fills every column within the radius.
void fillAll(SectionField& field, u16 mask, bool meshed)
{
    const int r = field.radius();
    for (i32 dz = -r; dz <= r; ++dz) {
        for (i32 dx = -r; dx <= r; ++dx) {
            fillColumn(field, field.centreX() + dx, field.centreZ() + dz, mask, meshed);
        }
    }
}

std::set<std::pair<i32, i32>> columnsOf(const std::vector<VisibleSection>& list)
{
    std::set<std::pair<i32, i32>> out;
    for (const VisibleSection& s : list) {
        out.insert({s.chunkX, s.chunkZ});
    }
    return out;
}

bool contains(const std::vector<VisibleSection>& list, i32 cx, int sy, i32 cz)
{
    for (const VisibleSection& s : list) {
        if (s.chunkX == cx && s.chunkZ == cz && s.sectionY == sy) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(the_field_addresses_columns_by_absolute_coordinate)
{
    SectionField field;
    field.reset(2);
    field.setCentre(100, -100);

    CHECK_EQ(field.edge(), 5);
    CHECK(field.inRange(100, -100));
    CHECK(field.inRange(102, -98));
    CHECK(!field.inRange(103, -100));
    CHECK(!field.inRange(100, -103));

    CHECK(!field.isLoaded(100, -100));
    fillColumn(field, 100, -100, mesh::kVisibilityAll, false);
    CHECK(field.isLoaded(100, -100));
    CHECK_EQ(field.visibility(100, 0, -100).mask(), mesh::kVisibilityAll);
}

TEST(a_wrapped_cell_does_not_answer_for_the_column_that_used_to_hold_it)
{
    // The grid wraps modulo its edge, so column 0 and column 5 share a cell on
    // a radius-2 field. Reading one must never return the other's data --
    // that would draw a chunk of terrain from the far side of the world.
    SectionField field;
    field.reset(2);
    field.setCentre(0, 0);
    fillColumn(field, 0, 0, mesh::kVisibilityAll, true);

    CHECK(field.isLoaded(0, 0));

    field.setCentre(5, 0);
    CHECK(!field.inRange(0, 0));
    CHECK(field.inRange(5, 0));

    // Cell (0 mod 5) is the same cell, but it still holds column 0.
    CHECK(!field.isLoaded(5, 0));
    CHECK_EQ(field.visibility(5, 0, 0).mask(), u16(0));
    CHECK(!field.hasMesh(5, 0, 0));
}

TEST(a_column_moving_into_a_cell_brings_no_meshes_with_it)
{
    SectionField field;
    field.reset(2);
    field.setCentre(0, 0);
    fillColumn(field, 0, 0, mesh::kVisibilityAll, true);
    CHECK(field.hasMesh(0, 3, 0));

    // Republishing the same column keeps its meshes: this is what a neighbour
    // arriving or a light update does, and rebuilding every mesh then would be
    // ruinous.
    SectionVisibility masks[kSectionsY];
    for (int sy = 0; sy < kSectionsY; ++sy) {
        masks[sy] = SectionVisibility(mesh::kVisibilityAll);
    }
    field.setColumn(0, 0, masks);
    CHECK(field.hasMesh(0, 3, 0));

    // A different column landing in the same cell does not.
    field.setCentre(5, 0);
    field.setColumn(5, 0, masks);
    CHECK(!field.hasMesh(5, 3, 0));
}

TEST(an_open_world_is_walked_to_the_edge_of_the_field)
{
    SectionField field;
    field.reset(3);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, true);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    // 7x7 columns of 8 sections each, all reachable and all meshed.
    CHECK_EQ(set.draw.size(), usize(7 * 7 * kSectionsY));
    CHECK_EQ(set.toMesh.size(), usize(0));
    CHECK_EQ(columnsOf(set.draw).size(), usize(49));
}

TEST(sections_the_walk_never_reaches_are_never_queued_for_meshing)
{
    // The finding that shaped this file: 73 % of a real world's geometry is
    // underground. A solid section stops the search, so everything sealed
    // behind it stays unmeshed and costs no VBO.
    SectionField field;
    field.reset(2);
    field.setCentre(0, 0);

    // Every column open above y=64 (sections 4..7) and solid below.
    SectionVisibility masks[kSectionsY];
    for (int sy = 0; sy < kSectionsY; ++sy) {
        masks[sy] = SectionVisibility(sy >= 4 ? mesh::kVisibilityAll : 0);
    }
    for (i32 dz = -2; dz <= 2; ++dz) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            field.setColumn(dx, dz, masks);
        }
    }

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 6, 0, &set);

    // The four open section layers across 5x5 columns, plus the one solid
    // layer immediately below them, which is reached (and drawn -- you can see
    // the ground) but not passed through.
    CHECK_EQ(set.toMesh.size(), usize(5 * 5 * 5));

    for (const VisibleSection& s : set.toMesh) {
        CHECK(s.sectionY >= 3);
    }

    // Nothing from the deep sections was queued at all.
    CHECK(!contains(set.toMesh, 0, 0, 0));
    CHECK(!contains(set.toMesh, 0, 2, 0));
}

TEST(a_solid_world_stops_at_the_camera_and_its_six_neighbours)
{
    // Buried in stone, the walk reaches exactly the section the camera is in
    // plus the six it touches, and goes no further.
    //
    // Seven rather than one is deliberate. The camera's own section is left by
    // any face, because its mask says which faces connect to each other and
    // not which air volume the player is standing in -- consulting it would
    // black out the world for anyone standing in a pocket that reaches only
    // one face. Those six neighbours are entered with an entry face, their
    // masks are 0, and the search dies there.
    SectionField field;
    field.reset(3);
    field.setCentre(0, 0);
    fillAll(field, 0, true);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK_EQ(set.draw.size(), usize(7));
    CHECK(contains(set.draw, 0, 4, 0));
    for (const VisibleSection& s : set.draw) {
        const int dx = s.chunkX < 0 ? -s.chunkX : s.chunkX;
        const int dz = s.chunkZ < 0 ? -s.chunkZ : s.chunkZ;
        const int dy = s.sectionY > 4 ? s.sectionY - 4 : 4 - s.sectionY;
        CHECK_EQ(dx + dy + dz <= 1, true);
    }
}

TEST(a_corridor_is_followed_and_its_walls_are_not)
{
    // Every column passes only along X. The walk should run east and west from
    // the camera and never turn.
    SectionField field;
    field.reset(3);
    field.setCentre(0, 0);

    const u16 alongX = u16(1u << mesh::visibilityBit(mesh::kFaceNegX, mesh::kFacePosX));
    fillAll(field, alongX, true);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    // Seven columns along the row z=0, plus the four neighbours the camera's
    // own section is allowed to step into regardless of its mask.
    CHECK_EQ(set.draw.size(), usize(7 + 4));

    CHECK(contains(set.draw, -3, 4, 0));
    CHECK(contains(set.draw, 3, 4, 0));

    // The corridor never turns: nothing two steps off the row exists.
    CHECK(!contains(set.draw, 1, 4, 1));
    CHECK(!contains(set.draw, 1, 5, 0));
    CHECK(!contains(set.draw, 0, 4, 2));
    CHECK(!contains(set.draw, 0, 6, 0));
}

TEST(the_camera_section_is_kept_even_when_the_frustum_rejects_it)
{
    // You are standing inside it and the near plane cuts through it. Dropping
    // it puts a hole under the player's feet.
    SectionField field;
    field.reset(1);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, true);

    Frustum nothing;
    for (int i = 0; i < Frustum::kPlaneCount; ++i) {
        // A plane whose interior is empty: everything is behind it.
        nothing.setPlane(i, Plane{0.0f, 0.0f, 1.0f, -1e9f});
    }

    VisibleSet set;
    render::buildVisibleSet(field, nothing, 0, 4, 0, &set);

    CHECK_EQ(set.draw.size(), usize(1));
    CHECK_EQ(int(set.draw[0].sectionY), 4);
    CHECK(set.rejectedByFrustum > 0);
}

TEST(the_walk_does_not_escape_into_unloaded_columns)
{
    // Only the middle column exists. The search must not queue meshes for
    // columns that were never loaded, and must not run to the field's edge.
    SectionField field;
    field.reset(4);
    field.setCentre(0, 0);
    fillColumn(field, 0, 0, mesh::kVisibilityAll, false);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK_EQ(columnsOf(set.toMesh).size(), usize(1));
    CHECK_EQ(set.toMesh.size(), usize(kSectionsY));
    CHECK_EQ(set.draw.size(), usize(0));
}

TEST(an_unloaded_camera_column_still_lets_the_walk_start)
{
    // Standing in a chunk that has not arrived yet is ordinary during
    // streaming. The world around it must still draw.
    SectionField field;
    field.reset(2);
    field.setCentre(0, 0);
    fillColumn(field, 1, 0, mesh::kVisibilityAll, true);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK(contains(set.draw, 1, 4, 0));
    CHECK(!contains(set.draw, 0, 4, 0));
}

TEST(the_mesh_queue_is_ordered_nearest_first)
{
    // A caller that can afford three meshes this frame takes the first three,
    // so the order is the whole contract.
    SectionField field;
    field.reset(4);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, false);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK(set.toMesh.size() > usize(0));
    CHECK_EQ(set.toMesh[0].chunkX, 0);
    CHECK_EQ(set.toMesh[0].chunkZ, 0);
    CHECK_EQ(int(set.toMesh[0].sectionY), 4);

    // Breadth-first over the section graph, whose edges are the six faces, so
    // the invariant is Manhattan distance in sections -- not distance in
    // columns, which a vertical step leaves unchanged. It must never decrease.
    int previous = 0;
    for (const VisibleSection& s : set.toMesh) {
        const int dx = s.chunkX < 0 ? -s.chunkX : s.chunkX;
        const int dz = s.chunkZ < 0 ? -s.chunkZ : s.chunkZ;
        const int dy = s.sectionY > 4 ? s.sectionY - 4 : 4 - s.sectionY;
        const int distance = dx + dy + dz;
        CHECK(distance >= previous);
        previous = distance;
    }
}

TEST(the_draw_list_is_ordered_nearest_first_too)
{
    // The translucent pass walks this list **backwards** to get far-to-near,
    // which is the whole of its sort -- see Renderer::drawPass. That makes the
    // draw list's order load-bearing for correctness and not just for
    // early-depth throughput, so it is pinned here rather than assumed at the
    // one call site that depends on it.
    SectionField field;
    field.reset(4);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, true);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK(set.draw.size() > usize(1));
    CHECK_EQ(set.draw[0].chunkX, 0);
    CHECK_EQ(set.draw[0].chunkZ, 0);
    CHECK_EQ(int(set.draw[0].sectionY), 4);

    int previous = 0;
    for (const VisibleSection& s : set.draw) {
        const int dx = s.chunkX < 0 ? -s.chunkX : s.chunkX;
        const int dz = s.chunkZ < 0 ? -s.chunkZ : s.chunkZ;
        const int dy = s.sectionY > 4 ? s.sectionY - 4 : 4 - s.sectionY;
        const int distance = dx + dy + dz;
        CHECK(distance >= previous);
        previous = distance;
    }
}

TEST(meshed_and_unmeshed_sections_land_in_different_lists)
{
    SectionField field;
    field.reset(1);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, false);

    // Give one column its meshes.
    for (int sy = 0; sy < kSectionsY; ++sy) {
        field.setMeshSlot(1, sy, 0, u16(sy));
    }

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK_EQ(set.draw.size(), usize(kSectionsY));
    CHECK_EQ(columnsOf(set.draw).size(), usize(1));
    CHECK(contains(set.draw, 1, 4, 0));
    CHECK(!contains(set.toMesh, 1, 4, 0));
    CHECK(contains(set.toMesh, 0, 4, 0));

    // The draw list carries the slot, so the render loop needs no second
    // lookup per section.
    for (const VisibleSection& s : set.draw) {
        CHECK_EQ(s.slot, u16(s.sectionY));
    }
}

// 36 % of a real world's sections are uniform air and another 15 % of the ones
// worth meshing come out with no geometry. Without a state for that they would
// be indistinguishable from unmeshed and re-enter the queue every frame.
TEST(a_section_meshed_to_nothing_is_neither_drawn_nor_queued)
{
    SectionField field;
    field.reset(1);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, false);

    for (int sy = 0; sy < kSectionsY; ++sy) {
        field.setMeshSlot(1, sy, 0, SectionField::kEmptyMesh);
    }

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    CHECK(!contains(set.draw, 1, 4, 0));
    CHECK(!contains(set.toMesh, 1, 4, 0));

    // Empty means air, so the walk still goes through it -- the column beyond
    // is reached.
    CHECK(contains(set.toMesh, 1, 4, 1));
}

TEST(every_section_is_visited_at_most_once)
{
    SectionField field;
    field.reset(3);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, true);

    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);

    std::set<std::tuple<i32, i32, int>> seen;
    for (const VisibleSection& s : set.draw) {
        const auto key = std::make_tuple(s.chunkX, s.chunkZ, int(s.sectionY));
        CHECK(seen.find(key) == seen.end());
        seen.insert(key);
    }
    CHECK_EQ(set.sectionsVisited, int(set.draw.size()));
}

TEST(a_camera_outside_the_world_height_is_clamped_not_rejected)
{
    // Flying above the build limit is possible and the world still has to
    // render. So does standing below y=0 after a teleport gone wrong.
    SectionField field;
    field.reset(1);
    field.setCentre(0, 0);
    fillAll(field, mesh::kVisibilityAll, true);

    VisibleSet high;
    render::buildVisibleSet(field, openFrustum(), 0, 99, 0, &high);
    CHECK(high.draw.size() > usize(0));

    VisibleSet low;
    render::buildVisibleSet(field, openFrustum(), 0, -5, 0, &low);
    CHECK(low.draw.size() > usize(0));
}

TEST(a_field_that_was_never_sized_produces_nothing_rather_than_reading_memory)
{
    SectionField field;
    VisibleSet set;
    render::buildVisibleSet(field, openFrustum(), 0, 4, 0, &set);
    CHECK_EQ(set.draw.size(), usize(0));
    CHECK_EQ(set.toMesh.size(), usize(0));
}
