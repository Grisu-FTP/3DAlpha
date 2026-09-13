// The frame half of a particle: the four scale ramps, the two brightness
// overrides, the split into one span per sheet, and what the generated
// `particles.png` stand-in actually contains.

#include "core/entity/particle.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/render/particle_mesh.hpp"
#include "core/texture/particle_sheet.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Particle;
using mc::entity::ParticleKind;
using mc::entity::ParticleSheet;
using mc::entity::ParticleSystem;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

struct Scene {
    SceneWorld world{0, 0};

    Scene()
    {
        for (i32 x = -2; x <= 2; ++x) {
            for (i32 z = -2; z <= 2; ++z) {
                world.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
    }
};

// A camera looking down +x with no pitch: right is +x, up is +y.
constexpr render::Billboard kFlat{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

}  // namespace

TEST(smoke_swells_out_of_nothing_and_a_flame_shrinks_away)
{
    Scene s;
    ParticleSystem fx(101);
    fx.spawn(s.world.w(), ParticleKind::Smoke, 0.5, 70.5, 0.5);
    Particle puff = fx[0];
    puff.maxAge = 64;

    // `nl.a`: `elapsed * 32`, clamped -- so it is at nothing on the tick it is
    // made and full size a thirty-second of the way through.
    puff.age = 0;
    CHECK_EQ(render::particleQuadScale(puff, 0.0f), 0.0f);
    puff.age = 2;
    CHECK(render::particleQuadScale(puff, 0.0f)
          == render::kParticleQuadScale * puff.birthScale);
    puff.age = 60;
    CHECK(render::particleQuadScale(puff, 0.0f)
          == render::kParticleQuadScale * puff.birthScale);

    // `jb.a`: `1 - f*f*0.5`, so a flame ends at exactly half the size it
    // started and never reaches nothing.
    ParticleSystem flames(102);
    flames.spawn(s.world.w(), ParticleKind::Flame, 0.5, 70.5, 0.5);
    Particle flame = flames[0];
    flame.maxAge = 64;
    flame.age = 0;
    const float born = render::particleQuadScale(flame, 0.0f);
    flame.age = 64;
    const float spent = render::particleQuadScale(flame, 0.0f);
    CHECK(spent < born);
    CHECK(spent > born * 0.49f && spent < born * 0.51f);

    // `cq.a`: `1 - f*f`, all the way to nothing.
    ParticleSystem lava(103);
    lava.spawn(s.world.w(), ParticleKind::Lava, 0.5, 70.5, 0.5);
    Particle pop = lava[0];
    pop.maxAge = 64;
    pop.age = 64;
    CHECK_EQ(render::particleQuadScale(pop, 0.0f), 0.0f);
}

TEST(a_lava_pop_ignores_the_dark_and_the_rest_of_them_do_not)
{
    Scene s;
    ParticleSystem fx(104);
    // Deep enough to be unlit.
    fx.spawn(s.world.w(), ParticleKind::Lava, 0.5, 40.5, 0.5);
    fx.spawn(s.world.w(), ParticleKind::Smoke, 0.5, 40.5, 0.5);
    CHECK_EQ((long long) render::particleLight(fx[0], 0.5f), 255LL);
    CHECK_EQ((long long) render::particleLight(fx[1], 0.5f), (long long) fx[1].light);
}

TEST(each_sheet_gets_its_own_span_and_nothing_is_drawn_twice)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);

    ParticleSystem fx(105);
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);          // 64 on terrain.png
    fx.spawn(s.world.w(), ParticleKind::Smoke, 0.5, 64.5, 0.5);   // particles.png
    fx.spawn(s.world.w(), ParticleKind::Slime, 0.5, 64.5, 0.5);   // gui/items.png
    CHECK_EQ(fx.count(), 66);

    std::vector<mesh::DetailVertex> out(fx.count() * 4);
    render::DrawCutoff cutoff;
    int total = 0;
    int counts[3] = {};
    const ParticleSheet order[3] = {ParticleSheet::Terrain, ParticleSheet::Particles,
                                    ParticleSheet::Items};
    for (int i = 0; i < 3; ++i) {
        counts[i] = render::buildParticles(fx, kFlat, 0.0, 64.0, 0.0, 0.0f, order[i],
                                           out.data() + total,
                                           int(out.size()) - total, &cutoff);
        total += counts[i];
    }
    // Every particle written exactly once, on exactly one sheet.
    CHECK_EQ(counts[0], 64 * 4);
    CHECK_EQ(counts[1], 4);
    CHECK_EQ(counts[2], 4);
    CHECK_EQ(total, fx.count() * 4);
}

TEST(a_sprite_shows_a_whole_tile_and_a_fleck_shows_a_quarter)
{
    Scene s;
    ParticleSystem fx(106);
    fx.spawn(s.world.w(), ParticleKind::Flame, 0.5, 64.5, 0.5);

    mesh::DetailVertex quad[4];
    const int written = render::buildParticles(fx, kFlat, 0.0, 64.0, 0.0, 0.0f,
                                               ParticleSheet::Particles, quad, 4);
    CHECK_EQ(written, 4);

    // `nq.a` steps `0.0624375F` of the sheet -- just under a sixteenth -- where
    // a digging fleck steps a quarter of that. In the 1/16384 units the vertex
    // carries, that is 1023 against 255.
    const int uSpan = int(quad[2].u) - int(quad[0].u);
    CHECK_EQ((long long) uSpan, 1023LL);

    // **Low u on the `-right` corners.** Corner 0 is `pos - right - up` and
    // takes `(uLo, vHi)`, which is the pairing `renderParticle` writes; getting
    // it the other way round mirrors every sprite horizontally.
    CHECK(quad[0].u < quad[2].u);
    CHECK(quad[0].v > quad[1].v);
    // Corner 0 is down-left in this basis, corner 2 up-right.
    CHECK(quad[0].x < quad[2].x);
    CHECK(quad[0].y < quad[2].y);
}

TEST(the_generated_particle_sheet_holds_the_tiles_a1_1_2_names)
{
    std::vector<u8> sheet;
    texture::buildDevArtParticles(&sheet);
    CHECK_EQ((long long) sheet.size(), (long long) texture::kParticleSheetBytes);

    const auto opaqueIn = [&](int tile) {
        const int ox = (tile % texture::kParticleTilesPerEdge) * texture::kParticleTilePixels;
        const int oy = (tile / texture::kParticleTilesPerEdge) * texture::kParticleTilePixels;
        int n = 0;
        for (int y = 0; y < texture::kParticleTilePixels; ++y) {
            for (int x = 0; x < texture::kParticleTilePixels; ++x) {
                const usize i =
                    (usize(oy + y) * usize(texture::kParticleSheetEdge) + usize(ox + x)) * 4;
                n += sheet[i + 3] > 0 ? 1 : 0;
            }
        }
        return n;
    };

    // The eight smoke frames, the bubble, the flame, the lava pop and the five
    // rain/splash frames all have something in them.
    for (int frame = 0; frame <= int(entity::kParticleTileSmokeLast); ++frame) {
        CHECK(opaqueIn(frame) > 0);
    }
    CHECK(opaqueIn(int(entity::kParticleTileBubble)) > 0);
    CHECK(opaqueIn(int(entity::kParticleTileFlame)) > 0);
    CHECK(opaqueIn(int(entity::kParticleTileLava)) > 0);
    for (int i = 0; i <= entity::kParticleTileRainSpan; ++i) {
        CHECK(opaqueIn(int(entity::kParticleTileRainFirst) + i) > 0);
    }

    // **Frame 7 is the fresh one and frame 0 the faded one**, which is the
    // direction `tile = 7 - age * 8 / maxAge` walks the row in. A stand-in that
    // had it the other way round would make every puff appear at its thinnest.
    CHECK(opaqueIn(int(entity::kParticleTileSmokeLast)) > opaqueIn(0));

    // A tile nothing reaches is left empty rather than filled with a square
    // somebody chose.
    CHECK_EQ((long long) opaqueIn(200), 0LL);
}

TEST(a_pack_without_particles_png_still_gets_a_sheet)
{
    io::PosixFileSystem fs;
    std::vector<u8> sheet;
    // An empty path is Dev Art, which has no art of its own here either.
    texture::buildParticleSheet(fs, std::string(), &sheet);
    CHECK_EQ((long long) sheet.size(), (long long) texture::kParticleSheetBytes);

    std::vector<u8> standIn;
    texture::buildDevArtParticles(&standIn);
    CHECK(sheet == standIn);
}

TEST(the_three_spans_share_one_budget_and_never_overrun_it)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);

    // Four blocks' worth of flecks and a handful of sprites: 268 particles,
    // where the buffer below holds 40.
    ParticleSystem fx(107);
    for (int i = 0; i < 4; ++i) {
        fx.addBlockDestroy(s.world.w(), 0, 64, 0);
    }
    for (int i = 0; i < 8; ++i) {
        fx.spawn(s.world.w(), ParticleKind::Smoke, 0.5, 64.5, 0.5);
        fx.spawn(s.world.w(), ParticleKind::Slime, 0.5, 64.5, 0.5);
    }
    CHECK(fx.count() > 200);

    constexpr int kBudget = 40 * 4;
    std::vector<mesh::DetailVertex> out(kBudget);
    render::DrawCutoff cutoff;
    int total = 0;
    const ParticleSheet order[3] = {ParticleSheet::Terrain, ParticleSheet::Particles,
                                    ParticleSheet::Items};
    for (int i = 0; i < 3; ++i) {
        total += render::buildParticles(fx, kFlat, 0.0, 64.0, 0.0, 0.0f, order[i],
                                        out.data() + total, kBudget - total, &cutoff);
    }
    // **The three together stay inside the one buffer.** The cutoff is settled
    // by the first pass over the *whole* pool, so the later two are charged
    // against what is left of the same edge rather than each getting the budget
    // over again -- which is what would overrun it.
    CHECK(total <= kBudget);
    CHECK(total % 4 == 0);
    CHECK(total > 0);
}
