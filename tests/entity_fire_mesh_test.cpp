// The flames on a burning entity: how many sheets a shape gets, where the stack
// stands relative to the entity and the camera, and that only a mob whose fire
// counter is running gets any.
//
// See core/render/entity_fire_mesh.hpp for the eight numbers this pins.

#include "core/render/entity_fire_mesh.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/texture/texture_fx.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::MobSystem;
using mc::entity::MobType;
using mc::render::buildEntityFire;
using mc::render::buildEntityFires;
using mc::render::FireScene;
using mc::render::entityFireLayers;
using mc::render::fireFacing;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A zombie's box, which is the one shape in a1.1.2 that gets three sheets.
constexpr float kTallWidth = 0.6f;
constexpr float kTallHeight = 1.8f;

// The tile the flame lives in, asked the way the builder asks for it.
int flame() { return texture::flameTile(0); }

// The mob pass's units, which is what a flame vertex holds: 1/256 of a block.
float blocks(i16 units) { return float(units) / float(mc::render::kEntityUnitsPerBlock); }

// How far a sheet's corner may be from the origin: 125 blocks.
constexpr float kReach = 32000.0f / float(mc::render::kEntityUnitsPerBlock);

bool near(float a, float b, float tolerance = 0.002f)
{
    return std::fabs(a - b) <= tolerance;
}

// A field of grass with one animal on it, for the system-level half.
struct Field {
    SceneWorld scene{0, 0};
    MobSystem mobs{7777};

    Field()
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 8; ++z) {
                scene.place(x, 62, z, bid(mcver::Block::Dirt), 0);
                scene.place(x, 63, z, bid(mcver::Block::Grass), 0);
            }
        }
        scene.lightColumnsFrom(-8, 8, -8, 8, 64);
    }

    int add(MobType type, double x, double z)
    {
        if (!mobs.spawn(scene.w(), type, x, 64.0, z, 0.0f)) {
            return -1;
        }
        return mobs.count() - 1;
    }
};

}  // namespace

TEST(fire_sheets_are_ceil_height_over_width)
{
    // `while (f9 > 0) { f9 -= 1; }` on `height / width`.
    CHECK_EQ(entityFireLayers(kTallWidth, kTallHeight), 3);  // zombie, skeleton
    CHECK_EQ(entityFireLayers(0.9f, 0.9f), 1);               // pig, cow, sheep
    CHECK_EQ(entityFireLayers(0.3f, 0.4f), 2);               // chicken
    CHECK_EQ(entityFireLayers(1.4f, 0.9f), 1);               // spider, wider than tall

    // Bounded, unlike the original's loop, because a dimension out of a data
    // file could be anything at all.
    CHECK_EQ(entityFireLayers(0.01f, 100.0f), mc::render::kEntityFireMaxLayers);
    CHECK_EQ(entityFireLayers(0.0f, 1.8f), 1);
    CHECK_EQ(entityFireLayers(0.6f, 0.0f), 0);
}

TEST(a_fire_sheet_stands_on_the_entity_and_leans_toward_the_camera)
{
    if (flame() < 0) {
        return;  // a version with no fire block draws none of this
    }

    // Yaw zero: the local axes are the world's, so every number below can be
    // read straight off the class file.
    std::vector<mesh::DetailVertex> verts(size_t(mc::render::kEntityFireMaxVertices));
    const int written = buildEntityFire(0.0, 0.0, 0.0, kTallWidth, kTallHeight, flame(),
                                        fireFacing(0.0f), verts.data(), int(verts.size()));
    CHECK_EQ(written, 3 * 4);

    const float scale = kTallWidth * mc::render::kEntityFireScale;  // 0.84

    // The first sheet: x from -0.5 to +0.5 of the scale, y from the entity's
    // feet to 1.4 of it, and the whole quad flat in z.
    CHECK(near(blocks(verts[0].x), 0.5f * scale));
    CHECK(near(blocks(verts[1].x), -0.5f * scale));
    CHECK(near(blocks(verts[0].y), 0.0f));
    CHECK(near(blocks(verts[2].y), 1.4f * scale));
    for (int c = 1; c < 4; ++c) {
        CHECK_EQ(verts[c].z, verts[0].z);
    }

    // `-0.4 + (int)(1.8F / 0.6F) * 0.02`, and **that truncates to 2, not 3**:
    // the quotient of the two floats is 2.99999976, so the stack of three
    // sheets is placed as though there were two. Java divides the same floats
    // and gets the same answer. Negative local z is the camera's side -- a
    // camera at yaw zero looks along +z -- so the sheets sit between it and
    // the mob rather than inside the mob.
    CHECK(near(blocks(verts[0].z), -0.36f * scale));

    // The second sheet: one unit up, a tenth narrower on its +x edge only, and
    // 0.04 nearer the camera.
    CHECK(near(blocks(verts[4].y), 1.0f * scale));
    CHECK(near(blocks(verts[4].x), (0.9f - 0.5f) * scale));
    CHECK(near(blocks(verts[5].x), -0.5f * scale));
    CHECK(near(blocks(verts[4].z), -0.40f * scale));

    // Third sheet, and the shrink compounds rather than stepping.
    CHECK(near(blocks(verts[8].x), (0.81f - 0.5f) * scale));
    CHECK(near(blocks(verts[8].y), 2.0f * scale));
}

TEST(fire_is_full_bright_white_and_off_one_tile)
{
    if (flame() < 0) {
        return;
    }
    std::vector<mesh::DetailVertex> verts(size_t(mc::render::kEntityFireMaxVertices));
    const int written = buildEntityFire(0.0, 0.0, 0.0, kTallWidth, kTallHeight, flame(),
                                        fireFacing(0.0f), verts.data(), int(verts.size()));
    CHECK(written > 0);

    const int column = flame() % mesh::kAtlasTilesPerEdge;
    const int row = flame() / mesh::kAtlasTilesPerEdge;
    for (int v = 0; v < written; ++v) {
        // `glDisable(GL_LIGHTING)` and `glColor4f(1, 1, 1, 1)`.
        CHECK_EQ(int(verts[v].light), 0xFF);
        CHECK_EQ(int(verts[v].r), 0xFF);
        CHECK_EQ(int(verts[v].g), 0xFF);
        CHECK_EQ(int(verts[v].b), 0xFF);

        // **One tile for the whole stack**, which is what a1.1.2 does and what
        // later versions stopped doing.
        CHECK(verts[v].u >= mesh::tileUvMin(column) && verts[v].u <= mesh::tileUvMax(column));
        CHECK(verts[v].v >= mesh::tileUvMin(row) && verts[v].v <= mesh::tileUvMax(row));
    }

    // The tile's bottom row goes on the bottom of the sheet.
    CHECK_EQ(verts[0].v, mesh::tileUvMax(row));
    CHECK_EQ(verts[2].v, mesh::tileUvMin(row));
}

TEST(the_stack_turns_with_the_camera)
{
    if (flame() < 0) {
        return;
    }
    std::vector<mesh::DetailVertex> verts(size_t(mc::render::kEntityFireMaxVertices));

    // A camera at yaw 90 looks along -x, so the sheet's width now lies along z
    // and the lean toward the camera is along +x.
    const int written = buildEntityFire(0.0, 0.0, 0.0, kTallWidth, kTallHeight, flame(),
                                        fireFacing(90.0f), verts.data(), int(verts.size()));
    CHECK(written >= 4);

    const float scale = kTallWidth * mc::render::kEntityFireScale;
    CHECK(near(blocks(verts[0].z), 0.5f * scale));
    CHECK(near(blocks(verts[1].z), -0.5f * scale));
    CHECK(near(blocks(verts[0].x), 0.36f * scale));
    CHECK(near(blocks(verts[0].y), 0.0f));
}

TEST(a_sheet_that_does_not_fit_the_position_is_dropped)
{
    if (flame() < 0) {
        return;
    }
    std::vector<mesh::DetailVertex> verts(size_t(mc::render::kEntityFireMaxVertices));

    // A flame position reaches 125 blocks and a zombie's stack is nearly
    // three tall, so a mob at the top of that window has sheets that cannot be
    // written. They are dropped one at a time; the ones that fit are still
    // drawn, and nothing wraps round to the far side of the world.
    const int high = buildEntityFire(0.0, double(kReach) - 1.25, 0.0, kTallWidth, kTallHeight,
                                     flame(), fireFacing(0.0f), verts.data(),
                                     int(verts.size()));
    CHECK_EQ(high, 4);  // the first sheet clears the top; the two above it do not
    for (int v = 0; v < high; ++v) {
        CHECK(blocks(verts[v].y) >= kReach - 1.35f && blocks(verts[v].y) <= kReach);
    }

    // The same mob at the bottom of the window keeps its *upper* sheets, which
    // is why the builder drops a sheet rather than stopping the stack.
    const int low = buildEntityFire(0.0, -double(kReach) - 0.25, 0.0, kTallWidth, kTallHeight,
                                    flame(), fireFacing(0.0f), verts.data(),
                                    int(verts.size()));
    CHECK_EQ(low, 2 * 4);
    for (int v = 0; v < low; ++v) {
        CHECK(blocks(verts[v].y) >= -kReach);
    }
}

TEST(only_a_burning_mob_gets_flames)
{
    if (flame() < 0) {
        return;
    }
    Field field;
    const int pig = field.add(MobType::Pig, 0.0, 0.0);
    CHECK(pig >= 0);

    FireScene scene;
    scene.mobs = &field.mobs;

    std::vector<mesh::DetailVertex> verts(size_t(mc::render::kEntityFireMaxVertices));
    CHECK_EQ(buildEntityFires(scene, 0.0f, 0.0, 64.0, 0.0, 1.0f, verts.data(),
                              int(verts.size())),
             0);

    // A pig is as wide as it is tall, so it gets the one sheet -- and it gets
    // it at its feet, which is where `body.posY` is for a mob.
    field.mobs.at(pig).fire = 40;
    const int written = buildEntityFires(scene, 0.0f, 0.0, 64.0, 0.0, 1.0f, verts.data(),
                                         int(verts.size()));
    CHECK_EQ(written, 4);
    CHECK(near(blocks(verts[0].y), 0.0f));

    // And it stops with the counter, not with the damage.
    field.mobs.at(pig).fire = 0;
    CHECK_EQ(buildEntityFires(scene, 0.0f, 0.0, 64.0, 0.0, 1.0f, verts.data(),
                              int(verts.size())),
             0);
}
