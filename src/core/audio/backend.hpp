#pragma once

// The audio output seam -- `IAudio` in docs/architecture.md's list of narrow
// platform interfaces, and the reason core never includes a DSP header.
//
// This is one of the few places the project accepts a vtable, and it qualifies
// on the same terms `io::FileSystem` does: it is the platform boundary, it is
// called at block granularity rather than per sample, and the host tests need
// something to inject. A recording backend is how the music schedule gets
// asserted without a console.
//
// **It is deliberately narrow, and it is narrow because that is all there is to
// drive yet.** a1.1.2's SoundManager also plays one-shot effects at a position
// with an attenuation model; none of that is here, because nothing in this port
// can emit a sound yet -- there is no block placement, no player body and no
// entity system, so a `playSample` added now would be a virtual with no caller
// and no test. It arrives with its first emitter. See docs/audio-a1.1.2.md.
//
// The one rule for every implementation: **`available()` false means silence,
// not failure.** A console without a dumped DSP firmware, a build without a
// decoder and a player who turned audio off all arrive here identically, and
// none of them is an error path. Nothing above this interface may treat a false
// as something to report, retry or work around.

#include "core/audio/pcm_source.hpp"
#include "core/util/types.hpp"

#include <memory>

namespace mc::audio {

class Backend {
public:
    virtual ~Backend() = default;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // False when the output never came up. Checked once by everything above
    // and then not asked again: it cannot change while the game runs.
    virtual bool available() const = 0;

    // Starts streaming `source`, taking ownership, and replaces whatever was
    // playing. False if it could not start, which is not an error either --
    // the ticker will simply try again when its counter next reaches zero.
    //
    // `gain` is 0..1 and is a1.1.2's `musicVolume`, unscaled. The original
    // applies no factor to music, unlike the 0.25 it applies to interface
    // sounds, and that difference is audible.
    virtual bool playMusic(std::unique_ptr<PcmSource> source, float gain) = 0;

    virtual void stopMusic() = 0;

    // `SoundSystem.playing("BgMusic")` -- the first thing `of.c()` asks, and
    // the reason the music counter does not run while a track is on. True from
    // the moment playMusic succeeds until the last decoded frame has actually
    // left the hardware, not until the decoder reaches the end of the file.
    virtual bool musicPlaying() const = 0;

    // Applied live, as `of.a()` does when the options screen changes. A gain of
    // exactly zero stops the track outright rather than playing it silently,
    // which is again what the original does.
    virtual void setMusicGain(float gain) = 0;

    // Once a frame, from the main thread. Must not allocate, must not block and
    // must not touch the card -- everything expensive belongs on the decode
    // thread. Most frames it does nothing at all.
    virtual void update() = 0;

protected:
    Backend() = default;
};

// The backend every build has: the one that plays nothing. It is what the host
// tests and the harness run against, and it is what a console with no DSP
// firmware gets, so the silent path is the same code on both.
class NullBackend final : public Backend {
public:
    bool available() const override { return false; }
    bool playMusic(std::unique_ptr<PcmSource>, float) override { return false; }
    void stopMusic() override {}
    bool musicPlaying() const override { return false; }
    void setMusicGain(float) override {}
    void update() override {}
};

}  // namespace mc::audio
