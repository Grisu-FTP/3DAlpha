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
    for (int i = 0; i < kEffectVoices; ++i) {
        ndspChnWaveBufClear(kFirstEffectChannel + i);
    }
    ndspExit();

    if (ring_ != nullptr) {
        linearFree(ring_);
        ring_ = nullptr;
    }
    // After ndspExit, so nothing the DSP might still be reading is handed back
    // to the allocator first.
    for (int i = 0; i < sampleCount_; ++i) {
        if (samples_[i].data != nullptr) {
            linearFree(samples_[i].data);
            samples_[i] = LoadedSample{};
        }
    }
    sampleCount_ = 0;
    nextVoice_ = 0;
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

audio::SampleId NdspBackend::addSample(const audio::Sample& sample)
{
    // Not an error and never reported as one: a console with no firmware takes
    // no samples, exactly as it plays no music. See audio::Backend::addSample.
    if (status_ != AudioStatus::Ready || sampleCount_ >= kMaxSamples) {
        return audio::kNoSample;
    }
    const usize frames = sample.frames();
    if (frames == 0 || sample.channels < 1 || sample.channels > 2
        || sample.sampleRate <= 0) {
        return audio::kNoSample;
    }

    const usize bytes = frames * usize(sample.channels) * sizeof(i16);
    i16* data = static_cast<i16*>(linearAlloc(bytes));
    if (data == nullptr) {
        return audio::kNoSample;
    }
    std::memcpy(data, sample.pcm.data(), bytes);

    // Once, here, and never again: the bytes are const for the life of the
    // process, so unlike the music ring there is nothing to re-flush per play.
    DSP_FlushDataCache(data, bytes);

    const audio::SampleId id = audio::SampleId(sampleCount_);
    samples_[sampleCount_] = LoadedSample{data, u32(frames), sample.channels,
                                          sample.sampleRate};
    ++sampleCount_;
    return id;
}

void NdspBackend::playSample(audio::SampleId id, float gain, float pitch)
{
    if (status_ != AudioStatus::Ready || id < 0 || id >= audio::SampleId(sampleCount_)) {
        return;
    }
    const LoadedSample& sample = samples_[id];
    if (sample.data == nullptr) {
        return;
    }

    // Round-robin, and it steals. a1.1.2 does the same thing with its 256
    // rotating source names -- `"sound_" + (id++ % 256)` -- and never looks to
    // see whether the one it is about to reuse is still playing. Waiting for a
    // free voice instead would mean a click that sometimes does not happen,
    // which is worse than one that cuts another off.
    const int voice = nextVoice_;
    nextVoice_ = (nextVoice_ + 1) % kEffectVoices;
    const int channel = kFirstEffectChannel + voice;

    // Detach before touching the wave buffer: the DSP may still be reading the
    // structure from a previous play, and reusing it under the hardware is the
    // one way this can produce noise rather than sound.
    ndspChnWaveBufClear(channel);
    ndspChnReset(channel);
    ndspChnSetInterp(channel, NDSP_INTERP_LINEAR);

    // Pitch is a playback-rate multiplier, which is what the original's
    // `setPitch` is too -- paulscode resamples rather than shifting. ndsp mixes
    // at ~32,728 Hz and does the conversion in hardware; see the note in
    // docs/audio-a1.1.2.md about why there is no resampler of ours here.
    const float rate = float(sample.sampleRate) * (pitch > 0.0f ? pitch : 1.0f);
    ndspChnSetRate(channel, rate);
    ndspChnSetFormat(channel, sample.channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                                                   : NDSP_FORMAT_MONO_PCM16);

    const float level = gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain);
    float mix[12] = {};
    mix[0] = level;
    mix[1] = level;
    ndspChnSetMix(channel, mix);

    ndspWaveBuf& buffer = voices_[voice];
    buffer = ndspWaveBuf{};
    buffer.data_pcm16 = sample.data;
    buffer.nsamples = sample.frames;
    buffer.looping = false;
    buffer.status = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(channel, &buffer);
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
