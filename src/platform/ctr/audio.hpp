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
// Everything the DSP reads lives in linear memory and is flushed out of the
// data cache immediately before it is handed over. Both are allocated once, at
// init, so nothing here allocates on a frame.

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

    // The music channel. Channel 0 of 24; the rest stay free for the effects
    // and records that arrive with their first emitter.
    static constexpr int kMusicChannel = 0;

    AudioStatus status_ = AudioStatus::Disabled;

    void* thread_ = nullptr;
    LightEvent wake_{};
    LightLock lock_{};

    ndspWaveBuf buffers_[kRingBuffers]{};
    mc::i16* ring_ = nullptr;

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
