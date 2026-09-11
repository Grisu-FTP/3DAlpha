// Footsteps, and the three block sounds around them.
//
// The trigger is `Entity.moveEntity`'s and lives on the body, so it can be
// driven here with no audio at all: what `move()` decides is *which block* the
// step belongs to, and `audio::stepCue` turns that into a key, a volume and a
// pitch. Both halves are checked; nothing here needs a backend.

#include "core/audio/block_sound.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/block/registry.hpp"
#include "core/entity/player_body.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>
#include <string>

using namespace mc;
using mc::block::BlockId;
using mc::entity::PlayerBody;
using mc::entity::PlayerInput;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// A floor of one block type, and a player standing on it.
struct Walk {
    SceneWorld world{0, 0};
    PlayerBody body;

    explicit Walk(mcver::Block floor)
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 24; ++z) {
                world.place(x, 63, z, bid(floor), 0);
            }
        }
        body.setFeet(0.5, 64.0, 0.5);
    }

    // Walk forward for `ticks` and collect the blocks that made a footstep.
    int walk(int ticks, bool sneak = false)
    {
        PlayerInput input;
        input.forward = 1.0f;
        input.yawDegrees = 0.0f;
        input.sneak = sneak;

        int steps = 0;
        for (int i = 0; i < ticks; ++i) {
            body.tick(world.w(), input);
            if (body.stepSoundDue != block::kAir) {
                ++steps;
                last = body.stepSoundDue;
            }
        }
        return steps;
    }

    BlockId last = block::kAir;
};

}  // namespace

TEST(walking_makes_a_footstep_about_once_a_block)
{
    Walk w(mcver::Block::Grass);
    const double startZ = w.body.z;
    const int steps = w.walk(60);
    const double walked = w.body.z - startZ;

    CHECK(steps > 0);
    // `distanceWalkedModified` accumulates 0.6 of the distance covered and pays
    // out one step per whole unit of it, so the count trails the blocks walked
    // by roughly that factor rather than matching it.
    CHECK(steps <= int(walked) + 1);
    CHECK(steps >= int(walked * 0.6) - 1);
    CHECK_EQ((long long) w.last, (long long) int(mcver::Block::Grass));
}

TEST(sneaking_is_silent)
{
    Walk w(mcver::Block::Grass);
    CHECK_EQ(w.walk(60, /*sneak=*/true), 0);
}

TEST(standing_still_never_steps)
{
    Walk w(mcver::Block::Grass);
    PlayerInput input;
    for (int i = 0; i < 40; ++i) {
        w.body.tick(w.world.w(), input);
        CHECK_EQ((long long) w.body.stepSoundDue, 0LL);
    }
}

TEST(the_footstep_is_the_block_underfoot_and_snow_on_top_wins)
{
    Walk w(mcver::Block::Stone);
    // A layer of snow over the whole walkway: an inch of snow is what you hear.
    for (i32 z = -8; z <= 24; ++z) {
        for (i32 x = -8; x <= 8; ++x) {
            w.world.place(x, 64, z, bid(mcver::Block::SnowLayer), 0);
        }
    }
    CHECK(w.walk(60) > 0);
    CHECK_EQ((long long) w.last, (long long) int(mcver::Block::SnowLayer));
}

TEST(flying_earns_no_footstep_and_banks_no_distance_for_later)
{
    // **The Creative bug this was written for.** `moveEntity` accumulates
    // walking distance whether or not there is a floor to hear, and only spends
    // it where the cell under the feet is not air -- so a flight across the sky
    // used to bank one block of credit per block travelled and cash the whole
    // lot in as a burst of footsteps the moment the player skimmed low ground.
    Walk w(mcver::Block::Grass);

    PlayerInput input;
    input.forward = 1.0f;
    input.yawDegrees = 0.0f;

    // Up into open air, then forward over the walkway, then back down onto it
    // and forward again -- the whole shape of the report. No leg may earn a
    // step, and -- the half that made it audible -- no leg may leave anything
    // banked behind it for the next one to spend.
    const auto fly = [&](int ticks, bool up, bool down) {
        for (int i = 0; i < ticks; ++i) {
            w.body.tickFlying(w.world.w(), input, up, down, mc::entity::kFlightSpeed);
            CHECK_EQ((long long) w.body.stepSoundDue, 0LL);
        }
    };

    fly(20, /*up=*/true, false);
    fly(16, false, false);
    CHECK_EQ(w.body.distanceWalked, 0.0f);
    CHECK_EQ(w.body.nextStepDistance, 1);

    // Down onto the floor, and then skimming along it -- which is exactly where
    // the banked credit used to be cashed in.
    fly(40, false, /*down=*/true);
    fly(16, false, false);
    CHECK_EQ(w.body.distanceWalked, 0.0f);
    CHECK_EQ(w.body.nextStepDistance, 1);
}

TEST(walking_after_a_flight_still_steps_on_its_own_schedule)
{
    // The other half of the fix: suppressing the flight must not suppress the
    // walk that follows it, and must not leave the counter where a first step
    // is owed immediately.
    Walk w(mcver::Block::Grass);
    PlayerInput input;
    input.forward = 1.0f;
    input.yawDegrees = 0.0f;
    for (int i = 0; i < 30; ++i) {
        w.body.tickFlying(w.world.w(), input, false, false, mc::entity::kFlightSpeed);
    }
    // Back at the start of the walkway, and the very next tick must not be a
    // footstep -- a counter left owing one would fire immediately.
    w.body.setFeet(0.5, 64.0, 0.5);
    w.body.tick(w.world.w(), input);
    CHECK_EQ((long long) w.body.stepSoundDue, 0LL);
    CHECK(w.walk(80) > 0);
}

TEST(a_liquid_underfoot_makes_no_sound)
{
    // Standing in water: `Material.isLiquid()` silences the step, and the
    // silence is a step that was *earned* -- the counter still advanced.
    Walk w(mcver::Block::Water);
    CHECK_EQ(w.walk(60), 0);
}

// ---------------------------------------------------------------------------
// The three cues
// ---------------------------------------------------------------------------

TEST(the_three_cues_are_the_three_call_sites)
{
    const BlockId grass = bid(mcver::Block::Grass);
    const block::StepSound& sound = block::stepSoundOf(grass);
    CHECK(!sound.silent());

    const audio::SoundCue step = audio::stepCue(grass);
    CHECK_EQ(std::string(step.key), std::string("step.grass"));
    CHECK_EQ(double(step.volume), double(sound.volume * 0.15f));
    CHECK_EQ(double(step.pitch), double(sound.pitch));

    const audio::SoundCue broke = audio::breakCue(grass);
    CHECK_EQ(std::string(broke.key), std::string("step.grass"));
    CHECK_EQ(double(broke.volume), double((sound.volume + 1.0f) / 2.0f));
    CHECK_EQ(double(broke.pitch), double(sound.pitch * 0.8f));

    // A block is *placed* with its step sound at the break volume, which is
    // ItemBlock.onItemUse playing the wrong-looking getter on purpose.
    const audio::SoundCue placed = audio::placeCue(grass);
    CHECK_EQ(std::string(placed.key), std::string(step.key));
    CHECK_EQ(double(placed.volume), double(broke.volume));
    CHECK_EQ(double(placed.pitch), double(broke.pitch));
}

TEST(sand_and_glass_break_with_a_different_sound_than_they_are_walked_on)
{
    // The two `StepSound` subclasses, and the whole reason the table carries
    // two names per row.
    CHECK_EQ(std::string(audio::stepCue(bid(mcver::Block::Sand)).key),
             std::string("step.sand"));
    CHECK_EQ(std::string(audio::breakCue(bid(mcver::Block::Sand)).key),
             std::string("step.gravel"));

    CHECK_EQ(std::string(audio::stepCue(bid(mcver::Block::Glass)).key),
             std::string("step.stone"));
    CHECK_EQ(std::string(audio::breakCue(bid(mcver::Block::Glass)).key),
             std::string("random.glass"));
}

TEST(metal_is_the_one_row_with_a_pitch_of_its_own)
{
    // Iron and gold blocks and the two doors: `("stone", 1.0F, 1.5F)`.
    const audio::SoundCue iron = audio::stepCue(bid(mcver::Block::IronBlock));
    CHECK_EQ(std::string(iron.key), std::string("step.stone"));
    CHECK_EQ(double(iron.pitch), 1.5);
}

TEST(air_and_an_unknown_id_are_silent)
{
    CHECK(!audio::stepCue(block::kAir).playable());
    CHECK(!audio::breakCue(block::kAir).playable());
    CHECK(!audio::stepCue(BlockId(250)).playable());
}

// ---------------------------------------------------------------------------
// The positional gain
// ---------------------------------------------------------------------------

TEST(a_positional_sound_fades_linearly_over_sixteen_blocks)
{
    // `ATTENUATION_LINEAR` over a 16-block fade distance, and **no 0.25
    // interface factor** -- which is the difference that makes a footstep at
    // 0.15 audible at all.
    CHECK_EQ(double(audio::positionalGain(1.0f, 0.0f, 1.0f)), 1.0);
    CHECK_EQ(double(audio::positionalGain(1.0f, 8.0f, 1.0f)), 0.5);
    CHECK_EQ(double(audio::positionalGain(1.0f, 16.0f, 1.0f)), 0.0);
    CHECK_EQ(double(audio::positionalGain(1.0f, 100.0f, 1.0f)), 0.0);

    // The player's slider is a plain multiplier.
    CHECK_EQ(double(audio::positionalGain(1.0f, 8.0f, 0.5f)), 0.25);

    // **Above 1 the range stretches and the gain does not**: twice as loud is
    // twice as far, clamped back to 1 at the source.
    CHECK_EQ(double(audio::positionalGain(2.0f, 0.0f, 1.0f)), 1.0);
    CHECK_EQ(double(audio::positionalGain(2.0f, 16.0f, 1.0f)), 0.5);
    CHECK(audio::positionalGain(2.0f, 31.0f, 1.0f) > 0.0f);

    // Silence is silence.
    CHECK_EQ(double(audio::positionalGain(0.0f, 1.0f, 1.0f)), 0.0);
    CHECK_EQ(double(audio::positionalGain(1.0f, 1.0f, 0.0f)), 0.0);
}
