#include "core/util/compress.hpp"

#include <zlib.h>

namespace mc::zip {

namespace {

// zlib selects the framing through the windowBits argument: the base window
// size, negated for raw deflate, or +16 for a gzip wrapper.
int windowBits(Wrapper wrapper)
{
    switch (wrapper) {
        case Wrapper::Gzip: return MAX_WBITS + 16;
        case Wrapper::Zlib: return MAX_WBITS;
        case Wrapper::Raw: return -MAX_WBITS;
    }
    return MAX_WBITS;
}

// Decompressed output is written in blocks of this size. Large enough that a
// typical 80 KB chunk column finishes in a handful of iterations, small enough
// that a truncated stream does not commit megabytes before failing.
constexpr usize kOutputBlock = 64 * 1024;

}  // namespace

bool decompress(ConstByteSpan in, std::vector<u8>& out, Wrapper wrapper, usize maxOutput)
{
    if (in.empty()) {
        return false;
    }

    z_stream stream = {};
    if (inflateInit2(&stream, windowBits(wrapper)) != Z_OK) {
        return false;
    }

    stream.next_in = const_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());

    const usize startSize = out.size();
    int status = Z_OK;

    while (status != Z_STREAM_END) {
        if (out.size() - startSize >= maxOutput) {
            inflateEnd(&stream);
            out.resize(startSize);
            return false;
        }

        const usize before = out.size();
        const usize block = kOutputBlock;
        out.resize(before + block);

        stream.next_out = out.data() + before;
        stream.avail_out = static_cast<uInt>(block);

        status = inflate(&stream, Z_NO_FLUSH);
        out.resize(out.size() - stream.avail_out);

        if (status == Z_OK || status == Z_STREAM_END) {
            continue;
        }
        // Z_BUF_ERROR with no input left means the stream was truncated; any
        // other code means it was corrupt. Both are unusable.
        inflateEnd(&stream);
        out.resize(startSize);
        return false;
    }

    inflateEnd(&stream);
    return true;
}

bool compress(ConstByteSpan in, std::vector<u8>& out, Wrapper wrapper, int level)
{
    z_stream stream = {};
    if (deflateInit2(&stream, level, Z_DEFLATED, windowBits(wrapper), 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        return false;
    }

    stream.next_in = const_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());

    const usize startSize = out.size();
    out.resize(startSize + deflateBound(&stream, static_cast<uLong>(in.size())));

    stream.next_out = out.data() + startSize;
    stream.avail_out = static_cast<uInt>(out.size() - startSize);

    // deflateBound guarantees one pass is enough, so anything short of
    // Z_STREAM_END here is a real failure rather than a full output buffer.
    const int status = deflate(&stream, Z_FINISH);
    const usize produced = (out.size() - startSize) - stream.avail_out;
    deflateEnd(&stream);

    if (status != Z_STREAM_END) {
        out.resize(startSize);
        return false;
    }
    out.resize(startSize + produced);
    return true;
}

}  // namespace mc::zip
