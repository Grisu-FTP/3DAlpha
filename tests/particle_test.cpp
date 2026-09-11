// The block-breaking particles: how many, where they start, and how they move.
//
// Their velocities are random in the original too -- two time-seeded generators
// -- so what is checked here is the *shape* of the answer: the count, the grid
// they are cut from, the lifetime bounds, that they fall, that they land on
// solid ground and stay there, and that the pool refuses rather than overruns.

#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
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
