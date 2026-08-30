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
// **It is narrow because that is all there is to drive.** It grew once, and
// only once: the menus click, so one-shot effects arrived with their first
// emitter and not before. What is still absent is the *positional* half of
// a1.1.2's SoundManager -- an attenuated source at a world coordinate -- which
// needs a listener, and this port has no player body to put one on. It arrives
// with its own first emitter, on the same terms. See docs/audio-a1.1.2.md.
//
// The two halves have deliberately different shapes. Music is handed over as a
// `PcmSource` because it is minutes long and must be decoded as it plays; an
// effect is handed over already decoded, once, and afterwards played by handle.
// That is not a stylistic split: a click has to be audible on the frame the
// button was pressed, and a card read on that frame is the one thing
// CONTRIBUTING forbids outright. See core/audio/sample.hpp.
//
// The one rule for every implementation: **`available()` false means silence,
// not failure.** A console without a dumped DSP firmware, a build without a
// decoder and a player who turned audio off all arrive here identically, and
// none of them is an error path. Nothing above this interface may treat a false
// as something to report, retry or work around.

#include "core/audio/pcm_source.hpp"
#include "core/audio/sample.hpp"
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

    // Takes a decoded effect and returns the handle it will be played by, or
    // `kNoSample` if it would not take it -- which is not an error and never
    // reported as one: a backend that plays nothing takes nothing, and every
    // caller already treats a missing sample as silence.
    //
    // **Boot-time only.** It may copy, allocate and talk to hardware, none of
    // which belongs on a frame, and the samples it holds live until shutdown.
    // There is no matching `removeSample`: a handful of interface sounds are
    // loaded once and the game never stops needing them, so a free list would
    // be machinery with no caller.
    virtual SampleId addSample(const Sample& sample) = 0;

    // `of.a(name, vol, pitch)` below the seam -- one shot, fire and forget.
    // `gain` is the final 0..1 number with a1.1.2's 0.25f interface factor and
    // the player's sound volume already folded in, and `pitch` multiplies the
    // sample's own rate, 1.0 being the file as recorded.
    //
    // There is no handle, no stop and no query, because there is nothing to ask:
    // a1.1.2 rotates 256 source names and lets the oldest be overwritten
    // without ever looking at them again, and a fixed ring of voices here is
    // the same bargain in less memory. An unknown or `kNoSample` handle is
    // silence.
    virtual void playSample(SampleId id, float gain, float pitch) = 0;

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
    SampleId addSample(const Sample&) override { return kNoSample; }
    void playSample(SampleId, float, float) override {}
    void update() override {}
};

}  // namespace mc::audio
