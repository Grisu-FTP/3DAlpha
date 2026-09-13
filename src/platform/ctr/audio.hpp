#pragma once

// ndsp behind `audio::Backend`: one streamed voice, a ring of wave buffers in
// linear memory, and a missing DSP firmware that costs the player silence
// rather than a boot failure.
//
// **The firmware is the reason this file is defensive.** 3DS homebrew audio
// needs `sdmc:/3ds/dspfirm.cdc`, dumped from the player's own console with
// Luma3DS's Rosalina menu; Nintendo's copyright means it can never ship with
// the game. `ndspInit()` returns an error when it is absent, and the only
// correct response is to carry on quietly -- docs/assets.md:320 has said so
// since before anything called ndsp. So `available()` is false, every other
// method is a no-op, and nothing above here knows the difference between that
// and a player who turned audio off.
//
// **Why a decode thread and not the ndsp callback.** libctru runs its own
// high-priority ndsp thread and will call a callback from it once per audio
// frame. Decoding Vorbis there would put a few milliseconds of IMDCT on the
// thread that feeds the DSP, which is the one place it must never be. So the
// callback does one thing -- signal an event -- and a worker owned by this file
// does the decoding. See core/util/worker.hpp for why that worker cannot be a
// `std::thread`.
//
// **Depth, not priority, is what makes it work.** The decode thread runs
// *below* the main thread, so it cannot cost a frame; the price is that it only
// runs in the slack the main thread leaves while blocked on VBlank, and on an
// Old 3DS that slack is all there is -- a 3DSX has core 0 and nothing else, so
// CONTRIBUTING's "no decompression on core 0" cannot be honoured literally and
// is honoured in substance instead: never on the main thread, bounded to one
// buffer per wake, and buffered deeply enough that a missed frame is inaudible.
// kRingBuffers x kFramesPerBuffer is a third of a second of audio; a console
// that cannot decode 23 ms of Vorbis in a third of a second has a bigger
// problem than music.
//
// **The effect voices are the opposite arrangement, on purpose.** A one-shot is
// already decoded when it gets here (see core/audio/sample.hpp), so it is
// copied into linear memory once at boot and played straight out of it: no
// decode, no wake, no lock, and nothing for the worker to do. The main thread
// starts it and the DSP finishes it. That is what lets a menu click be audible
// on the frame the button was pressed rather than a worker wake later.
//
// Everything the DSP reads lives in linear memory and is flushed out of the
// data cache immediately before it is handed over. All of it is allocated once,
// at init or at boot, so nothing here allocates on a frame.

#include "core/audio/backend.hpp"
#include "core/audio/pcm_source.hpp"
#include "core/util/types.hpp"

#include <3ds.h>

#include <atomic>
#include <memory>

namespace mc::ctr {

// 1024 frames is ~23 ms at 44.1 kHz. Small on purpose: the longest a single
// decode can hold core 0 against the main thread is one buffer's worth, so a
// bigger block would trade an inaudible benefit for a visible stall.
inline constexpr int kFramesPerBuffer = 1024;

// Eight buffers is ~186 ms, or six frames at 30 fps. That is the jitter budget
// that lets the decode thread sit below the main thread.
inline constexpr int kRingBuffers = 8;

// 1024 frames x 2 channels x 2 bytes x 8 = 32 KB of linear memory, taken once.
inline constexpr usize kRingBytes =
    usize(kFramesPerBuffer) * 2 * sizeof(mc::i16) * usize(kRingBuffers);

// Effect voices, one ndsp channel each, handed out round-robin. a1.1.2 rotates
// 256 paulscode sources and overwrites the oldest without asking; four is the
// same bargain sized for what can actually overlap here -- a menu click is a
// tenth of a second and nothing in this port emits two sounds in a frame -- and
// it leaves nineteen of ndsp's twenty-four channels free for what comes later.
inline constexpr int kEffectVoices = 4;

// How many distinct effects the backend will hold.
//
// **80, and it is measured rather than reasoned about.** It was 48, chosen off
// the block table alone: nine StepSound singletons name six distinct keys --
// `step.stone`, `step.wood`, `step.gravel`, `step.grass`, `step.cloth`,
// `step.sand` -- plus `random.glass` and the menu's `random.click`, which came
// to 35 files against Mojang's own a1.1.2-era set.
//
// That was already a fiction by the time the animals landed, in two ways. The
// four of them name eight `mob.*` keys, and `Entity` itself names `random.
// splash`, `random.drr` and `random.bow`, none of which any table knows about
// -- see core/audio/effect_preload.hpp, which is now the one list. And a
// player does not copy the a1.1.2-era folder, because that server has been
// gone for years; they copy whatever resources tree they have, and a modern
// one carries **eight** variants of `step.grass` where the old one carried six.
//
// `--audio-list <resources>` decodes the whole boot set on the host exactly as
// this backend decodes it at boot and prints what it cost. Against the real
// tree on this machine, measured twice:
//
//     72 samples, 1,780,061 frames, 3,500.8 KB   -- the animals
//     108 samples, 3,386,798 frames, 6,638.9 KB  -- and the monsters
//
// **The monsters cost half as much again as everything before them put
// together**, which is not a surprise once the keys are counted: five kinds
// name eleven distinct `getSound` keys between them, and `mob.zombie`,
// `mob.zombiehurt` and their kin ship with two or three variants each, plus
// `random.fuse`, `random.explode`, `mob.slimeattack` and `random.hurt`.
//
// So 108 is the floor for a full modern folder and **128** is that plus room
// for a pack with a few more variants. At the measured 61.5 KB a sample the cap
// is ~7.9 MB of linear memory, against the 16 MB `__system_allocateHeaps`
// reserves for everything that is not the mesh pool (docs/3ds-performance.md
// -- the linear/heap split). That is now half the reserve rather than a
// quarter of it, and it is the number to re-measure before anything else large
// goes in. a1.1.2 ships no sounds at all, so the common case is still zero.
//
// **Partial loading remains the thing to avoid, and the cap is not a loader.**
// `playSoundFX` draws its variant *before* it knows whether that file is
// resident -- the original draws there, and moving the test in front of it
// would make the sequence depend on what happens to be in memory -- so a key
// with four of its eight variants loaded is a footstep that is silent half the
// time. Raising the cap past the measurement is what keeps that from
// happening; it is not enforced per key.
inline constexpr int kMaxSamples = 128;

// Why ndsp did not come up. The distinction matters to the player: someone who
// has already dumped their firmware must not be told to dump it again.
enum class AudioStatus : mc::u8 {
    Ready,
    NoFirmware,   // sdmc:/3ds/dspfirm.cdc is missing -- the common case
    Unavailable,  // the DSP is held by something else, or ndsp refused
    Disabled,     // the player turned audio off; ndsp was never initialised
};

class NdspBackend final : public mc::audio::Backend {
public:
    NdspBackend() = default;
    ~NdspBackend() override;

    // `enabled` is the `audio` setting. False means ndsp is never initialised
    // and no thread is spawned, which is exactly what the options contract in
    // docs/3ds-performance.md promises the toggle does.
    void init(bool enabled);
    void shutdown();

    AudioStatus status() const { return status_; }

    bool available() const override { return status_ == AudioStatus::Ready; }
    bool playMusic(std::unique_ptr<mc::audio::PcmSource> source, float gain) override;
    void stopMusic() override;
    bool musicPlaying() const override;
    void setMusicGain(float gain) override;
    mc::audio::SampleId addSample(const mc::audio::Sample& sample) override;
    void playSample(mc::audio::SampleId id, float gain, float pitch) override;
    void update() override;

    // For the debug overlay. Underruns are the number that says whether the
    // decode thread is keeping up on this console, and it is the one thing
    // about this subsystem a host cannot answer.
    mc::u32 underruns() const { return underruns_.load(std::memory_order_relaxed); }
    mc::u32 decodeMicros() const { return decodeMicros_.load(std::memory_order_relaxed); }

private:
    static void threadEntry(void* self);
    void run();
    void fillBuffers();
    void applyGain();

    // The music channel. Channel 0 of 24; 1..4 are the effect voices below and
    // the rest stay free for the records and positional sounds that arrive with
    // their own first emitter.
    //
    // **The split by channel is what makes the threading safe.** Channel 0 is
    // touched only by the decode thread once a track is running; the effect
    // channels are touched only by the main thread. So the two never name the
    // same channel and neither needs a lock of ours -- which is the claim this
    // rests on, and it is a claim about *our* code, checkable by reading it.
    //
    // It does assume libctru's per-channel state is not one shared structure
    // two threads can tear. That is what its API implies and what the music
    // path has assumed since it was written -- `ndspChnWaveBufAdd` already runs
    // on the decode thread while the main thread calls `ndspChnWaveBufClear`
    // from `stopMusic` -- but libctru's sources are not installed here and it
    // has not been read. ThreadSanitizer cannot reach this either: the decode
    // thread is 3DS-only. Recorded as an assumption, not as a measured fact.
    static constexpr int kMusicChannel = 0;
    static constexpr int kFirstEffectChannel = 1;

    AudioStatus status_ = AudioStatus::Disabled;

    void* thread_ = nullptr;
    LightEvent wake_{};
    LightLock lock_{};

    ndspWaveBuf buffers_[kRingBuffers]{};
    mc::i16* ring_ = nullptr;

    // One decoded effect, in memory the DSP can reach. `data` is its own
    // linearAlloc rather than a slice of a pool: the samples are taken once at
    // boot and freed once at shutdown, so a pool would be an allocator with two
    // calls in its life.
    struct LoadedSample {
        mc::i16* data = nullptr;
        mc::u32 frames = 0;
        int channels = 1;
        int sampleRate = 44100;
    };

    // Main thread only, all of it: written at boot by addSample, read on the
    // frame that plays a sound, and never seen by the decode thread.
    LoadedSample samples_[kMaxSamples]{};
    int sampleCount_ = 0;
    ndspWaveBuf voices_[kEffectVoices]{};
    int nextVoice_ = 0;

    // Owned by the decode thread once `generation_` has been published. The
    // main thread only ever swaps it under `lock_`.
    std::unique_ptr<mc::audio::PcmSource> source_;
    std::unique_ptr<mc::audio::PcmSource> pending_;

    // Bumped on every start and stop. A buffer decoded for an older generation
    // is dropped rather than played, which is what makes "stop the music
    // because the volume went to zero" safe in the middle of a decode.
    std::atomic<mc::u32> generation_{0};

    std::atomic<bool> playing_{false};
    std::atomic<bool> running_{false};
    std::atomic<float> gain_{1.0f};
    std::atomic<mc::u32> underruns_{0};
    std::atomic<mc::u32> decodeMicros_{0};
};

}  // namespace mc::ctr
