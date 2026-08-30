#include "platform/ctr/audio.hpp"

#include "core/util/worker.hpp"

#include <cstdio>
#include <cstring>

namespace mc::ctr {

namespace {

// libctru's ndsp thread calls this once per audio frame. It does exactly one
// thing, and the reason is in the header: decoding here would put Vorbis on the
// thread that feeds the DSP.
void ndspFrameCallback(void* context)
{
    LightEvent_Signal(static_cast<LightEvent*>(context));
}

}  // namespace

NdspBackend::~NdspBackend()
{
    shutdown();
}

void NdspBackend::init(bool enabled)
{
    if (!enabled) {
        // The options contract in docs/3ds-performance.md: ndsp is never
        // initialised and the mixing thread is never spawned. Not a quieter
        // version of the same work -- none of it happens.
        status_ = AudioStatus::Disabled;
        return;
    }

    if (R_FAILED(ndspInit())) {
        // Overwhelmingly this is a console whose owner has not dumped
        // dspfirm.cdc. It can also be the DSP being held by something else, and
        // telling that player to dump a firmware they already have is worse
        // than saying nothing -- so the two are distinguished by whether the
        // file is there, which is the only signal libctru gives us.
        FILE* firmware = std::fopen("sdmc:/3ds/dspfirm.cdc", "rb");
        if (firmware != nullptr) {
            std::fclose(firmware);
            status_ = AudioStatus::Unavailable;
        } else {
            status_ = AudioStatus::NoFirmware;
        }
        return;
    }

    // 32 KB, once, for the life of the process. linearAlloc because the DSP
    // reads it by physical address and cannot follow the MMU.
    ring_ = static_cast<i16*>(linearAlloc(kRingBytes));
    if (ring_ == nullptr) {
        ndspExit();
        status_ = AudioStatus::Unavailable;
        return;
    }
    std::memset(ring_, 0, kRingBytes);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);

    LightEvent_Init(&wake_, RESET_ONESHOT);
    LightLock_Init(&lock_);

    for (int i = 0; i < kRingBuffers; ++i) {
        buffers_[i] = ndspWaveBuf{};
        buffers_[i].data_pcm16 = ring_ + usize(i) * usize(kFramesPerBuffer) * 2;
        buffers_[i].nsamples = 0;
        buffers_[i].status = NDSP_WBUF_DONE;
    }

    running_.store(true, std::memory_order_release);

    // The ndsp callback signals the decode thread; it never decodes.
    ndspSetCallback(&ndspFrameCallback, &wake_);

    WorkerSpawn spawn = workerSpawn();
    if (spawn != nullptr) {
        thread_ = spawn(&NdspBackend::threadEntry, this, WorkerRole::Audio);
    }
    if (thread_ == nullptr) {
        // No thread means no decoding, and decoding on the main thread is not
        // an acceptable fallback for audio the way doing generation inline is.
        running_.store(false, std::memory_order_release);
        ndspSetCallback(nullptr, nullptr);
        linearFree(ring_);
        ring_ = nullptr;
        ndspExit();
        status_ = AudioStatus::Unavailable;
        return;
    }

    status_ = AudioStatus::Ready;
}

void NdspBackend::shutdown()
{
    if (status_ != AudioStatus::Ready) {
        return;
    }

    stopMusic();
    running_.store(false, std::memory_order_release);
    LightEvent_Signal(&wake_);

    if (thread_ != nullptr) {
        if (WorkerJoin join = workerJoin()) {
            join(thread_);
        }
        thread_ = nullptr;
    }

    ndspSetCallback(nullptr, nullptr);
    ndspChnWaveBufClear(kMusicChannel);
    ndspExit();

    if (ring_ != nullptr) {
        linearFree(ring_);
        ring_ = nullptr;
    }
    status_ = AudioStatus::Disabled;
}

bool NdspBackend::playMusic(std::unique_ptr<audio::PcmSource> source, float gain)
{
    if (status_ != AudioStatus::Ready || !source) {
        return false;
    }

    // The source is not opened, queried or validated here: all three are card
    // and decoder work, and this runs on the frame loop. `prepare()` does them
    // on the decode thread, and a file that turns out to be unplayable is
    // dropped there. So this cannot fail for a bad file, only for a backend
    // that is not up -- which is why "true" here means "accepted", not
    // "playing".
    gain_.store(gain, std::memory_order_relaxed);

    LightLock_Lock(&lock_);
    pending_ = std::move(source);
    // Everything already queued belongs to the track being replaced.
    generation_.fetch_add(1, std::memory_order_acq_rel);
    LightLock_Unlock(&lock_);

    playing_.store(true, std::memory_order_release);
    LightEvent_Signal(&wake_);
    return true;
}

void NdspBackend::stopMusic()
{
    if (status_ != AudioStatus::Ready) {
        return;
    }

    LightLock_Lock(&lock_);
    pending_.reset();
    generation_.fetch_add(1, std::memory_order_acq_rel);
    LightLock_Unlock(&lock_);

    playing_.store(false, std::memory_order_release);
    ndspChnWaveBufClear(kMusicChannel);
    LightEvent_Signal(&wake_);
}

bool NdspBackend::musicPlaying() const
{
    // `SoundSystem.playing("BgMusic")`. Deliberately true until the hardware
    // has drained, not until the decoder reached the end of the file: the music
    // counter must not start running again over the last three seconds of a
    // track.
    return playing_.load(std::memory_order_acquire);
}

void NdspBackend::setMusicGain(float gain)
{
    gain_.store(gain, std::memory_order_relaxed);
    if (status_ == AudioStatus::Ready) {
        applyGain();
    }
}

void NdspBackend::applyGain()
{
    const float gain = gain_.load(std::memory_order_relaxed);

    // ndsp's mix is twelve floats: front left, front right, then back and the
    // aux sends. Music is not positional -- a1.1.2 plays it with attenuation
    // disabled -- so both fronts get the same value and everything else stays
    // at zero.
    float mix[12] = {};
    mix[0] = gain;
    mix[1] = gain;
    ndspChnSetMix(kMusicChannel, mix);
}

void NdspBackend::update()
{
    // Nothing, and deliberately nothing.
    //
    // The obvious implementation is for the main thread to notice here that the
    // track has drained and clear `playing_`. That would mean the main thread
    // reading `ndspWaveBuf::status` out of the ring while the decode thread
    // writes the same structures -- a real data race for a fact the decode
    // thread already knows. So the drain is detected there instead (see `run`)
    // and this stays a method the seam needs rather than work the frame does.
    //
    // It is not removed from the interface: a backend that mixes on the main
    // thread would need it, and a per-frame hook that costs a call is cheaper
    // than discovering later that there is nowhere to put one.
}

void NdspBackend::threadEntry(void* self)
{
    static_cast<NdspBackend*>(self)->run();
}

void NdspBackend::run()
{
    u32 generation = generation_.load(std::memory_order_acquire);

    while (running_.load(std::memory_order_acquire)) {
        // One wake, one pass over the ring, then back to sleep. Bounded work
        // per wake is what keeps this off the frame on an Old 3DS.
        LightEvent_Wait(&wake_);
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        // Pick up a track handed over by the main thread. The swap happens
        // under the lock; the decoding does not.
        LightLock_Lock(&lock_);
        const u32 current = generation_.load(std::memory_order_acquire);
        if (current != generation) {
            generation = current;
            source_ = std::move(pending_);
            pending_.reset();

            ndspChnWaveBufClear(kMusicChannel);
            for (ndspWaveBuf& buffer : buffers_) {
                buffer.status = NDSP_WBUF_DONE;
                buffer.nsamples = 0;
            }

            // The open, the header parse and the format query, all here on the
            // decode thread rather than on the frame that asked for the track.
            if (source_ && !source_->prepare()) {
                source_.reset();
                playing_.store(false, std::memory_order_release);
            }

            if (source_) {
                ndspChnReset(kMusicChannel);
                ndspChnSetInterp(kMusicChannel, NDSP_INTERP_LINEAR);
                ndspChnSetRate(kMusicChannel, float(source_->sampleRate()));
                ndspChnSetFormat(kMusicChannel,
                                 source_->channels() == 2
                                     ? NDSP_FORMAT_STEREO_PCM16
                                     : NDSP_FORMAT_MONO_PCM16);
                applyGain();
            }
        }
        LightLock_Unlock(&lock_);

        fillBuffers();

        // The track has drained when the decoder is finished *and* the hardware
        // has run out. Both halves matter: clearing `playing_` when the decoder
        // reaches the end of the file would let the music counter start running
        // over the last three seconds of every track, which is a1.1.2 playing
        // music slightly too often for a reason nobody would ever find.
        //
        // This lives on the decode thread because the decode thread is the one
        // that owns the ring; see `update()`.
        if (!source_ && playing_.load(std::memory_order_acquire) &&
            !ndspChnIsPlaying(kMusicChannel)) {
            playing_.store(false, std::memory_order_release);
        }
    }

    source_.reset();
}

void NdspBackend::fillBuffers()
{
    if (!source_) {
        return;
    }

    const int channels = source_->channels();
    const u64 before = svcGetSystemTick();
    bool decoded = false;

    for (int i = 0; i < kRingBuffers; ++i) {
        ndspWaveBuf& buffer = buffers_[i];
        if (buffer.status != NDSP_WBUF_DONE && buffer.status != NDSP_WBUF_FREE) {
            continue;
        }

        i16* out = ring_ + usize(i) * usize(kFramesPerBuffer) * 2;
        const usize frames = source_->read(out, usize(kFramesPerBuffer));
        if (frames == 0) {
            // End of the track. The channel keeps playing whatever is still
            // queued; `update()` on the main thread notices when it drains and
            // only then lets the music counter start again.
            source_.reset();
            break;
        }
        decoded = true;

        buffer.nsamples = u32(frames);

        // The DSP reads this by physical address and does not see the data
        // cache, so the bytes just written have to be pushed out -- and exactly
        // those bytes, immediately before handing them over.
        DSP_FlushDataCache(out, frames * usize(channels) * sizeof(i16));
        ndspChnWaveBufAdd(kMusicChannel, &buffer);
    }

    if (decoded) {
        const u64 elapsed = svcGetSystemTick() - before;
        decodeMicros_.store(u32(elapsed / (SYSCLOCK_ARM11 / 1000000)),
                            std::memory_order_relaxed);
    } else if (source_ && !ndspChnIsPlaying(kMusicChannel)) {
        // Nothing was free and the channel has nothing to play: the decoder
        // fell behind far enough to be audible. This is the number that says
        // whether the thread policy is right on this console, so it is counted
        // rather than guessed at.
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace mc::ctr
