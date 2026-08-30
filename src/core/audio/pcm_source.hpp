#pragma once

// "Give me the next N frames." The one thing the platform's mixer needs from
// anything that makes sound, and the reason ndsp never learns what Vorbis is.
//
// A frame is one sample per channel. `read` fills interleaved signed 16-bit
// samples -- the only format ndsp and every host backend agree on without
// conversion -- and returns how many frames it actually produced, which is less
// than asked for exactly once, at the end of the stream.
//
// **`read` is called on the decode thread, never on the main thread.** That is
// the whole point of the seam: a Vorbis packet takes a few hundred microseconds
// to unpack and a frame has 16.7 ms in it, so decoding where the renderer runs
// would be visible. Implementations must therefore be safe to construct on one
// thread and read from another, and must not touch anything the main thread
// owns.
//
// There is deliberately no seek. Music plays start to finish and stops; a1.1.2
// never scrubs, and a `seek` on this interface would be an unused virtual that
// every implementation has to answer for.

#include "core/util/types.hpp"

namespace mc::audio {

class PcmSource {
public:
    virtual ~PcmSource() = default;

    PcmSource(const PcmSource&) = delete;
    PcmSource& operator=(const PcmSource&) = delete;

    // Opens whatever the source needs opening -- a file, a decoder, a header.
    // **Called once, on the decode thread, before anything else.** It exists so
    // that constructing a source is free and the card read that a real one
    // needs does not land on the main thread: starting a track happens a couple
    // of times an hour, but a few milliseconds of SD latency is still a dropped
    // frame, and CONTRIBUTING is explicit that neither filesystem access nor
    // decompression belongs on core 0.
    //
    // False means the source is unusable and should be dropped. `channels` and
    // `sampleRate` are only meaningful after this has returned true.
    virtual bool prepare() { return true; }

    // Interleaved s16. Returns frames written; 0 means the stream has ended.
    // A short read that is not zero is legal and ordinary -- Vorbis hands back
    // one packet at a time and a packet is not a round number of frames.
    virtual usize read(i16* out, usize frames) = 0;

    // 1 or 2. a1.1.2's music is 44100 Hz stereo; its sound effects are mono.
    virtual int channels() const = 0;
    virtual int sampleRate() const = 0;

    // True once `read` has returned 0. Kept separate from `read` so the mixer
    // can tell "the decoder is behind" from "the track is over" without having
    // to remember the last return value.
    virtual bool finished() const = 0;

protected:
    PcmSource() = default;
};

}  // namespace mc::audio
