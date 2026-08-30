#pragma once

// The countdown that decides when background music starts. This is the whole
// of a1.1.2's random music, and it is nine lines of arithmetic.
//
// Transcribed from `of.c()` (SoundManager.playMusicTicker) in
// minecraft-a1.1.2_01-client.jar:
//
//     if (!loaded || options.musicVolume == 0.0F) return;
//     if (playing("BgMusic"))  return;
//     if (playing("streaming")) return;      // a record suppresses music
//     if (this.i > 0) { --this.i; return; }
//     SoundPoolEntry e = this.musicPool.getRandomSound();
//     if (e == null) return;
//     this.i = this.rand.nextInt(24000) + 24000;
//     backgroundMusic("BgMusic", e.url, e.name, false);
//     setVolume("BgMusic", options.musicVolume);
//     play("BgMusic");
//
// and from `of.<init>`, which seeds the counter at `rand.nextInt(12000)`.
//
// **The order of those lines is the behaviour.** The two `playing()` checks
// come *before* the decrement, so the counter does not advance while a track is
// playing: the gap between two tracks is 20-40 minutes of silence, and the
// wall-clock period is the track's own length *plus* that. A ticker that
// decremented unconditionally would start the next track the moment the last
// one ended, roughly twice as often as the real game, and would feel wrong long
// before anyone could say why. It is the one rule in this file worth a test of
// its own, and it has one.
//
// The counter is in **ticks at 20 Hz**, so 12000 is ten minutes and 24000 is
// twenty. Nothing here is in seconds, and nothing here reads a clock: it is
// stepped by whole world ticks, exactly as `Minecraft.i()` stepped the
// original, so a console that drops a frame does not drift the schedule.
//
// This class deliberately knows nothing about decoding, channels or ndsp. It is
// handed the state it is allowed to see and it answers with the entry to start,
// which is what makes the schedule testable on a host with no audio hardware at
// all.

#include "core/audio/sound_pool.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::audio {

// What `of.c()` reads before it does anything. Every field is one of the
// original's early-return conditions, in the original's order.
struct MusicState {
    // `of.g` -- the sound system came up. False when ndsp is missing its
    // firmware, when `audio` is off, or on the host harness.
    bool available = false;

    // `options.musicVolume`, 0..1. Exactly zero suppresses everything; the
    // original compares against 0.0F and so do we.
    float musicVolume = 1.0f;

    // `playing("BgMusic")`.
    bool musicPlaying = false;

    // `playing("streaming")` -- a record on a jukebox. a1.1.2 will not talk
    // over one, and neither does the counter.
    bool recordPlaying = false;
};

class MusicTicker {
public:
    // a1.1.2 seeds from `new Random()`, i.e. the wall clock, so its first track
    // lands somewhere in the first ten minutes and no two sessions agree.
    // Seeding explicitly costs no fidelity -- the distribution is identical --
    // and buys a test that can assert on an exact schedule.
    explicit MusicTicker(i64 seed);

    // One world tick. Returns the entry to start playing, or null for "nothing
    // happens", which is the answer on all but a handful of ticks in an hour.
    const SoundEntry* tick(SoundPool& musicPool, const MusicState& state);

    // Ticks left before the next track may start. For the debug overlay and for
    // tests; nothing in the game reads it.
    i32 ticksRemaining() const { return counter_; }

private:
    JavaRandom rand_;
    i32 counter_;
};

}  // namespace mc::audio
