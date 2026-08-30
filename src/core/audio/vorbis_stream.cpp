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

    ov_callbacks callbacks{};
    callbacks.read_func = &readFunc;
    callbacks.seek_func = &seekFunc;
    callbacks.close_func = &closeFunc;
    callbacks.tell_func = &tellFunc;

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
    position_ += wanted;
    return wanted / size;
}

int VorbisStream::callbackSeek(i64 offset, int whence)
{
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
