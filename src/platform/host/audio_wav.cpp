#include "platform/host/audio_wav.hpp"

#include <cstring>

namespace mc::host {

namespace {

constexpr int kTicksPerSecond = 20;

void writeLE32(std::FILE* file, u32 value)
{
    const u8 bytes[4] = {u8(value), u8(value >> 8), u8(value >> 16), u8(value >> 24)};
    std::fwrite(bytes, 1, sizeof(bytes), file);
}

void writeLE16(std::FILE* file, u16 value)
{
    const u8 bytes[2] = {u8(value), u8(value >> 8)};
    std::fwrite(bytes, 1, sizeof(bytes), file);
}

}  // namespace

WavBackend::WavBackend(std::string path, int sampleRate)
    : path_(std::move(path)), sampleRate_(sampleRate)
{
    if (path_.empty()) {
        return;
    }
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr) {
        return;
    }
    // A placeholder header; the two length fields are rewritten by finish(),
    // because neither is known until the last frame is in.
    std::fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, file_);
    writeLE32(file_, 16);
    writeLE16(file_, 1);   // PCM
    writeLE16(file_, 2);   // stereo, always: mono sources are duplicated
    writeLE32(file_, u32(sampleRate_));
    writeLE32(file_, u32(sampleRate_) * 4);
    writeLE16(file_, 4);
    writeLE16(file_, 16);
    std::fwrite("data\0\0\0\0", 1, 8, file_);
}

WavBackend::~WavBackend()
{
    finish();
}

bool WavBackend::playMusic(std::unique_ptr<mc::audio::PcmSource> source, float gain)
{
    if (!source) {
        return false;
    }
    // The console defers this to its decode thread; the harness has none and
    // no frame to protect, so it opens the file here and reports the failure
    // straight away -- which is what makes a broken file visible in a dump
    // rather than silently absent.
    if (!source->prepare()) {
        return false;
    }
    gain_ = gain;
    source_ = std::move(source);
    ++tracksPlayed_;
    return true;
}

mc::audio::SampleId WavBackend::addSample(const mc::audio::Sample& sample)
{
    if (sample.empty()) {
        return mc::audio::kNoSample;
    }
    samples_.push_back(sample);
    return mc::audio::SampleId(samples_.size() - 1);
}

void WavBackend::playSample(mc::audio::SampleId id, float gain, float pitch)
{
    if (id < 0 || usize(id) >= samples_.size()) {
        return;
    }
    samplePlays_.push_back(SamplePlay{id, gain, pitch});
}

void WavBackend::stopMusic()
{
    source_.reset();
}

void WavBackend::advance(int ticks)
{
    if (ticks <= 0) {
        return;
    }

    const usize frames = usize(ticks) * usize(sampleRate_) / usize(kTicksPerSecond);
    if (source_ == nullptr) {
        // Silence still occupies time. Writing it is what makes the gaps in the
        // .wav the gaps the game would have, so the file is a recording of the
        // schedule and not just of the tracks.
        if (file_ != nullptr) {
            scratch_.assign(frames * 2, 0);
            writeFrames(scratch_.data(), frames);
        } else {
            framesWritten_ += frames;
        }
        return;
    }

    const usize channels = usize(source_->channels());
    scratch_.resize(frames * channels);
    usize produced = source_->read(scratch_.data(), frames);

    if (file_ != nullptr) {
        // Interleave up to stereo and apply the gain a1.1.2 applies to music,
        // which is the volume itself with no further factor.
        std::vector<i16> out(frames * 2, 0);
        for (usize i = 0; i < produced; ++i) {
            const float left = float(scratch_[i * channels]) * gain_;
            const float right =
                float(scratch_[i * channels + (channels == 2 ? 1 : 0)]) * gain_;
            out[i * 2] = i16(left);
            out[i * 2 + 1] = i16(right);
        }
        writeFrames(out.data(), frames);
    } else {
        framesWritten_ += frames;
    }

    if (produced < frames) {
        // The track ended inside this slice, which is exactly when a1.1.2's
        // counter is allowed to start running again.
        source_.reset();
    }
}

void WavBackend::writeFrames(const i16* frames, usize count)
{
    std::fwrite(frames, sizeof(i16) * 2, count, file_);
    framesWritten_ += count;
}

void WavBackend::finish()
{
    if (closed_) {
        return;
    }
    closed_ = true;
    if (file_ == nullptr) {
        return;
    }

    const u32 dataBytes = u32(framesWritten_ * 4);
    std::fseek(file_, 4, SEEK_SET);
    writeLE32(file_, 36 + dataBytes);
    std::fseek(file_, 40, SEEK_SET);
    writeLE32(file_, dataBytes);
    std::fclose(file_);
    file_ = nullptr;
}

}  // namespace mc::host
