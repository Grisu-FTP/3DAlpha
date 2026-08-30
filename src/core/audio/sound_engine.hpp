#pragma once

// a1.1.2's SoundManager, above the output seam: it owns the pools, the music
// counter and the volumes, and it is the one thing the game's frame loop talks
// to about sound.
//
// It exists so that `runGame` gains one line rather than five. The frame loop
// already knows how many whole world ticks it owes -- `TickTimer::elapsedTicks()`
// -- and that is exactly the currency the music counter is denominated in, so
// the whole of the integration is `sound.tick(elapsed)`.
//
// **Ticks, not seconds.** `of.c()` was called once per `Minecraft.runTick()`,
// so the schedule is measured in 20 Hz ticks and a console that drops frames
// does not drift it. Passing a wall clock in here instead would be a different
// game on a console that runs at 24 fps.
//
// What is *not* here yet: one-shot effects, positional attenuation and records.
// The machinery each needs is real, but nothing in this port can emit them --
// there is no block placement, no player body, no jukebox and no entities -- so
// they arrive with their first caller rather than as dead code. The seams they
// will hang off are the pools below and `Backend`. See docs/audio-a1.1.2.md.

#include "core/audio/backend.hpp"
#include "core/audio/music_ticker.hpp"
#include "core/audio/resource_index.hpp"
#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

namespace mc::audio {

class SoundEngine {
public:
    // `fs` and `backend` are borrowed and must outlive the engine; both are
    // process-lifetime objects in every caller. `seed` seeds the music counter
    // -- a1.1.2 uses `new Random()`, so the game passes a clock and the tests
    // pass a constant.
    SoundEngine(io::FileSystem& fs, Backend& backend, i64 seed);

    // Walks `kResourcesDir` and fills the pools. Safe to call when the folder
    // is absent: the pools stay empty and the game is silent. Returns the
    // number of entries found, which is what the options screen reports.
    usize loadResources();
    usize loadResources(std::string_view root);

    const ResourceIndex& resources() const { return resources_; }

    // 0..1. Setting it to exactly zero stops the current track, as `of.a()`
    // does when the options screen moves the slider to OFF.
    void setMusicVolume(float volume);
    float musicVolume() const { return musicVolume_; }

    // One frame's worth of world ticks, straight from `TickTimer::elapsedTicks()`.
    // Zero is the common case at 30 fps and costs a compare.
    void tick(int elapsedTicks);

    // Once a frame, from the main thread, whether or not any tick elapsed --
    // the backend has buffers to recycle even on a frame that owes no tick.
    void update() { backend_.update(); }

    void stopMusic();

    // For the debug overlay: ticks until the next track may start.
    i32 ticksUntilMusic() const { return ticker_.ticksRemaining(); }

    // True when this build and this console can actually make a sound. The
    // options screen uses it to explain the silence.
    bool available() const { return backend_.available(); }

private:
    // Starts `entry`, or gives up quietly. a1.1.2 hands the decoder a URL and
    // lets it fail later; we open the file here so a track that cannot be
    // decoded costs one failed open rather than a silent voice.
    void startTrack(const SoundEntry& entry);

    io::FileSystem& fs_;
    Backend& backend_;
    ResourceIndex resources_;
    MusicTicker ticker_;
    float musicVolume_ = 1.0f;
};

}  // namespace mc::audio
