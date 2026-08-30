#include "core/audio/sample.hpp"

#include "core/audio/vorbis_stream.hpp"

#include <memory>

namespace mc::audio {

bool decodeSample(io::FileSystem& fs, const std::string& path, Sample* out)
{
    if (out == nullptr) {
        return false;
    }

    // The same decoder the music uses, driven to the end instead of a block at
    // a time. Reusing it is the point: there is one place that knows what Ogg
    // Vorbis is, and a build without a decoder fails here exactly as it fails
    // for music -- `create` returns null and the caller is silent.
    std::unique_ptr<VorbisStream> stream = VorbisStream::create(fs, path);
    if (!stream || !stream->prepare()) {
        return false;
    }

    const int channels = stream->channels();
    const int sampleRate = stream->sampleRate();
    if (channels < 1 || channels > 2 || sampleRate <= 0) {
        return false;
    }

    // `totalFrames` comes off the Ogg page headers and costs a seek, so the
    // buffer is sized once when it is known. It is 0 for a stream that could
    // not be measured, and the read loop below grows in blocks in that case
    // rather than refusing a file the decoder is otherwise happy with.
    const u64 announced = stream->totalFrames();
    if (announced > kMaxSampleFrames) {
        return false;
    }

    constexpr usize kBlockFrames = 4096;

    Sample sample;
    sample.channels = channels;
    sample.sampleRate = sampleRate;
    if (announced > 0) {
        sample.pcm.reserve(usize(announced) * usize(channels));
    }

    usize frames = 0;
    for (;;) {
        sample.pcm.resize((frames + kBlockFrames) * usize(channels));
        const usize got = stream->read(sample.pcm.data() + frames * usize(channels),
                                       kBlockFrames);
        frames += got;
        if (got == 0) {
            break;
        }
        if (frames > kMaxSampleFrames) {
            // A file that lied about its length, or one that could not be
            // measured at all. Refused for the same reason the announced
            // length is: see the header.
            return false;
        }
    }

    if (frames == 0) {
        return false;
    }

    sample.pcm.resize(frames * usize(channels));
    sample.pcm.shrink_to_fit();
    *out = std::move(sample);
    return true;
}

}  // namespace mc::audio
