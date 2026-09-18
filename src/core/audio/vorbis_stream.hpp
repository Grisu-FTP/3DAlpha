#pragma once

// Ogg Vorbis, decoded a packet at a time out of a file that is never fully
// read.
//
// a1.1.2's music is 44100 Hz stereo Vorbis, three to four minutes a track. Fully
// decoded that is around 46 MB per file, against 64 MB of system memory on an
// Old 3DS -- so this streams, and streaming is not an optimisation here, it is
// the only option. The file is read through `io::RandomAccessFile`, whose
// positional `readAt` is exactly the shape a Vorbis callback wants and which
// exists for the same reason: see core/io/file_system.hpp.
//
// **Two libraries, one implementation.** The 3DS links Tremor
// (`3ds-libvorbisidec`), Xiph's fixed-point decoder, because an ARM11 has no
// business doing this in floating point. The host links the reference
// `libvorbisfile`. Their APIs are identical except that Tremor's `ov_read`
// drops the endianness, word-size and signedness arguments -- it only ever
// produces host-endian signed 16-bit -- so one `#if` covers the difference.
//
// That `#if` is on **which library is present**, not on which game version is
// being built. CLAUDE.md bans the latter absolutely; this is the former, and it
// is the same kind of conditional as `#ifdef _WIN32`. It is called out here
// because a reviewer scanning for `#if` will find it and should not have to
// guess.
//
// When neither library is available `MC_HAVE_VORBIS` is 0, `open` always fails,
// and the game is silent -- the same degradation as a missing resources folder
// or a missing DSP firmware. Audio never breaks a build.

#include "core/audio/pcm_source.hpp"
#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace mc::audio {

// True when a decoder was linked in. The options screen says so, because
// "silent" and "silent because this build has no decoder" are different
// problems for whoever is holding the console.
bool vorbisAvailable();

// **`.mus` -- Mojang's own container, which is an Ogg Vorbis file behind a
// one-byte cipher.** Records are the only thing in a1.1.2 that uses it, and
// until this existed they were the one sound the port could not make.
//
// The whole of it is class `hk`, the `InputStream` that `ep` (the codec) wraps
// the file in. Its constructor takes the **file's own name** -- the part of the
// URL after the last slash, extension and all -- and seeds a 32-bit state with
// `String.hashCode()`; `read` then walks the bytes:
//
// ```
// byte b = buf[i] ^= (byte)(key >> 8);   // and b is the *decoded* byte
// key = key * 498729871 + 85731 * b;
// ```
//
// **The state advances on the plaintext**, so the keystream at byte n depends
// on every byte before it and there is no way to jump into the middle of the
// file. That is why a `.mus` is opened as an *unseekable* stream: Vorbis is
// perfectly happy decoding one forward-only, which is exactly what paulscode
// did with it over a socket-shaped `InputStream`.
//
// `musKey` is Java's `String.hashCode` -- `h = 31 * h + c`, wrapping -- over
// the file name.
i32 musKey(std::string_view fileName);

// One block of `.mus`, decoded in place, with `*key` carried on to the next.
void musDecode(u8* bytes, usize count, i32* key);

class VorbisStream final : public PcmSource {
public:
    // **Touches nothing.** It records the file to open and returns; the open,
    // the header parse and the format query all happen in `prepare()`, on the
    // decode thread. Null only when this build has no decoder at all, which the
    // caller can equally ask `vorbisAvailable()` about.
    //
    // The split exists because starting a track is main-thread work and reading
    // a card is not. See PcmSource::prepare.
    static std::unique_ptr<VorbisStream> create(io::FileSystem& fs, std::string path);

    // The same, for a `.mus`: every block read is deciphered before Vorbis sees
    // it, and the stream is opened unseekable because the cipher cannot be
    // entered halfway. See `musKey`.
    static std::unique_ptr<VorbisStream> createMus(io::FileSystem& fs, std::string path);

    // **The codec picked off the extension**, which is what paulscode does with
    // the URL it is handed: `.mus` goes through the cipher and everything else
    // does not. It matters because a `streaming/` folder can hold both -- a
    // beta-era one holds `13.mus` *and* `13.ogg`, and the pool keys them
    // identically -- so the file, not the caller, has to decide.
    static std::unique_ptr<VorbisStream> open(io::FileSystem& fs, std::string path);

    // True when this path is Mojang's container rather than a plain Ogg.
    static bool isMus(std::string_view path);

    // Opens the file and reads its headers -- a few kilobytes. False if it is
    // missing, is not Ogg Vorbis, or has more channels than ndsp can play.
    bool prepare() override;

    ~VorbisStream() override;

    usize read(i16* out, usize frames) override;
    int channels() const override { return channels_; }
    int sampleRate() const override { return sampleRate_; }
    bool finished() const override { return finished_; }

    // Total frames, or 0 when the stream is not seekable enough to say. Only
    // the harness uses it, to size a .wav header.
    u64 totalFrames() const { return totalFrames_; }

    VorbisStream();

    // The three Vorbis file callbacks, as members so they can see the handle.
    // Public only because the library takes plain function pointers and the
    // thunks that adapt to them are free functions; nothing else calls these.
    usize callbackRead(void* ptr, usize size, usize nmemb);
    int callbackSeek(i64 offset, int whence);
    u64 callbackTell() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
    io::FileSystem* fs_ = nullptr;
    std::string path_;
    std::unique_ptr<io::RandomAccessFile> file_;
    u64 fileSize_ = 0;
    u64 position_ = 0;
    // **The `.mus` cipher, or off.** `musKeySeed_` is the file name's hash and
    // `musKey_` is the state the next byte will be deciphered with -- which is
    // only ever advanced forwards, in `callbackRead`, because the stream is
    // opened unseekable.
    bool mus_ = false;
    i32 musKeySeed_ = 0;
    i32 musKey_ = 0;
    int channels_ = 0;
    int sampleRate_ = 0;
    u64 totalFrames_ = 0;
    bool finished_ = false;
};

}  // namespace mc::audio
