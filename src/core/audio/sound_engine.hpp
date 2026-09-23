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
// **Effects are loaded before they are asked for, and that is the one place
// this class departs from `of`.** a1.1.2 hands paulscode a URL at the moment of
// the click and lets the library read the file; here the file is read and
// decoded by `preloadSound` at boot, and `playSoundFX` is a handle and two
// floats. The reason is CONTRIBUTING's rule about the per-frame path, not
// taste: the frame that clicks cannot touch an SD card. Which sound is drawn
// and how loud it is are `of.a`'s, unchanged.
//
// A sound that was never preloaded is silence -- deliberately, and not an
// error. It is the same degradation as a missing resources folder, and it is
// what makes the preload list an honest statement of what this port can
// actually make a noise about. That list is short today because the menus are
// the only emitter; when block placement lands and `dig.*` needs three hundred
// files that cannot all be resident, the answer is a decode request queued onto
// the audio worker, and this is the interface it will arrive behind.
//
// What is *not* here at all: positional attenuation and records. Both need
// something the port has not got -- a listener with a position, a jukebox --
// so they arrive with their first caller rather than as dead code. The seams
// they will hang off are the pools below and `Backend`.
// See docs/audio-a1.1.2.md.

#include "core/audio/backend.hpp"
#include "core/audio/music_ticker.hpp"
#include "core/audio/resource_index.hpp"
#include "core/audio/sample.hpp"
#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc::audio {

// `of.a(String, float, float)`'s volume arithmetic, exposed on its own for the
// same reason `poolKey` is: it can then be checked without a decoder, a file, a
// backend or a console, and it is the half of the interface path that is easy
// to get wrong.
//
//     if (volume > 1.0F) volume = 1.0F;
//     volume = volume * 0.25F;
//     SoundSystem.setVolume(src, volume * options.soundVolume);
//
// The 0.25f is the interface factor. Music does not have it and neither do
// positional sounds; see the volume table in docs/audio-a1.1.2.md.
float interfaceGain(float volume, float soundVolume);

// `of.b(String, float, float, float, float, float)`'s volume arithmetic -- the
// *positional* path, which is a different sum from the interface one and not
// only by a factor:
//
//     float range = 16.0F;
//     if (volume > 1.0F) range = 16.0F * volume;
//     sndSystem.newSource(..., x, y, z, ATTENUATION_LINEAR, range);
//     sndSystem.setPitch(src, pitch);
//     if (volume > 1.0F) volume = 1.0F;
//     sndSystem.setVolume(src, volume * options.soundVolume);
//
// **There is no 0.25f here**, which is why a footstep at `volume * 0.15` is
// still audible; and a volume above 1 does not get louder, it gets *further* --
// it stretches the range and is then clamped back to 1.
//
// **The falloff itself is the library's, not the jar's.** `ATTENUATION_LINEAR`
// is paulscode's, and paulscode fades a source linearly to nothing across the
// distance it is given. That rule is transcribed here; what cannot be is the
// stereo panning around it, which needs a listener orientation the seam does
// not carry. So a sound in this port is attenuated by distance and centred.
float positionalGain(float volume, float distance, float soundVolume);

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

    // `options.soundVolume`, 0..1, and a plain multiplier: unlike music, an
    // effect already playing is not restarted or stopped when it moves -- there
    // is nothing to stop, since every one of them is over in a fraction of a
    // second. Zero suppresses the lot, which is `of.a`'s own first line.
    void setSoundVolume(float volume);
    float soundVolume() const { return soundVolume_; }

    // Decodes every entry in the sound pool keyed `key` -- `random.click`
    // covers `random/click.ogg`, `step.grass` would cover `grass1..grass6` --
    // and hands each to the backend. Returns how many were taken.
    //
    // **This opens files and runs the decoder**, so it belongs at boot beside
    // `loadResources` and nowhere near a frame. Calling it twice for the same
    // key loads nothing the second time.
    //
    // While a background preload is running, only its worker may call this:
    // what it decodes is then staged for `pumpPreload` rather than committed,
    // and the count is of what was staged.
    usize preloadSound(std::string_view key);

    // **The rest of the effects, decoded while the menu is up.** Runs `list`
    // -- `preloadEffects` -- on an audio worker and returns at once; what it
    // decodes waits in a `SampleStage` until `pumpPreload` commits it on this
    // thread. Keys already loaded are skipped, so the click the first menu
    // needs can be preloaded first and joined. False, and nothing loads, if a
    // preload is already running or no worker could be started: decoding
    // inline would put Tremor on a 32 KB main stack.
    //
    // An effect played before its turn is silence, as it would be with no
    // file for it. See core/audio/sample_stage.hpp.
    bool startPreload(usize (*list)(SoundEngine&));

    // Main thread, once a frame -- `update` does it in game; the menu calls it
    // itself. Commits what is waiting and joins the worker once it is done.
    // With nothing waiting it is a lock and a compare.
    void pumpPreload();

    bool preloading() const { return preload_ != nullptr; }

    // Abandons the preload and joins its worker; what was not committed yet is
    // dropped. Before the backend is shut down, and safe to call when nothing
    // is running. The destructor does it too.
    void stopPreload();

    ~SoundEngine();
    SoundEngine(const SoundEngine&) = delete;
    SoundEngine& operator=(const SoundEngine&) = delete;

    // `of.a(String, float, float)` -- playSoundFX, the interface path. The
    // entry is drawn uniformly among the ones sharing `key`, from the sound
    // pool's own Random, and the gain is the original's arithmetic exactly:
    //
    //     if (volume > 1) volume = 1;
    //     volume *= 0.25F;
    //     setVolume(src, volume * options.soundVolume);
    //
    // That 0.25f is the interface factor and it is audible; music has no such
    // thing, and positional sounds do not apply it either. See the volume table
    // in docs/audio-a1.1.2.md.
    //
    // Safe to call on every frame and from anywhere: with no backend, no
    // resources, a zero volume or an unloaded sound it does nothing at all.
    void playSoundFX(std::string_view key, float volume = 1.0f, float pitch = 1.0f);

    // `of.b(...)` -- the positional path: a footstep, a block breaking, a door.
    // Attenuated against the listener set by `setListener`, and silent past the
    // range. Safe to call from anywhere on any frame, on the same terms as
    // `playSoundFX`.
    void playSoundAt(std::string_view key, double x, double y, double z,
                     float volume = 1.0f, float pitch = 1.0f);

    // Where the ears are. a1.1.2 sets this every frame from the render view
    // entity; nothing reads it but `playSoundAt`, and until it is called the
    // listener is at the origin -- which is a real position, so a game that
    // forgets to call it is quiet rather than silent.
    void setListener(double x, double y, double z);

    // How many decoded effects the backend is holding -- the options screen's
    // way of saying whether a click will actually make a noise.
    usize loadedSamples() const { return samples_.size(); }

    // One frame's worth of world ticks, straight from `TickTimer::elapsedTicks()`.
    // Zero is the common case at 30 fps and costs a compare.
    void tick(int elapsedTicks);

    // Once a frame, from the main thread, whether or not any tick elapsed --
    // the backend has buffers to recycle even on a frame that owes no tick.
    void update()
    {
        backend_.update();
        pumpPreload();
    }

    void stopMusic();

    // `of.a(String, FFFFF)` -- **playStreaming**, which is what a jukebox does
    // and the one sound in a1.1.2 that keeps going after the call:
    //
    // ```
    // if (!loaded || options.soundVolume == 0.0F) return;
    // if (playing("streaming")) stop("streaming");
    // if (name == null) return;
    // SoundPoolEntry e = streamingPool.getRandomSound(name);
    // if (e != null && volume > 0) {
    //     if (playing("BgMusic")) stop("BgMusic");
    //     newStreamingSource(..., x, y, z, ATTENUATION_LINEAR, 16.0F * 4.0F);
    //     setVolume("streaming", 0.5F * options.soundVolume);
    //     play("streaming");
    // }
    // ```
    //
    // **Three things about it are not the music path.** It is gated on
    // `soundVolume`, not `musicVolume`; its gain is a flat half of that rather
    // than the interface path's quarter; and its range is sixty-four blocks
    // rather than sixteen, so a disc is audible from four times as far as a
    // footstep.
    //
    // **A null `track` is the stop**, which is the original's own signal --
    // `BlockJukeBox.ejectRecord` calls `world.playRecord(null, ...)`. So is a
    // track this card has no file for.
    //
    // It rides the backend's one streaming voice, which the music was already
    // using, and that is faithful rather than a compromise: the original stops
    // `BgMusic` the moment a record starts and `playMusicTicker` will not start
    // one while `playing("streaming")`, so the two never overlap in a1.1.2
    // either.
    void playRecord(const char* track, double x, double y, double z);

    // True while a disc is on the streaming voice -- `playing("streaming")`,
    // which is the second thing `of.c()` asks before it touches its counter.
    bool recordPlaying() const;

    // **Stops a disc and leaves the background music alone**, which is the one
    // thing `stopMusic` cannot do: the two share a voice here, so a caller that
    // wanted to silence a jukebox and used `stopMusic` would cut a track the
    // menu is in the middle of.
    //
    // This is what leaving a world calls. a1.1.2 has nothing to do here --
    // quitting a world tears the whole `SoundManager` down with it -- but this
    // port keeps one engine for the life of the process, so a record left
    // playing followed the player all the way back to the title screen.
    // Reported from play.
    void stopRecord();

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

    // `0.5F * options.soundVolume`, attenuated from the disc to the listener
    // over the streaming source's own sixty-four-block range.
    float recordGain() const;

    // What a preloaded file is playable as. Keyed by path rather than by a
    // pointer into the pool because `SoundPool::add` grows a vector and would
    // invalidate one, and looked up by a linear scan for the same reason the
    // pool's own buckets are: this holds the handful of sounds a menu makes,
    // built once, and a hash map would cost more in allocation than the scan
    // ever costs in time.
    struct LoadedSample {
        std::string path;
        SampleId id = kNoSample;
    };

    SampleId sampleFor(std::string_view path) const;

    // Hands a decoded sample to the backend and makes it playable.
    void commitSample(const std::string& path, const Sample& sample);

    // Defined in the .cpp: the stage, the worker's handle and what it has
    // already decoded. Present only while a background preload runs.
    struct Preload;
    static void preloadEntry(void* arg);
    void joinPreload();

    io::FileSystem& fs_;
    Backend& backend_;
    ResourceIndex resources_;
    MusicTicker ticker_;
    std::vector<LoadedSample> samples_;
    std::unique_ptr<Preload> preload_;
    float musicVolume_ = 1.0f;
    float soundVolume_ = 1.0f;

    // Where the disc on the streaming voice is, so its gain can follow the
    // listener. `recordPlaying_` is ours rather than the backend's: the voice
    // is shared with the music and the backend cannot say which of the two is
    // on it.
    bool recordPlaying_ = false;
    double recordX_ = 0.0;
    double recordY_ = 0.0;
    double recordZ_ = 0.0;
    double listenerX_ = 0.0;
    double listenerY_ = 0.0;
    double listenerZ_ = 0.0;
};

}  // namespace mc::audio
