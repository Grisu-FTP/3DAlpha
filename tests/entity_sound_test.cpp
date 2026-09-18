// The sounds an *entity* makes, as against the ones a block behaviour makes.
//
// There are three of them in a1.1.2 once the monsters are left out, and they
// are checked here through `TickWorld`'s sound sink with no audio anywhere in
// the build -- the same trick tests/step_sound_test.cpp plays with the
// footstep and tests/drop_catcher.hpp plays with drops.
//
//   * **`random.splash`** -- `kh.y()`'s water entry, which the player, the four
//     animals, dropped items, arrows and boats all reach and which the falling
//     block, the painting and the minecart do not. That set is the interesting
//     part and it is derived rather than chosen: `kh.e_()` is one line, `y()`,
//     so an entity splashes exactly when its own `onUpdate` calls
//     `super.onUpdate()`. See core/entity/water_entry.hpp.
//   * **`random.fizz`** -- a stack landing in lava, `dx.e_()`.
//   * **`random.drr`** -- an arrow striking something, `kg.e_()`, and the thing
//     worth asserting about it is *where* it plays.
//
// **What cannot be checked here is the half that actually failed**, which is
// whether the key was ever decoded -- an unpreloaded key is silence, and the
// animals shipped mute for exactly that reason. That is
// `a_key_that_is_played_is_a_key_that_is_preloaded` below, which compares the
// two lists rather than the sound.

#include "core/audio/block_sound.hpp"
#include "core/block/registry.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/item_entity.hpp"
#include "core/item/creative_palette.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/water_entry.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"
#include "sound_catcher.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::WaterEntry;
using mc::entity::WaterEntryResult;
using mc::test::SceneWorld;
using mc::test::SoundCatcher;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A stone floor at y = 62 with a pool of water in it: the cells from x,z in
// [-2, 2] are water at y = 63 and 64, so an entity coming down lands in it.
struct Pool {
    SoundCatcher heard;
    SceneWorld scene{0, 0};

    Pool()
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 8; ++z) {
                scene.place(x, 62, z, bid(mcver::Block::Stone), 0);
                if (x >= -2 && x <= 2 && z >= -2 && z <= 2) {
                    scene.place(x, 63, z, bid(mcver::Block::Water), 0);
                    scene.place(x, 64, z, bid(mcver::Block::Water), 0);
                } else {
                    scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
                }
            }
        }
        scene.lightColumnsFrom(-8, 8, -8, 8, 65);
        heard.watch(scene.w());
    }

    tick::TickWorld& w() { return scene.w(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// The branch itself, with no world and no pool around it.
// ---------------------------------------------------------------------------

// "Nothing of this entity is touching water", which is the predicate
// `updateWaterEntry` latches its splash on -- see core/entity/water_entry.hpp.
// With no world here, an entity the jar's own probe calls dry really is dry.
const auto dry = [] { return false; };

// ...and its opposite, for the case the latch exists for: the jar's probe says
// dry on a tick where the entity is plainly still under the surface.
const auto submerged = [] { return true; };

TEST(the_splash_is_an_edge_and_not_a_state)
{
    WaterEntry state;

    // `firstUpdate` swallows the first tick, whatever it says. That is what
    // keeps a world from splashing on the frame it opens, and a boat from
    // announcing itself on the tick it is placed.
    CHECK(!mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, dry).splash);

    // Still in it: no second splash, however long it stays.
    for (int i = 0; i < 40; ++i) {
        const WaterEntryResult wet = mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, dry);
        CHECK(wet.inWater);
        CHECK(!wet.splash);
    }

    // Out, and back in: that is an edge and it sounds.
    CHECK(!mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry).inWater);
    const WaterEntryResult again = mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, dry);
    CHECK(again.splash);
    CHECK(again.inWater);
}

// **The latch, which is this port's and not the jar's.**
//
// `g_()` shrinks the box by four tenths top and bottom, so for anything shorter
// than 0.8 of a block the box it probes with is inverted and its answer
// flickers -- 45 % of ticks over a cell of positions. Left alone, a sinking
// item splashes again every few blocks. See core/entity/water_entry.hpp.
TEST(a_flickering_probe_does_not_splash_twice)
{
    WaterEntry state;
    mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry);

    // Into the water: one splash.
    CHECK(mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, submerged).splash);

    // Now the flicker: the jar's probe says dry on alternate ticks while the
    // entity is still under the surface. Not one further splash.
    for (int i = 0; i < 40; ++i) {
        CHECK(!mc::entity::updateWaterEntry(state, false, 0.0, -1.0, 0.0, submerged).splash);
        CHECK(!mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, submerged).splash);
    }

    // **`inWater` is still the jar's answer**, which is the half that is not
    // deviated from: the current pushes and the fall distance clears on exactly
    // the ticks the jar says.
    CHECK(!mc::entity::updateWaterEntry(state, false, 0.0, -1.0, 0.0, submerged).inWater);
    CHECK(mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, submerged).inWater);

    // Out of the water for real, and the next entry sounds again.
    CHECK(!mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry).splash);
    CHECK(mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, submerged).splash);
}

TEST(the_splash_volume_is_the_motion_and_is_clamped_at_one)
{
    // `sqrt(mx*mx*0.2 + my*my + mz*mz*0.2) * 0.2F`, capped at 1.
    //
    // Entering with no motion at all is a real splash at volume zero, which is
    // the jar's: `playSoundAtEntity` is called either way.
    {
        WaterEntry state;
        mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry);
        const WaterEntryResult wet = mc::entity::updateWaterEntry(state, true, 0.0, 0.0, 0.0, dry);
        CHECK(wet.splash);
        CHECK(wet.volume == 0.0f);
    }

    // A one-block-a-tick drop: sqrt(1) * 0.2 = 0.2.
    {
        WaterEntry state;
        mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry);
        const WaterEntryResult wet = mc::entity::updateWaterEntry(state, true, 0.0, -1.0, 0.0, dry);
        CHECK(std::fabs(double(wet.volume) - 0.2) < 1e-6);
    }

    // **The horizontal terms are weighted a fifth**, which is why running into
    // a lake is quieter than falling into it by the same speed. Same magnitude
    // as above, sideways: sqrt(0.2) * 0.2 = 0.0894...
    {
        WaterEntry state;
        mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry);
        const WaterEntryResult wet = mc::entity::updateWaterEntry(state, true, 1.0, 0.0, 0.0, dry);
        CHECK(std::fabs(double(wet.volume) - std::sqrt(0.2) * 0.2) < 1e-6);
    }

    // Terminal velocity many times over still clamps to 1.
    {
        WaterEntry state;
        mc::entity::updateWaterEntry(state, false, 0.0, 0.0, 0.0, dry);
        const WaterEntryResult wet = mc::entity::updateWaterEntry(state, true, 0.0, -80.0, 0.0, dry);
        CHECK(wet.volume == 1.0f);
    }
}

// ---------------------------------------------------------------------------
// The pools that reach it.
// ---------------------------------------------------------------------------

TEST(a_player_falling_into_water_splashes_once_at_the_feet)
{
    Pool pool;
    entity::PlayerBody body;
    body.setFeet(0.5, 72.0, 0.5);

    entity::PlayerInput input;
    int splashes = 0;
    double splashFeet = 0.0;
    double splashBoxBottom = 0.0;
    double splashMotionX = 0.0, splashMotionY = 0.0, splashMotionZ = 0.0;
    float splashVolume = 0.0f;
    for (int i = 0; i < 60; ++i) {
        const double beforeX = body.motionX;
        const double beforeY = body.motionY;
        const double beforeZ = body.motionZ;
        const WaterEntryResult wet = body.updateWaterEntry(pool.w());
        if (wet.splash) {
            splashMotionX = beforeX;
            splashMotionY = beforeY;
            splashMotionZ = beforeZ;
            ++splashes;
            splashFeet = body.posY - double(entity::kEyeHeight);
            splashBoxBottom = body.box.minY;
            splashVolume = wet.volume;
        }
        body.tick(pool.w(), input);
    }

    // Once. A player who stays in the pool does not keep splashing.
    CHECK_EQ(splashes, 1);

    // **The position is the feet.** `playSoundAtEntity` passes `posY -
    // yOffset`, and a player's `yOffset` is 1.62, so a sound taken off the
    // camera -- which is what `posY` is in this version -- would be a block and
    // a half above the water it is meant to be coming out of.
    CHECK(std::fabs(splashFeet - splashBoxBottom) < 1e-9);

    // **And the volume is the expression, checked against the motion that
    // produced it** rather than against a number typed in here. Seven blocks of
    // fall is nowhere near the clamp -- a1.1.2's gravity and drag settle at
    // about 3.9 a tick and this reaches about 1.0 -- so what this proves is the
    // arithmetic, which is the part that could be wrong.
    const double expected =
        std::sqrt(splashMotionX * splashMotionX * 0.20000000298023224
                  + splashMotionY * splashMotionY
                  + splashMotionZ * splashMotionZ * 0.20000000298023224)
        * 0.2;
    CHECK(std::fabs(double(splashVolume) - expected) < 1e-6);
    CHECK(splashVolume > 0.0f);

    // And the fall distance went with it, which is the rest of that branch: a
    // dive into deep water hurts nobody.
    CHECK_EQ(double(body.fallDistance), 0.0);
}

TEST(a_dropped_item_splashes_when_it_lands_in_the_pool)
{
    Pool pool;
    entity::ItemEntitySystem items{4242};
    CHECK(items.spawn(pool.w(), 0.5, 72.0, 0.5, item::paletteItem(0), 1, 0));

    for (int i = 0; i < 60; ++i) {
        items.tick(pool.w());
    }

    CHECK_EQ(pool.heard.countOf(mc::entity::kSplashSound), 1);
}

TEST(a_placed_boat_is_silent_and_a_settling_one_is_inaudible)
{
    Pool pool;
    entity::BoatSystem boats{99};

    // `me.a(...)` puts the hull at `(hit + 0.5, hit + 1.5, hit + 0.5)`, so
    // placing it on the pool floor drops it straight into the water.
    CHECK(boats.place(pool.w(), 0, 62, 0));
    boats.tick(pool.w(), entity::VehicleRider{});

    // **Nothing on the tick it is put down, and that is what `firstUpdate` is
    // for.** A boat is placed *into* water; if the entry were a level test
    // rather than an edge, every boat would announce itself, and so would every
    // boat in the world on the tick a save was reloaded.
    CHECK_EQ(pool.heard.countOf(mc::entity::kSplashSound), 0);

    // **A floating boat does flicker, and it is faithful that it does.** `dc`
    // does not override `g_()`, which insets the box by four tenths top and
    // bottom -- and a hull is six tenths tall, so the probe it asks with is
    // inverted and its answer changes as the buoyancy rocks it across a cell
    // boundary. That is a1.1.2's, not something this port introduced.
    //
    // It costs nothing audible, and the reason is the volume expression rather
    // than luck: bobbing is about 0.04 of motion, which lands the splash near
    // 0.03 -- two orders below the dive above, before distance attenuation
    // touches it. Measured here so the claim is checked rather than asserted.
    for (int i = 0; i < 240; ++i) {
        boats.tick(pool.w(), entity::VehicleRider{});
    }
    CHECK(pool.heard.countOf(mc::entity::kSplashSound) <= 4);
    for (const mc::test::Sound& s : pool.heard.sounds) {
        CHECK(s.volume < 0.05f);
    }
}

TEST(an_arrow_striking_a_wall_is_heard_where_it_struck_and_not_at_the_listener)
{
    Pool pool;
    entity::ArrowSystem arrows{11};

    // Fired straight down at the stone floor from well outside the pool, so
    // what it strikes is a block and the strike is certain. Pitch 90 is down.
    CHECK(arrows.shoot(pool.w(), -6.5, 70.0, 6.5, 0.0f, 90.0f));
    for (int i = 0; i < 60; ++i) {
        arrows.tick(pool.w());
    }

    const mc::test::Sound* drr = pool.heard.first("random.drr");
    CHECK(drr != nullptr);
    CHECK_EQ(double(drr->volume), 1.0);

    // **The whole of the bug this closed.** It used to be counted here and
    // played by the frame loop at the player's own ears, so an arrow that
    // landed across the valley sounded like one landing at your feet. It is the
    // arrow's own position now: the shaft is standing in the floor at the far
    // corner of the scene, not at the listener.
    CHECK(std::fabs(drr->x - (-6.5)) < 1.0);
    CHECK(std::fabs(drr->z - 6.5) < 1.0);

    // **Below where it was fired, and not at the block it hit**, which is the
    // jar's own ordering rather than a looseness here: `kg.e_()` backs the
    // shaft out of the face by a twentieth, plays the sound, and only then adds
    // the motion that carries it to the impact point. So the position is the
    // tick's start, a shaft-length short of the wall.
    CHECK(drr->y < 70.0);
    CHECK(drr->y > 64.0);

    // `1.2F / (rand * 0.2F + 0.9F)`, so the pitch runs 1.09 to 1.33 and is
    // never outside that.
    CHECK(drr->pitch >= 1.2f / 1.1f - 1e-6f);
    CHECK(drr->pitch <= 1.2f / 0.9f + 1e-6f);
}

TEST(an_animal_that_walks_into_water_splashes_and_one_that_is_spawned_in_it_does_not)
{
    Pool pool;
    entity::MobSystem mobs{7777};
    entity::MobSurroundings around;

    // Spawned standing in the pool. The first tick is `firstUpdate`, so it is
    // silent -- a herd loaded with the world must not all splash at once.
    CHECK(mobs.spawn(pool.w(), entity::MobType::Pig, 0.5, 64.0, 0.5, 0.0f));
    for (int i = 0; i < 5; ++i) {
        mobs.tick(pool.w(), around);
    }
    CHECK_EQ(pool.heard.countOf(mc::entity::kSplashSound), 0);

    // Lifted out onto the stone and dropped back in: that is an edge.
    mobs.at(0).body.setFeet(6.5, 66.0, 6.5);
    for (int i = 0; i < 10; ++i) {
        mobs.tick(pool.w(), around);
    }
    CHECK_EQ(pool.heard.countOf(mc::entity::kSplashSound), 0);

    mobs.at(0).body.setFeet(0.5, 68.0, 0.5);
    for (int i = 0; i < 40; ++i) {
        mobs.tick(pool.w(), around);
    }
    CHECK_EQ(pool.heard.countOf(mc::entity::kSplashSound), 1);
}

TEST(an_item_thrown_into_lava_fizzes_before_it_goes)
{
    SoundCatcher heard;
    SceneWorld scene{0, 0};
    for (i32 x = -4; x <= 4; ++x) {
        for (i32 z = -4; z <= 4; ++z) {
            scene.place(x, 62, z, bid(mcver::Block::Stone), 0);
            scene.place(x, 63, z, bid(mcver::Block::Lava), 0);
        }
    }
    scene.lightColumnsFrom(-4, 4, -4, 4, 64);
    heard.watch(scene.w());

    entity::ItemEntitySystem items{4242};
    CHECK(items.spawn(scene.w(), 0.5, 66.0, 0.5, item::paletteItem(0), 1, 0));
    for (int i = 0; i < 40; ++i) {
        items.tick(scene.w());
    }

    const mc::test::Sound* fizz = heard.first("random.fizz");
    CHECK(fizz != nullptr);

    // **Its own numbers, and they are not the entity splash's.** `0.4F` and
    // `2.0F + rand.nextFloat() * 0.4F` -- one draw rather than two, so every
    // one of them is at or above two.
    CHECK_EQ(double(fizz->volume), double(0.4f));
    CHECK(fizz->pitch >= 2.0f);
    CHECK(fizz->pitch <= 2.4f);

    // **The branch has no edge test**, unlike the water branch above it: a
    // stack that hops, falls back and hops again fizzes every tick its centre
    // is in a lava cell. What stops it happening twice here is that it does not
    // live long enough -- `moveEntity`'s tail has been burning it since the
    // tick its box first touched the lava, one point of five a tick, and the
    // hop it is heard making is its last. This used to be a count above one,
    // and that was the missing tail rather than the sound. See
    // core/entity/fire_entry.hpp.
    CHECK(heard.countOf("random.fizz") >= 1);

    // And the stack is gone inside the forty ticks above, hop and all.
    CHECK_EQ(items.count(), 0);
}

// ---------------------------------------------------------------------------
// The half that was actually broken.
// ---------------------------------------------------------------------------

TEST(a_key_that_is_played_is_a_key_that_is_preloaded)
{
    // **An unpreloaded key is silence, deliberately and without an error** --
    // there is no filesystem in the per-frame path, so `playSoundFX` is a
    // handle and two floats and can be nothing else. That makes the boot list
    // the honest statement of what this port can be heard doing, and it is why
    // the animals shipped drawn, ticked, and mute.
    //
    // This cannot decode anything, so it checks the lists against each other:
    // every key `core/` can name has to be one `preloadEffects` decodes. The
    // right-hand side is spelled out rather than read back, because reading it
    // back off the same function would agree with itself.
    const std::vector<std::string> preloaded = {
        "random.click",      "random.door_open", "random.door_close", "fire.ignite",
        "random.pop",        "random.fizz",      "random.splash",     "random.drr",
        "random.bow",        "mob.chickenplop",  "random.fuse",       "random.explode",
        "mob.slimeattack",  "random.hurt",
    };

    auto isPreloaded = [&](const std::string& key) {
        for (const std::string& k : preloaded) {
            if (k == key) {
                return true;
            }
        }
        // The two derived halves. A footstep or break sound comes off the
        // block table and an animal's three come off `MobDef`, so anything in
        // either table is on the list by construction.
        for (int row = 0; row < mcver::kStepSoundCount; ++row) {
            const block::StepSound& sound = mcver::kStepSounds[row];
            if (!sound.silent() && (key == sound.step || key == sound.breakSound)) {
                return true;
            }
        }
        for (int row = 0; row < entity::kMobTypeCount; ++row) {
            const entity::MobDef& def = entity::mobDef(entity::MobType(row));
            for (const char* name : {def.livingSound, def.hurtSound, def.deathSound}) {
                if (name != nullptr && key == name) {
                    return true;
                }
            }
        }
        return false;
    };

    // Every mob's three, which is the set that was missing. **A null key is
    // `ge.c()`'s own answer** and not an omission -- the creeper and the slime
    // have no idle sound at all -- so a null is skipped rather than looked up.
    for (int row = 0; row < entity::kMobTypeCount; ++row) {
        const entity::MobDef& def = entity::mobDef(entity::MobType(row));
        for (const char* name : {def.livingSound, def.hurtSound, def.deathSound}) {
            if (name != nullptr) {
                CHECK(isPreloaded(name));
            }
        }
    }

    // The entity keys this change added, plus the two that were being played
    // by code that was never on the list.
    CHECK(isPreloaded(mc::entity::kSplashSound));
    CHECK(isPreloaded("random.drr"));
    CHECK(isPreloaded("random.bow"));
    CHECK(isPreloaded("random.fizz"));
    CHECK(isPreloaded("random.pop"));
    CHECK(isPreloaded("mob.chickenplop"));

    // And a footstep, so the derived half is exercised too.
    CHECK(isPreloaded(std::string(audio::stepCue(bid(mcver::Block::Grass)).key)));
}
