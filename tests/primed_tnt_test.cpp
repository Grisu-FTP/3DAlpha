// **TNT, from the pickaxe to the crater.**
//
// `jd` -- EntityTNTPrimed -- was the last piece of it missing: a1.1.2 has four
// ways to light TNT and three of them are ported, but each ended in a comment
// saying there was nothing to prime it into. What is checked here is the whole
// chain and not just the entity: the fuse arithmetic (which is off by one in a
// way that is easy to get wrong), the three ignition paths, the sound, the
// smoke, the blast, and the ripple a chain reaction makes.
//
// The numbers come from `q.class` and `jd.class`; see core/entity/primed_tnt.hpp
// for the transcription and docs/tick-a1.1.2.md for where TNT sits among the
// block behaviours.

#include "core/block/registry.hpp"
#include "core/entity/explosion.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/primed_tnt.hpp"
#include "core/item/use.hpp"
#include "core/render/primed_tnt_mesh.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"
#include "sound_catcher.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

struct Particle {
    int kind;
    double x, y, z;
};

// A floor of stone at y = 63 with everything a lit block needs wired to it: a
// pool to be primed into, a catcher for `random.fuse` and one for the smoke.
struct TntScene {
    SceneWorld scene{0, 0};
    entity::PrimedTntSystem tnt{99};
    mc::test::SoundCatcher sounds;
    mc::test::DropCatcher caught;
    std::vector<Particle> particles;

    TntScene()
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 8; ++z) {
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
        sounds.watch(w());
        caught.watch(w());
        w().setPrimedTntSink(&TntScene::sink, this);
        w().setParticleSink(&TntScene::puff, this);
    }

    static bool sink(void* ctx, i32 x, int y, i32 z, int fuse)
    {
        auto* self = static_cast<TntScene*>(ctx);
        return self->tnt.spawn(self->w(), x, y, z, fuse);
    }

    static void puff(void* ctx, int kind, double x, double y, double z, double, double,
                     double)
    {
        static_cast<TntScene*>(ctx)->particles.push_back(Particle{kind, x, y, z});
    }

    tick::TickWorld& w() { return scene.w(); }
    int id(i32 x, int y, i32 z) { return int(scene.w().blockAt(x, y, z)); }

    void run(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            tnt.tick(w());
        }
    }

    int countOf(int kind) const
    {
        int n = 0;
        for (const Particle& p : particles) {
            n += int(p.kind == kind);
        }
        return n;
    }
};

}  // namespace

TEST(breaking_tnt_lights_it_and_leaves_nothing_behind)
{
    // **a1.1.2's `q` has no metadata guard and a `quantityDropped` of zero**,
    // so a pickaxe to a block of TNT is a fuse and not a pickup. The
    // `if (metadata == 1)` that changes this arrives in a later version.
    TntScene g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    CHECK(item::destroyBlock(g.w(), 0, 64, 0, item::Effects{}));

    CHECK_EQ(g.id(0, 64, 0), 0);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK_EQ(g.tnt[0].fuse, entity::kPrimedTntFuse);
    // Nothing on the ground: `a(Ljava/util/Random;)I` returns 0.
    CHECK_EQ(int(g.caught.drops.size()), 0);

    // `playSoundAtEntity`, so the position is `posY - yOffset` and not the
    // block's centre.
    const mc::test::Sound* fuse = g.sounds.first("random.fuse");
    CHECK(fuse != nullptr);
    CHECK_EQ(fuse->volume, 1.0f);
    CHECK_EQ(fuse->pitch, 1.0f);
    CHECK_EQ(fuse->x, 0.5);
    CHECK_EQ(fuse->z, 0.5);
    CHECK(std::abs(fuse->y - (64.5 - entity::kPrimedTntHalf)) < 1e-9);

    // The entity is at the cell's centre and hops: up a fifth of a block, and
    // -- because the constructor converts an angle that is already radians --
    // very slightly towards -Z rather than in a random direction.
    CHECK_EQ(g.tnt[0].x, 0.5);
    CHECK_EQ(g.tnt[0].y, 64.5);
    CHECK_EQ(g.tnt[0].motionY, entity::kPrimedTntLaunch);
    CHECK(g.tnt[0].motionZ < 0.0);
    CHECK(std::abs(g.tnt[0].motionZ) > 0.019);
    CHECK(std::abs(g.tnt[0].motionX) < 0.0022);
}

TEST(a_fuse_of_eighty_smokes_eighty_times_and_blows_on_the_eighty_first)
{
    // `if (fuse-- <= 0)` reads the value **before** the decrement, so 80 is
    // eighty smoking ticks and one blast -- not seventy-nine and not eighty.
    TntScene g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    CHECK(item::destroyBlock(g.w(), 0, 64, 0, item::Effects{}));
    g.particles.clear();

    g.run(entity::kPrimedTntFuse);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK_EQ(g.tnt[0].fuse, 0);
    CHECK_EQ(g.countOf(int(entity::ParticleKind::Smoke)), entity::kPrimedTntFuse);

    // The smoke comes out half a block above the entity, which for this one is
    // the top face of the cube.
    const Particle& first = g.particles[0];
    CHECK(std::abs(first.y - (g.tnt[0].prevY + 0.5)) < 1.0);

    g.run(1);
    CHECK_EQ(g.tnt.count(), 0);
    // A crater in the floor, and the floor was stone.
    CHECK_EQ(g.id(0, 63, 0), 0);
    CHECK(g.sounds.countOf("random.explode") == 1);
}

TEST(primed_tnt_falls_bounces_and_settles)
{
    // `jd`'s physics is `ff`'s with one difference that shows: the landing
    // factors are applied on **every** tick it is on the ground, not once, so
    // a block dropped from a height hops and then stops rather than sliding.
    TntScene g;
    g.scene.place(0, 70, 0, bid(mcver::Block::Tnt), 0);
    CHECK(item::destroyBlock(g.w(), 0, 70, 0, item::Effects{}));
    CHECK_EQ(g.tnt.count(), 1);

    g.run(40);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK(g.tnt[0].onGround);
    // Resting on the stone at y = 63, so its box sits on y = 64 and its centre
    // is half its height above that.
    CHECK(std::abs(g.tnt[0].box.minY - 64.0) < 1e-6);
    CHECK(std::abs(g.tnt[0].motionX) < 1e-3);
    CHECK(std::abs(g.tnt[0].motionZ) < 1e-3);
}

TEST(a_blast_relights_tnt_on_a_short_fuse_and_says_nothing)
{
    // `q.c(Lcn;III)V` -- onBlockDestroyedByExplosion, the only override of it
    // in a1.1.2. The re-rolled fuse is `nextInt(80 / 4) + 80 / 8`, which is 10
    // to 29, and there is **no `random.fuse`** -- which is what makes a chain
    // sound like a chain rather than a hundred fuses at once.
    TntScene g;
    for (i32 x = -1; x <= 1; ++x) {
        g.scene.place(x, 64, 3, bid(mcver::Block::Tnt), 0);
    }
    entity::createExplosion(g.w(), 0.0, 64.5, 3.0, entity::kPrimedTntStrength);

    CHECK(g.tnt.count() > 0);
    for (int i = 0; i < g.tnt.count(); ++i) {
        CHECK(g.tnt[i].fuse >= entity::kPrimedTntRelitFloor);
        CHECK(g.tnt[i].fuse < entity::kPrimedTntRelitFloor + entity::kPrimedTntRelitSpread);
    }
    CHECK_EQ(g.sounds.countOf("random.fuse"), 0);
    // The cell is air before the entity exists, which is `je`'s own order --
    // otherwise a primed block would be standing inside a block of TNT.
    CHECK_EQ(g.id(0, 64, 3), 0);
    // And nothing was dropped as an item: `quantityDropped` is zero whichever
    // way the block is destroyed.
    for (const auto& drop : g.caught.drops) {
        CHECK(int(drop.item) != int(mcver::Block::Tnt));
    }
}

TEST(a_chain_ripples_rather_than_going_off_at_once)
{
    // Two blocks of TNT a few cells apart, the first lit by hand. The second
    // must be lit by the first's blast and must go off **later**, on its own
    // 10..29-tick fuse, rather than in the same tick.
    TntScene g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    g.scene.place(3, 64, 0, bid(mcver::Block::Tnt), 0);
    CHECK(item::destroyBlock(g.w(), 0, 64, 0, item::Effects{}));

    // Run up to the tick before the blast.
    g.run(entity::kPrimedTntFuse);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK_EQ(g.id(3, 64, 0), int(mcver::Block::Tnt));

    // The blast tick: the first is gone and the second is now an entity.
    g.run(1);
    CHECK_EQ(g.id(3, 64, 0), 0);
    CHECK_EQ(g.tnt.count(), 1);

    // **It was ticked on the tick it was spawned**, which is a1.1.2's own
    // behaviour and not an artefact here: `World.updateEntities` walks
    // `loadedEntityList` by index and `spawnEntityInWorld` appends to it, so an
    // entity created part way through the pass is reached before the pass ends.
    // The fuse below is therefore already one under the 10..29 that was rolled.
    const int relit = g.tnt[0].fuse;
    CHECK(relit >= entity::kPrimedTntRelitFloor - 1);
    CHECK(relit < entity::kPrimedTntRelitFloor + entity::kPrimedTntRelitSpread);

    // **Nothing cascades inside one tick.** Ten ticks at the very least
    // separate the two blasts, which is what a chain reaction looks like.
    CHECK_EQ(g.sounds.countOf("random.explode"), 1);
    g.run(relit);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK_EQ(g.tnt[0].fuse, 0);
    g.run(1);
    CHECK_EQ(g.tnt.count(), 0);
    CHECK_EQ(g.sounds.countOf("random.explode"), 2);
}

TEST(a_world_with_no_tnt_pool_still_takes_the_same_random_path)
{
    // The seam is optional, and a headless caller has none. What must not
    // change is the world's generator: `q.c`'s `nextInt(20)` is drawn whether
    // or not there is anywhere to put the entity, exactly as
    // `dropBlockAsItem`'s draws are.
    SceneWorld a{0, 0};
    SceneWorld b{0, 0};
    a.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    b.place(0, 64, 0, bid(mcver::Block::Tnt), 0);

    entity::PrimedTntSystem pool{7};
    struct Ctx {
        entity::PrimedTntSystem* pool;
        tick::TickWorld* world;
    } ctx{&pool, &a.w()};
    a.w().setPrimedTntSink(
        [](void* c, i32 x, int y, i32 z, int fuse) {
            auto* self = static_cast<Ctx*>(c);
            return self->pool->spawn(*self->world, x, y, z, fuse);
        },
        &ctx);

    tick::tntDestroyedByExplosion(a.w(), 0, 64, 0);
    tick::tntDestroyedByExplosion(b.w(), 0, 64, 0);
    CHECK_EQ(pool.count(), 1);
    // Both generators have taken the same number of draws, so the next one
    // agrees.
    CHECK_EQ(a.w().random().nextInt(1000), b.w().random().nextInt(1000));
}

TEST(fire_spreading_into_tnt_lights_it)
{
    // `og.a(Lcn;IIIILjava/util/Random;)V` calls
    // `Block.tnt.onBlockDestroyedByPlayer(world, x, y, z, 0)` after it has
    // already written fire or air over the cell -- so the block goes either
    // way and a full fuse is lit on top of it.
    TntScene g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    tick::tntDestroyedByPlayer(g.w(), 0, 64, 0);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK_EQ(g.tnt[0].fuse, entity::kPrimedTntFuse);
    CHECK_EQ(g.sounds.countOf("random.fuse"), 1);
}

TEST(powering_tnt_lights_it_and_takes_the_block)
{
    // `q.a(Lcn;IIIII)V` -- the fourth ignition path, and the one that needed
    // redstone underneath it. A lit torch beside a block of TNT is enough.
    TntScene g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    g.scene.place(1, 64, 0, bid(mcver::Block::RedstoneTorch), 5);
    CHECK(g.w().isIndirectlyPowered(0, 64, 0));

    tick::neighbourChanged(g.w(), 0, 64, 0, bid(mcver::Block::RedstoneTorch));
    CHECK_EQ(g.id(0, 64, 0), 0);
    CHECK_EQ(g.tnt.count(), 1);
    CHECK_EQ(g.tnt[0].fuse, entity::kPrimedTntFuse);
    CHECK_EQ(g.sounds.countOf("random.fuse"), 1);
}

TEST(an_unpowered_neighbour_leaves_tnt_alone)
{
    // **The gate is `canProvidePower`**, the same one a door uses, and it is
    // what keeps a block of TNT in a wall from re-reading the world every time
    // anything near it changes.
    TntScene g;
    g.scene.place(0, 64, 0, bid(mcver::Block::Tnt), 0);
    g.scene.place(1, 64, 0, bid(mcver::Block::Planks), 0);
    tick::neighbourChanged(g.w(), 0, 64, 0, bid(mcver::Block::Planks));
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Tnt));
    CHECK_EQ(g.tnt.count(), 0);

    // And a power source that is not actually powering it is not enough either.
    g.scene.place(5, 64, 5, bid(mcver::Block::RedstoneWire), 0);
    tick::neighbourChanged(g.w(), 0, 64, 0, bid(mcver::Block::RedstoneWire));
    CHECK_EQ(g.id(0, 64, 0), int(mcver::Block::Tnt));
    CHECK_EQ(g.tnt.count(), 0);
}

// ---------------------------------------------------------------------------
// What it looks like
// ---------------------------------------------------------------------------

TEST(the_swell_is_a_fourth_power_over_the_last_ten_ticks)
{
    using namespace mc::render;
    // Outside ten ticks there is no scale at all, which is the same number.
    CHECK_EQ(primedTntScale(80, 0.0f), 1.0f);
    CHECK_EQ(primedTntScale(10, 0.0f), 1.0f);

    // `f = 1 - (fuse - partial + 1) / 10`, squared twice, times 0.3. At fuse 5
    // that is `1 - 0.6 = 0.4`, and 0.4^4 is 0.0256 -- **under three
    // hundredths** of the swell, four ticks from the end. The fourth power is
    // what keeps TNT its own size until the last moment.
    const float five = primedTntScale(5, 0.0f);
    CHECK(std::abs(five - (1.0f + 0.0256f * 0.3f)) < 1e-5f);

    // And on the last drawn tick it is nearly the full 1.3.
    const float last = primedTntScale(0, 0.0f);
    CHECK(std::abs(last - (1.0f + 0.9f * 0.9f * 0.9f * 0.9f * 0.3f)) < 1e-5f);
}

TEST(the_flash_blinks_every_five_ticks_and_grows_towards_the_blast)
{
    using namespace mc::render;
    // `fuse / 5 % 2 == 0`: on for five, off for five.
    CHECK(primedTntFlashing(0));
    CHECK(primedTntFlashing(4));
    CHECK(!primedTntFlashing(5));
    CHECK(!primedTntFlashing(9));
    CHECK(primedTntFlashing(10));
    CHECK(primedTntFlashing(80));

    // `(1 - (fuse - partial + 1) / 100) * 0.8`. A fresh fuse flashes faintly
    // and the last one flashes hard.
    const float fresh = primedTntFlashAlpha(80, 0.0f);
    const float late = primedTntFlashAlpha(0, 0.0f);
    CHECK(std::abs(fresh - (1.0f - 0.81f) * 0.8f) < 1e-5f);
    CHECK(std::abs(late - (1.0f - 0.01f) * 0.8f) < 1e-5f);
    CHECK(late > fresh);
}

TEST(the_flash_cube_is_the_same_cube_the_textured_pass_draws)
{
    using namespace mc::render;
    // A white rind around the block would be the visible symptom of the two
    // passes disagreeing about the swell, so they are checked against each
    // other rather than against a constant.
    entity::PrimedTnt e{};
    e.setPosition(4.0, 65.0, -2.0);
    e.active = true;
    e.fuse = 3;

    OutlineVertex flash[kPrimedTntFlashVerticesEach];
    const int written = buildPrimedTntFlash(e, 0.0, 0.0, 0.0, 0.5f, flash,
                                            kPrimedTntFlashVerticesEach);
    CHECK_EQ(written, kPrimedTntFlashVerticesEach);

    const float half = 0.5f * primedTntScale(e.fuse, 0.5f);
    float maxX = flash[0].x;
    float minX = flash[0].x;
    for (int i = 1; i < written; ++i) {
        maxX = flash[i].x > maxX ? flash[i].x : maxX;
        minX = flash[i].x < minX ? flash[i].x : minX;
    }
    CHECK(std::abs((maxX - minX) - half * 2.0f) < 1e-4f);
    CHECK(std::abs(maxX - (4.0f + half)) < 1e-4f);

    // And nothing at all on a tick it is not flashing.
    e.fuse = 7;
    CHECK_EQ(buildPrimedTntFlash(e, 0.0, 0.0, 0.0, 0.5f, flash,
                                 kPrimedTntFlashVerticesEach),
             0);
}
