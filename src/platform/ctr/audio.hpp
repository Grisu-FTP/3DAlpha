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
// **Depth alone does not make it work, and hardware said so: the music skipped
// once the load got high enough.** The reason is the scheduler, not the
// decoder. The ARM11 kernel is strictly priority-ordered with *no* round-robin
// and *no* time slice -- 3dbrew's Multi-threading page calls it SCHED_FIFO --
// so a runnable thread never preempts one of equal priority, and a thread below
// the running one gets nothing at all until that one blocks. This file and
// core/util/worker.hpp were both written against the opposite assumption, and
// it cost the same bug on either console:
//
//   * **New 3DS.** The decoder shared core 2 with the generation worker at the
//     *same* priority, and `WorldStreamer::workerMain` takes its own next job
//     the moment it finishes one. A full slate is a thread that never blocks,
//     so the decoder ran only in the gaps the generator's card reads left.
//   * **Old 3DS.** The decoder sits below the main thread on core 0, so it
//     lives on the slack the main thread leaves at VBlank -- and a frame that
//     is already over budget leaves none of that either.
//
// So depth buys time and priority is what spends it, and both are needed. The
// ring is ~370 ms rather than ~190; the decoder is now a step *above* the
// generation worker on a New 3DS (platform/ctr/main.cpp); and on either console
// it **raises its own priority above the main thread's when the ring runs low**
// and puts it back the moment the ring is full again -- see `queuedBuffers`,
// kBoostBelow and kRestoreAt.
//
// The steady state is therefore exactly what it was: below the main thread,
// costing no frame anything. The boost is bounded to kBoostBuffersPerPass
// buffers per wake, so the most a catch-up can take from the frame it
// interrupts is four buffers' decode -- and `boosts()` counts how often that
// happened, because a console where it never happens has lost nothing and a
// console where it happens constantly is one the priorities are still wrong on.
//
// CONTRIBUTING's "no decompression on core 0" is still honoured in substance
// rather than literally on an Old 3DS, which has no other core to offer: never
// on the main thread, bounded per wake, and deep enough that the boost has
// hundreds of milliseconds to notice and react.
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

// 1024 frames is ~23 ms at 44.1 kHz. Small on purpose: it is the grain
// everything else here is counted in, and the unit a boosted pass is capped in
// -- so it is also the shortest a decode can hold core 0 for. A bigger block
// would trade an inaudible benefit for a coarser stall.
inline constexpr int kFramesPerBuffer = 1024;

// Sixteen buffers is ~372 ms, or eleven frames at 30 fps. It was eight, and on
// hardware eight was not enough: it is the window the boost below has to notice
// a stall and catch up inside, so it is sized for the stall and not for the
// decoder. 32 KB more of linear memory is the whole of the price.
inline constexpr int kRingBuffers = 16;

// **When the decoder decides it is losing.** Both are counts of wave buffers the
// DSP still has in hand at the top of a pass: below kBoostBelow (~116 ms left)
// the decode thread raises its own priority above the main thread's, and at
// kRestoreAt (~279 ms) it puts it back. The gap between the two is hysteresis --
// a track that sat on the threshold would otherwise re-prioritise itself on
// every one of ndsp's ~5 ms wakes.
inline constexpr int kBoostBelow = 5;
inline constexpr int kRestoreAt = 12;

// How much a *boosted* pass may decode before it goes back to sleep. Boosted,
// this thread outranks the main thread, so an unbounded pass over an empty ring
// would hold a frame for sixteen buffers' worth of Vorbis. ndsp wakes it again
// in about 5 ms, so four buffers -- ~93 ms of audio -- catches up fast and is
// invisible in a frame time.
inline constexpr int kBoostBuffersPerPass = 4;

// 1024 frames x 2 channels x 2 bytes x 16 = 64 KB of linear memory, taken once.
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

    // How often the decode thread had to outrank the main thread to stay ahead,
    // and the fewest wave buffers the DSP had in hand at the top of a pass since
    // the track started. Together they are the answer to "is the ring deep
    // enough and is the priority right on this console": a low-water mark that
    // never approaches kBoostBelow means neither was ever tested, and boosts
    // climbing on every track means the steady-state priority is still wrong.
    mc::u32 boosts() const { return boosts_.load(std::memory_order_relaxed); }
    int ringLow() const { return ringLow_.load(std::memory_order_relaxed); }

private:
    static void threadEntry(void* self);
    void run();

    // Refills every free wave buffer, up to `maxBuffers` of them -- the cap is
    // what bounds a boosted pass against the frame it is interrupting.
    void fillBuffers(int maxBuffers);

    // How many buffers the DSP still has queued or is playing. Read off the ring
    // the hardware writes back into, so it is the real figure rather than a
    // count of what was handed over.
    int queuedBuffers() const;

    // Raise this thread above the main thread, or put it back. A no-op when the
    // kernel refused the probe in `run()` -- an exheader that grants no headroom
    // is a console that keeps the old behaviour, not a console that fails.
    void setBoosted(bool boosted);

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
    std::atomic<mc::u32> boosts_{0};
    std::atomic<int> ringLow_{kRingBuffers};

    // Decode thread only. `boostPriority_` is negative when the kernel would not
    // grant one: it is probed once, at the top of `run()`, because the only
    // honest way to find out what a process may ask for is to ask.
    // libctru's own `s32`, not mc::i32: it is what svcSetThreadPriority takes.
    s32 normalPriority_ = 0;
    s32 boostPriority_ = -1;
    bool boosted_ = false;

    // True once this track has had a full ring behind it. Until then an empty
    // ring is a track starting rather than a decoder losing, and neither
    // `ringLow_` nor `boosts_` should count it.
    bool primed_ = false;
};

}  // namespace mc::ctr
