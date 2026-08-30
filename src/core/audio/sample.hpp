#pragma once

// A sound effect, decoded once and held whole in memory -- the other half of
// `PcmSource`, and deliberately not the same thing.
//
// Music streams because a track is three minutes of 44.1 kHz stereo, around
// 46 MB decoded, against 64 MB of system memory on an Old 3DS. An effect is the
// opposite case in every dimension: `random/click.ogg` is a third of a second
// of audio, under 50 KB decoded, and it has to be audible on the frame the
// player pressed the button rather than a decode-thread wake later. So it is
// decoded at boot and afterwards playing it is a pointer and a length.
//
// **That split is what keeps CONTRIBUTING's rule intact.** No allocation, no
// filesystem access and no decompression in the per-frame path: the card read
// and the Vorbis decode happen once, where a menu is being built, and the frame
// that clicks does neither. A lazily-decoded effect would put both on the frame
// that asked for it, which is exactly the stall the rule exists to prevent.
//
// a1.1.2 needs none of this because paulscode owns the file and the thread; it
// hands the library a URL and the library goes away and reads it. We cannot, so
// the same behaviour is reached by loading earlier rather than by blocking
// later. Nothing about which sound plays, or how loud, differs -- see
// docs/audio-a1.1.2.md.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string>
#include <vector>

namespace mc::audio {

// Interleaved signed 16-bit, the one format ndsp and every host backend agree
// on -- the same contract `PcmSource::read` fills, just all at once.
struct Sample {
    std::vector<i16> pcm;
    int channels = 1;
    int sampleRate = 44100;

    usize frames() const { return channels > 0 ? pcm.size() / usize(channels) : 0; }
    bool empty() const { return pcm.empty(); }
};

// A backend's handle for a sample it has taken. Opaque on purpose: on a console
// it indexes linear memory the DSP can reach, and nothing above the seam may
// assume that or anything else about it.
using SampleId = i32;
inline constexpr SampleId kNoSample = -1;

// Four seconds at 44.1 kHz. Every effect a1.1.2 ships is well under a second;
// the ceiling is here so that a player who drops a music track into `sound/`
// gets a refusal rather than a megabyte of linear memory silently spent.
inline constexpr usize kMaxSampleFrames = 44100 * 4;

// Decodes `path` in full. False when the file is missing, is not something this
// build can decode, or is longer than `kMaxSampleFrames` -- **too long is
// refused rather than truncated**, because a sound that cuts off halfway is a
// bug that sounds like a design decision.
//
// Opens the card and runs the decoder, so it belongs at boot and nowhere near a
// frame. `out` is left untouched on failure.
bool decodeSample(io::FileSystem& fs, const std::string& path, Sample* out);

}  // namespace mc::audio
