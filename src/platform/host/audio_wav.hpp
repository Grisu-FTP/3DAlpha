#pragma once

// A host `audio::Backend` that writes what it was handed to a .wav instead of
// to a speaker.
//
// The host harness has no audio output and this file deliberately does not add
// one. Playing sound on Linux would mean linking SDL or ALSA into a target that
// today links neither, for the sake of a convenience a file already provides:
// a .wav can be listened to, diffed, and looked at in a spectrum view, and it
// answers "does the music start when it should and is it the right track"
// without a console in the room.
//
// It is not only a debugging aid. It is the second implementation of the seam,
// and having two is what stops `audio::Backend` from quietly becoming the shape
// of ndsp -- the same argument docs/architecture.md makes about the renderer.
//
// Time here is simulated, not real: the harness tells it how many ticks have
// passed and it pulls exactly that many milliseconds of audio out of the
// source. So a twenty-hour schedule renders in the time it takes to decode it.
//
// **One-shot effects are taken and logged, not mixed.** They are accepted --
// `addSample` returns real handles, so `SoundEngine::preloadSound` exercises the
// whole decode path on the host -- and every play is recorded with the gain and
// pitch it was asked for, which is the part worth asserting: the interface
// factor and the volume arithmetic are `of.a`'s and are easy to get wrong.
// Mixing them into the .wav would mean deciding where in simulated time a click
// that happened in a menu belongs, and a menu has no tick count.

#include "core/audio/backend.hpp"
#include "core/util/types.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace mc::host {

class WavBackend final : public mc::audio::Backend {
public:
    // `path` may be empty, in which case nothing is written and the backend
    // still reports availability and playing state -- which is what the
    // schedule dump wants.
    explicit WavBackend(std::string path, int sampleRate = 44100);
    ~WavBackend() override;

    bool available() const override { return true; }
    bool playMusic(std::unique_ptr<mc::audio::PcmSource> source, float gain) override;
    void stopMusic() override;
    bool musicPlaying() const override { return source_ != nullptr; }
    void setMusicGain(float gain) override { gain_ = gain; }
    mc::audio::SampleId addSample(const mc::audio::Sample& sample) override;
    void playSample(mc::audio::SampleId id, float gain, float pitch) override;
    void update() override {}

    // What `playSample` was handed, in order. The gain here is final: the
    // 0.25f interface factor and the player's sound volume are already in it.
    struct SamplePlay {
        mc::audio::SampleId id = mc::audio::kNoSample;
        float gain = 0.0f;
        float pitch = 1.0f;
    };

    const std::vector<mc::audio::Sample>& samples() const { return samples_; }
    const std::vector<SamplePlay>& samplePlays() const { return samplePlays_; }

    // Advance simulated time by `ticks` world ticks at 20 Hz, pulling that much
    // audio through. This is the method that makes the harness a clock.
    void advance(int ticks);

    // Writes the header and closes. Safe to call twice.
    void finish();

    u64 framesWritten() const { return framesWritten_; }
    int tracksPlayed() const { return tracksPlayed_; }

private:
    void writeFrames(const i16* frames, usize count);

    std::string path_;
    std::FILE* file_ = nullptr;
    int sampleRate_;
    float gain_ = 1.0f;
    std::unique_ptr<mc::audio::PcmSource> source_;
    std::vector<i16> scratch_;
    std::vector<mc::audio::Sample> samples_;
    std::vector<SamplePlay> samplePlays_;
    u64 framesWritten_ = 0;
    int tracksPlayed_ = 0;
    bool closed_ = false;
};

}  // namespace mc::host
