// The block-breaking particles: how many, where they start, and how they move.
//
// Their velocities are random in the original too -- two time-seeded generators
// -- so what is checked here is the *shape* of the answer: the count, the grid
// they are cut from, the lifetime bounds, that they fall, that they land on
// solid ground and stay there, and that the pool refuses rather than overruns.

#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
#include "core/item/registry.hpp"
#include "core/render/particle_mesh.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "low_heap.hpp"
#include "scene_world.hpp"

#include <cmath>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Particle;
using mc::entity::ParticleSystem;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

struct Scene {
    SceneWorld world{0, 0};

    Scene()
    {
        for (i32 x = -4; x <= 4; ++x) {
            for (i32 z = -4; z <= 4; ++z) {
                world.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
    }
};

}  // namespace

TEST(a_broken_block_makes_sixty_four_particles)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);

    ParticleSystem fx(1234);
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);
    CHECK_EQ(fx.count(), 64);
    CHECK_EQ((long long) fx.refused(), 0LL);
}

TEST(an_empty_cell_makes_none)
{
    Scene s;
    ParticleSystem fx(1234);
    fx.addBlockDestroy(s.world.w(), 0, 70, 0);
    CHECK_EQ(fx.count(), 0);
}

TEST(the_cloud_is_cut_from_the_blocks_own_grid)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    ParticleSystem fx(99);
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);

    // Every particle starts inside the cell it came from -- at a sub-cell
    // centre, so an eighth of a block in from each face at the closest.
    for (int i = 0; i < fx.count(); ++i) {
        const Particle& p = fx[i];
        CHECK(p.x >= 0.125 && p.x <= 0.875);
        CHECK(p.y >= 64.125 && p.y <= 64.875);
        CHECK(p.z >= 0.125 && p.z <= 0.875);
        // ...and it carries the block's own tile, not a face of it.
        CHECK_EQ((long long) p.tile,
                 (long long) block::def(bid(mcver::Block::Dirt)).texture);
        // The quarter-tile it shows is one of four along each axis.
        CHECK(p.jitterU >= 0.0f && p.jitterU < 3.0f);
        CHECK(p.jitterV >= 0.0f && p.jitterV < 3.0f);
        // `(nextFloat * 0.5 + 0.5) * 2 / 2`.
        CHECK(p.scale >= 0.5f && p.scale <= 1.0f);
        // `4.0f / (nextFloat * 0.9f + 0.1f)`, truncated.
        CHECK(p.maxAge >= 4 && p.maxAge <= 40);
    }
}

TEST(the_cloud_is_thrown_outward_and_upward)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    ParticleSystem fx(7);
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);

    // The constructor's `+0.1` on y is bigger than the *typical* scaled
    // velocity but not the largest one -- the speed is two random draws plus
    // one, times 0.15, times 0.4, so it reaches 0.18 -- which is why a few of
    // the flecks thrown hardest downward still start falling. Most rise, and
    // that is what makes a broken block puff before it drops.
    int rising = 0;
    for (int i = 0; i < fx.count(); ++i) {
        if (fx[i].motionY > 0.0) {
            ++rising;
        }
    }
    CHECK(rising > fx.count() * 3 / 4);
    CHECK(rising < fx.count());
}

TEST(particles_fall_land_and_stop)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    ParticleSystem fx(4321);

    // Spawn, **then** clear the cell -- which is the order `destroyBlock` runs
    // in and the order that matters: the particles need the block's texture and
    // then need it out of their way.
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);
    s.world.w().setBlockAndDataWithNotify(0, 64, 0, block::kAir, 0);

    // Nothing may end up inside the floor, which is what the shared sweep is
    // for: a particle is an entity and stops on the ground like one.
    for (int i = 0; i < 20; ++i) {
        fx.tick(s.world.w());
        for (int p = 0; p < fx.count(); ++p) {
            CHECK(fx[p].y - mc::entity::kParticleHalf >= 64.0 - 1e-9);
        }
    }

    // Long enough for the longest-lived to expire.
    for (int i = 0; i < 64; ++i) {
        fx.tick(s.world.w());
    }
    CHECK_EQ(fx.count(), 0);
}

TEST(a_particle_lives_between_five_and_fortyone_ticks)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    ParticleSystem fx(2024);
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);

    // `if (particleAge++ >= particleMaxAge)` is a post-increment, so a
    // particle survives its maxAge'th tick and dies on the next.
    int ticks = 0;
    while (fx.count() > 0 && ticks < 200) {
        fx.tick(s.world.w());
        ++ticks;
    }
    CHECK(ticks >= 5);
    CHECK(ticks <= 41);
}

TEST(the_pool_grows_past_eight_clouds)
{
    // `EffectRenderer.addEffect` is one `List.add`. Nine clouds is 576
    // particles, past the 512 held from construction, with no tick in between
    // to retire any of them -- and all of them are kept.
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    ParticleSystem fx(11);
    for (int i = 0; i < 9; ++i) {
        fx.addBlockDestroy(s.world.w(), 0, 64, 0);
    }
    CHECK_EQ(fx.count(), 9 * 64);
    CHECK_EQ(int(fx.refused()), 0);
}

TEST(a_full_heap_refuses_the_newest_particles_rather_than_overrunning)
{
    // Past what the heap will hold the *newest* is refused and counted:
    // dropping the oldest would make a cloud vanish mid-flight.
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    ParticleSystem fx(11);
    test::LowHeap low;
    for (int i = 0; i < 9; ++i) {
        fx.addBlockDestroy(s.world.w(), 0, 64, 0);
    }
    CHECK_EQ(fx.count(), ParticleSystem::kInitialCapacity);
    CHECK_EQ((long long) fx.refused(), 9LL * 64 - ParticleSystem::kInitialCapacity);
}

TEST(a_particle_carries_the_light_where_it_is)
{
    Scene s;
    // A torch beside the block, so there is block light to carry. The scene
    // world starts dark, so anything non-zero here came from the world rather
    // than from a default.
    s.world.place(0, 64, 0, bid(mcver::Block::Dirt), 0);
    s.world.w().setBlockAndDataWithNotify(1, 64, 0, bid(mcver::Block::Torch), 5);

    ParticleSystem fx(5);
    fx.addBlockDestroy(s.world.w(), 0, 64, 0);
    CHECK(fx.count() > 0);

    // Packed as `(sky << 4) | block`, the byte every mesh vertex carries -- so
    // whatever the world says at the particle is what the shader gets.
    const u8 want = u8((s.world.w().skyLightAt(0, 64, 0) << 4)
                       | s.world.w().blockLightAt(0, 64, 0));
    CHECK_EQ((long long) fx[0].light, (long long) want);
}

// ---------------------------------------------------------------------------
// The other eleven kinds
//
// Same terms as the digging fleck above: the velocities and the lifetimes are
// random in the jar too, so what is pinned is the *shape* -- which sheet a kind
// draws off, which tile it starts on, what its tint is, what its life is
// bounded by, and the handful of rules that are not random at all.
// ---------------------------------------------------------------------------

using mc::entity::ParticleKind;
using mc::entity::ParticleLight;
using mc::entity::ParticleSheet;

namespace {

// One particle of a kind, spawned into open air well above the floor.
const Particle& one(ParticleSystem& fx, Scene& s, ParticleKind kind, double mx = 0.0,
                    double my = 0.0, double mz = 0.0)
{
    fx.spawn(s.world.w(), kind, 0.5, 70.5, 0.5, mx, my, mz);
    return fx[fx.count() - 1];
}

}  // namespace

TEST(each_kind_lands_on_the_sheet_its_renderer_binds)
{
    // `nq.c()`: layer 0 is particles.png, layer 1 terrain.png, layer 2
    // gui/items.png. Ten of the twelve are sprites.
    CHECK(mc::entity::sheetOf(ParticleKind::Digging) == ParticleSheet::Terrain);
    CHECK(mc::entity::sheetOf(ParticleKind::SnowballPoof) == ParticleSheet::Items);
    CHECK(mc::entity::sheetOf(ParticleKind::Slime) == ParticleSheet::Items);
    for (ParticleKind kind : {ParticleKind::Bubble, ParticleKind::Smoke,
                              ParticleKind::Explode, ParticleKind::Flame,
                              ParticleKind::Lava, ParticleKind::Splash,
                              ParticleKind::LargeSmoke, ParticleKind::Reddust,
                              ParticleKind::Rain}) {
        CHECK(mc::entity::sheetOf(kind) == ParticleSheet::Particles);
    }
}

TEST(the_named_kinds_start_on_the_tiles_the_jar_names)
{
    Scene s;
    ParticleSystem fx(11);

    CHECK_EQ((long long) one(fx, s, ParticleKind::Bubble).tile,
             (long long) mc::entity::kParticleTileBubble);
    CHECK_EQ((long long) one(fx, s, ParticleKind::Flame).tile,
             (long long) mc::entity::kParticleTileFlame);
    CHECK_EQ((long long) one(fx, s, ParticleKind::Lava).tile,
             (long long) mc::entity::kParticleTileLava);

    // The smoke row is walked backwards, so a fresh puff is on the *last*
    // frame and thins towards 0.
    CHECK_EQ((long long) one(fx, s, ParticleKind::Smoke).tile,
             (long long) mc::entity::kParticleTileSmokeLast);
    CHECK_EQ((long long) one(fx, s, ParticleKind::Explode).tile,
             (long long) mc::entity::kParticleTileSmokeLast);
    CHECK_EQ((long long) one(fx, s, ParticleKind::Reddust).tile,
             (long long) mc::entity::kParticleTileSmokeLast);

    // `nf` draws 19..22 and `kq` adds one to whatever it drew, so a splash is
    // always one frame further along than the drop it is built from.
    const Particle& rain = one(fx, s, ParticleKind::Rain);
    CHECK(rain.tile >= mc::entity::kParticleTileRainFirst
          && rain.tile < mc::entity::kParticleTileRainFirst
                             + mc::entity::kParticleTileRainSpan);
    const Particle& splash = one(fx, s, ParticleKind::Splash);
    CHECK(splash.tile > mc::entity::kParticleTileRainFirst
          && splash.tile <= mc::entity::kParticleTileRainFirst
                                + mc::entity::kParticleTileRainSpan);
}

TEST(the_two_breaking_kinds_carry_an_item_icon)
{
    Scene s;
    ParticleSystem fx(12);
    CHECK_EQ((long long) one(fx, s, ParticleKind::SnowballPoof).tile,
             (long long) mc::item::def(mc::entity::kSnowballItem).icon);
    CHECK_EQ((long long) one(fx, s, ParticleKind::Slime).tile,
             (long long) mc::item::def(mc::entity::kSlimeballItem).icon);
    // `ig` takes its gravity off `Block.snow`, which is 1.0 like every other
    // block in this version -- so a poof falls at the same rate a fleck does.
    CHECK_EQ(one(fx, s, ParticleKind::Slime).gravity, mc::entity::kParticleGravity);
}

TEST(only_reddust_is_tinted_and_smoke_is_a_shadow)
{
    Scene s;
    ParticleSystem fx(13);

    // `en`: red 0.7..1.0, green and blue at most 0.1. The one coloured sprite.
    for (int i = 0; i < 40; ++i) {
        const Particle& dust = one(fx, s, ParticleKind::Reddust);
        CHECK(dust.red > dust.green && dust.red > dust.blue);
        CHECK(dust.red >= 178);  // 0.7 * 255
        CHECK(dust.green <= 26 && dust.blue <= 26);
    }

    // `nl`: a grey between black and 0.3, equal on all three -- so smoke is a
    // darkening of the sprite rather than a colour of its own.
    for (int i = 0; i < 40; ++i) {
        const Particle& smoke = one(fx, s, ParticleKind::Smoke);
        CHECK_EQ((long long) smoke.red, (long long) smoke.green);
        CHECK_EQ((long long) smoke.red, (long long) smoke.blue);
        CHECK(smoke.red <= 77);  // 0.3 * 255
    }

    // `dp`: pale, 0.7..1.0 on all three -- which is what tells a blast from the
    // smoke it leaves behind, since both walk the same eight frames.
    for (int i = 0; i < 40; ++i) {
        const Particle& blast = one(fx, s, ParticleKind::Explode);
        CHECK_EQ((long long) blast.red, (long long) blast.green);
        CHECK(blast.red >= 178);
    }
}

TEST(largesmoke_is_smoke_two_and_a_half_times_over)
{
    Scene s;
    ParticleSystem small(21);
    ParticleSystem large(21);  // the same seed, so the draws line up

    double smallScale = 0.0, largeScale = 0.0;
    int smallLife = 0, largeLife = 0;
    for (int i = 0; i < 50; ++i) {
        const Particle& a = one(small, s, ParticleKind::Smoke);
        const Particle& b = one(large, s, ParticleKind::LargeSmoke);
        smallScale += double(a.birthScale);
        largeScale += double(b.birthScale);
        smallLife += a.maxAge;
        largeLife += b.maxAge;
    }
    // `particleScale *= scale` and `particleMaxAge = (int)(maxAge * scale)`:
    // both are scaled, which is why a chimney's puff lingers as well as looms.
    CHECK(largeScale > smallScale * 2.4 && largeScale < smallScale * 2.6);
    CHECK(largeLife > smallLife * 2);
}

TEST(a_flame_does_not_collide_and_starts_full_bright)
{
    Scene s;
    ParticleSystem fx(31);
    // Inside the floor, where anything that swept would be pushed out.
    fx.spawn(s.world.w(), ParticleKind::Flame, 0.5, 63.5, 0.5);
    const double startY = fx[0].y;
    CHECK(fx[0].noClip);
    CHECK(fx[0].lighting == ParticleLight::FadeFromFull);

    // **Born full bright whatever the cell's light is.** Buried in stone, the
    // world's own byte here is 0; `jb.a(float)` is `world * f + (1 - f)` and
    // `f` is zero on the tick it is made, so the quad is lit at 15/15.
    CHECK_EQ((long long) mc::render::particleLight(fx[0], 0.0f), 255LL);
    // ...and it has settled onto the world's light by the end.
    const int life = fx[0].maxAge;
    Particle aged = fx[0];
    aged.age = life;
    CHECK_EQ((long long) mc::render::particleLight(aged, 0.0f), (long long) aged.light);

    for (int i = 0; i < 3; ++i) {
        fx.tick(s.world.w());
    }
    // It rose out of solid stone rather than being stopped by it: `jb` sets
    // `noClip`, so `moveEntity` offsets the box and asks the world nothing.
    CHECK(fx.count() == 1);
    CHECK(fx[0].y > startY);
}

TEST(a_lava_pop_is_full_bright_and_trails_smoke)
{
    Scene s;
    ParticleSystem fx(41);
    fx.spawn(s.world.w(), ParticleKind::Lava, 0.5, 70.5, 0.5);
    CHECK(fx[0].lighting == ParticleLight::Full);
    CHECK_EQ((long long) mc::render::particleLight(fx[0], 0.5f), 255LL);

    // `cq.e_()` throws a `smoke` whenever `nextFloat() > age / maxAge`, so a
    // fresh pop smokes nearly every tick. Three ticks is enough for at least
    // one, and every extra particle in the pool is that smoke.
    int ticks = 0;
    while (fx.count() == 1 && ticks < 8) {
        fx.tick(s.world.w());
        ++ticks;
    }
    CHECK(fx.count() > 1);
    bool sawSmoke = false;
    for (int i = 0; i < fx.count(); ++i) {
        sawSmoke = sawSmoke || fx[i].kind == ParticleKind::Smoke;
    }
    CHECK(sawSmoke);
}

TEST(a_bubble_dies_when_it_leaves_the_water)
{
    Scene s;
    // A column of water with air above it.
    for (int y = 64; y <= 66; ++y) {
        s.world.place(0, y, 0, bid(mcver::Block::Water), 0);
    }

    ParticleSystem fx(51);
    fx.spawn(s.world.w(), ParticleKind::Bubble, 0.5, 64.5, 0.5);
    CHECK_EQ(fx.count(), 1);

    // It rises -- `ba.e_()` adds 0.002 a tick and never subtracts -- and
    // survives while it is still inside the column.
    for (int i = 0; i < 3; ++i) {
        fx.tick(s.world.w());
    }
    CHECK_EQ(fx.count(), 1);

    // Take the water away and it goes on the next tick, whatever its countdown
    // says: the material test is not an age.
    for (int y = 64; y <= 66; ++y) {
        s.world.w().setBlockAndDataWithNotify(0, y, 0, block::kAir, 0);
    }
    fx.tick(s.world.w());
    CHECK_EQ(fx.count(), 0);
}

TEST(a_splash_takes_the_throwers_motion_and_a_raindrop_does_not)
{
    Scene s;
    ParticleSystem fx(61);

    // `kq`: any non-zero motion replaces `nf`'s own, with a tenth added on y.
    const Particle& thrown = one(fx, s, ParticleKind::Splash, 0.5, 0.25, -0.5);
    CHECK_EQ(thrown.motionX, 0.5);
    CHECK_EQ(thrown.motionY, 0.25 + 0.1);
    CHECK_EQ(thrown.motionZ, -0.5);

    // Still water throws a splash with no motion at all, and it keeps `nf`'s
    // upward toss instead.
    const Particle& still = one(fx, s, ParticleKind::Splash);
    CHECK(still.motionY >= 0.1 && still.motionY <= 0.3);

    // A raindrop is `nf` and has no motion branch, so the same arguments are
    // simply discarded -- `e.a` never passes it any.
    const Particle& drop = one(fx, s, ParticleKind::Rain, 5.0, 5.0, 5.0);
    CHECK(drop.motionX < 1.0 && drop.motionZ < 1.0);
}

TEST(six_of_the_named_kinds_ignore_the_motion_they_are_given)
{
    Scene s;
    ParticleSystem fx(71);
    // `cn.a("smoke", x, y, z, mx, my, mz)` really does drop mx/my/mz on the
    // floor in a1.1.2 -- `e.a` passes only the position to `new nl(...)`. A
    // hurl of 100 blocks a tick leaves a puff that barely drifts.
    for (ParticleKind kind : {ParticleKind::Smoke, ParticleKind::LargeSmoke,
                              ParticleKind::Lava, ParticleKind::Reddust,
                              ParticleKind::SnowballPoof, ParticleKind::Slime}) {
        const Particle& p = one(fx, s, kind, 100.0, 100.0, 100.0);
        CHECK(p.motionX < 1.0 && p.motionZ < 1.0);
    }
    // ...and the four that do take it, keep it.
    const Particle& blast = one(fx, s, ParticleKind::Explode, 100.0, 0.0, 0.0);
    CHECK(blast.motionX > 99.0);
}

TEST(the_smoke_row_thins_as_a_puff_ages)
{
    Scene s;
    ParticleSystem fx(81);
    fx.spawn(s.world.w(), ParticleKind::Smoke, 0.5, 70.5, 0.5);
    const int life = fx[0].maxAge;

    u16 previous = fx[0].tile;
    CHECK_EQ((long long) previous, (long long) mc::entity::kParticleTileSmokeLast);
    for (int i = 0; i < life && fx.count() == 1; ++i) {
        fx.tick(s.world.w());
        if (fx.count() == 0) {
            break;
        }
        CHECK(fx[0].tile <= previous);
        previous = fx[0].tile;
    }
    // It reached the far end of the row before it died.
    CHECK(previous <= 1);
}
