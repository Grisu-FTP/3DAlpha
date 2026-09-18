#include "core/audio/vorbis_stream.hpp"

#include <cstring>

#if MC_HAVE_VORBIS
#if MC_VORBIS_TREMOR
#include <tremor/ivorbisfile.h>
#else
#include <vorbis/vorbisfile.h>
#endif
#endif

namespace mc::audio {

bool vorbisAvailable()
{
#if MC_HAVE_VORBIS
    return true;
#else
    return false;
#endif
}

namespace {

// The two constants out of `hk.read(byte[], int, int)`, as `u32` so the
// multiply wraps rather than being undefined. See the header.
constexpr u32 kMusMultiplier = 498729871u;
constexpr u32 kMusAddend = 85731u;

// The part of a path after the last separator -- `url.getPath()` then
// `substring(lastIndexOf("/") + 1)`. The extension stays on: `hk` hashes
// "13.mus", not "13".
std::string_view fileNameOf(std::string_view path)
{
    const usize slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool VorbisStream::isMus(std::string_view path)
{
    const std::string_view name = fileNameOf(path);
    if (name.size() < 4) {
        return false;
    }
    const std::string_view tail = name.substr(name.size() - 4);
    return (tail[0] == '.') && (tail[1] == 'm' || tail[1] == 'M')
           && (tail[2] == 'u' || tail[2] == 'U') && (tail[3] == 's' || tail[3] == 'S');
}

i32 musKey(std::string_view fileName)
{
    // `String.hashCode()` over UTF-16 units. Every name a resources folder
    // holds is ASCII, where a UTF-8 byte and a UTF-16 unit are the same number;
    // a name that is not would hash differently in Java, and there is no such
    // file to be faithful to.
    u32 hash = 0;
    for (const char c : fileNameOf(fileName)) {
        hash = hash * 31u + u32(u8(c));
    }
    return i32(hash);
}

void musDecode(u8* bytes, usize count, i32* key)
{
    u32 state = u32(*key);
    for (usize i = 0; i < count; ++i) {
        // `buf[i] ^= (byte)(key >> 8)` -- the low eight bits of an *arithmetic*
        // shift, which for a XOR of one byte is the same either way.
        const u8 plain = u8(bytes[i] ^ u8(state >> 8));
        bytes[i] = plain;
        // `key = key * 498729871 + 85731 * b`, and `b` is the decoded byte
        // **sign-extended**: `i2b` leaves a signed byte on the stack and the
        // multiply is an int one. Widening it as unsigned is a different
        // keystream from the second byte over 0x7F onwards.
        state = state * kMusMultiplier + kMusAddend * u32(i32(i8(plain)));
    }
    *key = i32(state);
}

#if MC_HAVE_VORBIS

struct VorbisStream::Impl {
    OggVorbis_File vf{};
    bool open = false;
};

namespace {

// The three callbacks Vorbis needs, over `io::RandomAccessFile`. `readAt` is
// exact -- a short read is a failure, not a partial success -- so every read is
// clamped to the file's length first. Vorbis asks for a fixed block at the end
// of a file as a matter of course, and treating that as an error would truncate
// the last second of every track.
size_t readFunc(void* ptr, size_t size, size_t nmemb, void* datasource)
{
    VorbisStream* self = static_cast<VorbisStream*>(datasource);
    return self->callbackRead(ptr, size, nmemb);
}

int seekFunc(void* datasource, ogg_int64_t offset, int whence)
{
    VorbisStream* self = static_cast<VorbisStream*>(datasource);
    return self->callbackSeek(i64(offset), whence);
}

long tellFunc(void* datasource)
{
    VorbisStream* self = static_cast<VorbisStream*>(datasource);
    return long(self->callbackTell());
}

// Never closes: the handle is owned by the VorbisStream and outlives the
// decoder, so `ov_clear` must not take it down.
int closeFunc(void*)
{
    return 0;
}

}  // namespace

std::unique_ptr<VorbisStream> VorbisStream::create(io::FileSystem& fs, std::string path)
{
    std::unique_ptr<VorbisStream> stream(new VorbisStream());
    stream->fs_ = &fs;
    stream->path_ = std::move(path);
    return stream;
}

std::unique_ptr<VorbisStream> VorbisStream::createMus(io::FileSystem& fs, std::string path)
{
    std::unique_ptr<VorbisStream> stream = create(fs, std::move(path));
    if (stream) {
        stream->mus_ = true;
        stream->musKeySeed_ = musKey(stream->path_);
        stream->musKey_ = stream->musKeySeed_;
    }
    return stream;
}

std::unique_ptr<VorbisStream> VorbisStream::open(io::FileSystem& fs, std::string path)
{
    const bool cipher = isMus(path);
    return cipher ? createMus(fs, std::move(path)) : create(fs, std::move(path));
}

bool VorbisStream::prepare()
{
    if (impl_ && impl_->open) {
        return true;
    }
    if (fs_ == nullptr) {
        return false;
    }

    std::unique_ptr<io::RandomAccessFile> file = fs_->openRandomAccess(path_.c_str(), false);
    if (!file) {
        return false;
    }

    u64 size = 0;
    if (!file->size(&size) || size == 0) {
        return false;
    }

    file_ = std::move(file);
    fileSize_ = size;
    position_ = 0;
    impl_.reset(new Impl());

    musKey_ = musKeySeed_;

    ov_callbacks callbacks{};
    callbacks.read_func = &readFunc;
    // **A `.mus` is unseekable and says so.** Its keystream advances on the
    // plaintext, so byte n cannot be deciphered without every byte before it;
    // handing Vorbis a seek it could use would let it jump and read noise.
    // Vorbisfile treats a null `seek_func` as "this is a pipe" and decodes
    // forward only, which is how paulscode played these in the first place.
    callbacks.seek_func = mus_ ? nullptr : &seekFunc;
    callbacks.close_func = &closeFunc;
    callbacks.tell_func = mus_ ? nullptr : &tellFunc;

    if (ov_open_callbacks(this, &impl_->vf, nullptr, 0, callbacks) < 0) {
        return false;
    }
    impl_->open = true;

    vorbis_info* info = ov_info(&impl_->vf, -1);
    if (info == nullptr || info->channels < 1 || info->channels > 2 || info->rate <= 0) {
        // Mono and stereo are what the original's codec produced and what ndsp
        // can play without us writing a downmix. More than two channels in a
        // Minecraft resource folder is a file that does not belong there.
        return false;
    }
    channels_ = info->channels;
    sampleRate_ = int(info->rate);

    const ogg_int64_t total = ov_pcm_total(&impl_->vf, -1);
    totalFrames_ = total > 0 ? u64(total) : 0;
    return true;
}

VorbisStream::~VorbisStream()
{
    if (impl_ && impl_->open) {
        ov_clear(&impl_->vf);
    }
}

usize VorbisStream::read(i16* out, usize frames)
{
    if (finished_ || frames == 0 || !impl_ || !impl_->open) {
        return 0;
    }

    const usize wanted = frames * usize(channels_) * sizeof(i16);
    char* buffer = reinterpret_cast<char*>(out);
    usize filled = 0;

    while (filled < wanted) {
        int bitstream = 0;
        const int chunk = int(wanted - filled);
#if MC_VORBIS_TREMOR
        const long got = ov_read(&impl_->vf, buffer + filled, chunk, &bitstream);
#else
        // 0 = little-endian, 2 = 16-bit words, 1 = signed. Tremor produces
        // exactly this and has no way to be asked for anything else, which is
        // why the two branches agree on format.
        const long got = ov_read(&impl_->vf, buffer + filled, chunk, 0, 2, 1, &bitstream);
#endif
        if (got == 0) {
            finished_ = true;
            break;
        }
        if (got < 0) {
            // OV_HOLE is a gap in the stream and is recoverable -- a file
            // copied off a card may well have one. Anything else is not, and
            // ending the track is better than looping on an error forever.
            if (got == OV_HOLE) {
                continue;
            }
            finished_ = true;
            break;
        }
        filled += usize(got);
    }

    // A partial frame cannot happen -- ov_read returns whole frames -- but the
    // division is where it would show up, so it is written to truncate rather
    // than to trust.
    return filled / (usize(channels_) * sizeof(i16));
}

usize VorbisStream::callbackRead(void* ptr, usize size, usize nmemb)
{
    if (size == 0 || nmemb == 0) {
        return 0;
    }
    usize wanted = size * nmemb;
    if (position_ >= fileSize_) {
        return 0;
    }
    if (position_ + wanted > fileSize_) {
        wanted = usize(fileSize_ - position_);
    }
    // Whole elements only, as fread would.
    wanted = (wanted / size) * size;
    if (wanted == 0) {
        return 0;
    }

    if (!file_->readAt(position_, ByteSpan(static_cast<u8*>(ptr), wanted))) {
        return 0;
    }
    if (mus_) {
        // Forward-only, and the state carries across calls: the stream is
        // opened unseekable precisely so that this is always the next block.
        musDecode(static_cast<u8*>(ptr), wanted, &musKey_);
    }
    position_ += wanted;
    return wanted / size;
}

int VorbisStream::callbackSeek(i64 offset, int whence)
{
    if (mus_) {
        // Never reached -- `prepare` hands Vorbis a null seek for a `.mus` --
        // but a seek that silently succeeded here would decipher the rest of
        // the file against the wrong keystream, so it refuses out loud.
        return -1;
    }
    i64 target = 0;
    switch (whence) {
        case 0: target = offset; break;                       // SEEK_SET
        case 1: target = i64(position_) + offset; break;       // SEEK_CUR
        case 2: target = i64(fileSize_) + offset; break;       // SEEK_END
        default: return -1;
    }
    if (target < 0 || u64(target) > fileSize_) {
        return -1;
    }
    position_ = u64(target);
    return 0;
}

u64 VorbisStream::callbackTell() const
{
    return position_;
}

#else  // !MC_HAVE_VORBIS

// No decoder in this build. Every entry point fails in the one way the rest of
// the audio layer already handles: nothing opens, so nothing plays.
struct VorbisStream::Impl {};

std::unique_ptr<VorbisStream> VorbisStream::create(io::FileSystem&, std::string)
{
    return nullptr;
}

bool VorbisStream::prepare()
{
    return false;
}

VorbisStream::~VorbisStream() = default;

usize VorbisStream::read(i16*, usize)
{
    return 0;
}

usize VorbisStream::callbackRead(void*, usize, usize)
{
    return 0;
}

int VorbisStream::callbackSeek(i64, int)
{
    return -1;
}

u64 VorbisStream::callbackTell() const
{
    return 0;
}

#endif

VorbisStream::VorbisStream() = default;

}  // namespace mc::audio
