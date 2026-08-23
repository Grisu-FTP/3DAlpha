#include "core/texture/png.hpp"

#include "core/util/compress.hpp"

#include <cstring>

namespace mc::texture {

namespace {

constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

u32 readBe32(const u8* p)
{
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

// Channels per pixel for the four colour types this decodes. Palette is one
// index per pixel; the palette itself supplies the rest.
int channelsFor(u8 colourType)
{
    switch (colourType) {
        case 0: return 1;  // greyscale
        case 2: return 3;  // truecolour
        case 3: return 1;  // palette index
        case 4: return 2;  // greyscale + alpha
        case 6: return 4;  // truecolour + alpha
    }
    return 0;
}

// PNG's Paeth predictor, verbatim from the specification. `a` is the byte to
// the left, `b` the one above, `c` the one above-left.
u8 paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return u8(a);
    }
    return pb <= pc ? u8(b) : u8(c);
}

// Undoes the five scanline filters in place, one row at a time.
//
// Filters reference the *reconstructed* bytes of the row above and of this row,
// so this has to run front to back and cannot be vectorised across rows. `bpp`
// is the byte distance to the pixel on the left, which for the 8-bit depths
// here is simply the channel count.
bool unfilter(u8* data, int width, int height, int channels)
{
    const usize stride = usize(width) * usize(channels);
    // The filtered form carries one extra leading byte per row, so the rows are
    // walked in the source buffer and written compacted into the same buffer.
    // Destination always trails source, so the overlap is safe.
    u8* dst = data;
    const u8* src = data;
    const u8* prev = nullptr;

    for (int y = 0; y < height; ++y) {
        const u8 filter = *src++;
        if (filter > 4) {
            return false;
        }
        for (usize i = 0; i < stride; ++i) {
            const int x = i >= usize(channels) ? dst[i - usize(channels)] : 0;
            const int b = prev != nullptr ? prev[i] : 0;
            const int c = (prev != nullptr && i >= usize(channels))
                              ? prev[i - usize(channels)]
                              : 0;
            const int raw = src[i];
            switch (filter) {
                case 0: dst[i] = u8(raw); break;
                case 1: dst[i] = u8(raw + x); break;
                case 2: dst[i] = u8(raw + b); break;
                case 3: dst[i] = u8(raw + ((x + b) >> 1)); break;
                default: dst[i] = u8(raw + paeth(x, b, c)); break;
            }
        }
        prev = dst;
        dst += stride;
        src += stride;
    }
    return true;
}

}  // namespace

const char* pngErrorText(PngError error)
{
    switch (error) {
        case PngError::Ok: return "ok";
        case PngError::NotPng: return "not a PNG";
        case PngError::Truncated: return "the file is truncated";
        case PngError::BadHeader: return "the image header is unusable";
        case PngError::UnsupportedDepth: return "only 8 bits per sample are supported";
        case PngError::UnsupportedColour: return "unsupported colour type";
        case PngError::UnsupportedInterlace: return "interlaced PNGs are not supported";
        case PngError::UnsupportedCompression: return "unknown compression or filter method";
        case PngError::MissingPalette: return "a palette image with no palette";
        case PngError::BadFilter: return "a scanline filter byte is out of range";
        case PngError::InflateFailed: return "the image data is corrupt";
        case PngError::WrongSize: return "the image data is the wrong length";
        case PngError::TooLarge: return "the image is too large";
    }
    return "unknown";
}

PngError decodePng(ConstByteSpan in, Image* out, usize maxPixels)
{
    out->width = 0;
    out->height = 0;
    out->rgba.clear();

    if (in.size() < sizeof(kSignature) + 12
        || std::memcmp(in.data(), kSignature, sizeof(kSignature)) != 0) {
        return PngError::NotPng;
    }

    int width = 0;
    int height = 0;
    u8 depth = 0;
    u8 colourType = 0;
    int channels = 0;
    bool haveHeader = false;

    u8 palette[256][4];
    int paletteEntries = 0;
    // tRNS for a palette image is a run of alpha bytes, shorter than the
    // palette when the tail is opaque; for greyscale and truecolour it names a
    // single fully transparent sample value instead.
    bool haveColourKey = false;
    u16 colourKey[3] = {0, 0, 0};

    std::vector<u8> idat;

    usize offset = sizeof(kSignature);
    bool sawEnd = false;

    while (offset + 12 <= in.size()) {
        const u32 length = readBe32(in.data() + offset);
        // The length field is 31-bit by specification, and the addition below
        // must not wrap on a 32-bit console.
        if (length > 0x7FFFFFFFu || usize(length) + 12 > in.size() - offset) {
            return PngError::Truncated;
        }
        const u8* type = in.data() + offset + 4;
        const u8* body = in.data() + offset + 8;
        offset += usize(length) + 12;

        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length != 13) {
                return PngError::BadHeader;
            }
            const u32 w = readBe32(body);
            const u32 h = readBe32(body + 4);
            depth = body[8];
            colourType = body[9];
            const u8 compression = body[10];
            const u8 filterMethod = body[11];
            const u8 interlace = body[12];

            if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) {
                return PngError::BadHeader;
            }
            if (usize(w) * usize(h) > maxPixels) {
                return PngError::TooLarge;
            }
            if (compression != 0 || filterMethod != 0) {
                return PngError::UnsupportedCompression;
            }
            if (interlace != 0) {
                return PngError::UnsupportedInterlace;
            }
            if (depth != 8) {
                return PngError::UnsupportedDepth;
            }
            channels = channelsFor(colourType);
            if (channels == 0) {
                return PngError::UnsupportedColour;
            }
            width = int(w);
            height = int(h);
            haveHeader = true;
            continue;
        }

        if (!haveHeader) {
            // Every other chunk is meaningless before IHDR, and a file that
            // puts one there is malformed rather than merely unusual.
            return PngError::BadHeader;
        }

        if (std::memcmp(type, "PLTE", 4) == 0) {
            if (length % 3 != 0 || length > 256 * 3) {
                return PngError::BadHeader;
            }
            paletteEntries = int(length / 3);
            for (int i = 0; i < paletteEntries; ++i) {
                palette[i][0] = body[i * 3 + 0];
                palette[i][1] = body[i * 3 + 1];
                palette[i][2] = body[i * 3 + 2];
                palette[i][3] = 255;
            }
            continue;
        }

        if (std::memcmp(type, "tRNS", 4) == 0) {
            if (colourType == 3) {
                // Applied to whatever PLTE has been read; the specification
                // requires PLTE first, so this is not a reordering hazard.
                const int count = int(length) < paletteEntries ? int(length) : paletteEntries;
                for (int i = 0; i < count; ++i) {
                    palette[i][3] = body[i];
                }
            } else if (colourType == 0 && length >= 2) {
                haveColourKey = true;
                colourKey[0] = u16((u16(body[0]) << 8) | body[1]);
            } else if (colourType == 2 && length >= 6) {
                haveColourKey = true;
                for (int i = 0; i < 3; ++i) {
                    colourKey[i] = u16((u16(body[i * 2]) << 8) | body[i * 2 + 1]);
                }
            }
            continue;
        }

        if (std::memcmp(type, "IDAT", 4) == 0) {
            // **Concatenated, not decoded one at a time.** A PNG may split its
            // zlib stream across any number of IDAT chunks at any byte
            // boundary, so each one on its own is not a valid stream. Every
            // encoder that writes more than 8 KB of pixels does this, including
            // the ones that wrote the jar's larger textures.
            idat.insert(idat.end(), body, body + length);
            continue;
        }

        if (std::memcmp(type, "IEND", 4) == 0) {
            sawEnd = true;
            break;
        }
        // Everything else -- gAMA, pHYs, tEXt, sRGB, cHRM, iCCP, all of which
        // the jar's files carry -- is skipped. None of them changes the pixels
        // in a way a texture atlas cares about.
    }

    if (!haveHeader || !sawEnd || idat.empty()) {
        return PngError::Truncated;
    }
    if (colourType == 3 && paletteEntries == 0) {
        return PngError::MissingPalette;
    }

    const usize stride = usize(width) * usize(channels);
    const usize expected = (stride + 1) * usize(height);

    std::vector<u8> raw;
    raw.reserve(expected);
    // The ceiling is the exact expected size: a stream that inflates to more
    // than one filtered image is malformed, and bounding it here means a
    // decompression bomb never gets to allocate.
    if (!zip::decompress(idat, raw, zip::Wrapper::Zlib, expected)) {
        return PngError::InflateFailed;
    }
    if (raw.size() != expected) {
        return PngError::WrongSize;
    }

    if (!unfilter(raw.data(), width, height, channels)) {
        return PngError::BadFilter;
    }

    out->width = width;
    out->height = height;
    out->rgba.resize(usize(width) * usize(height) * 4);

    const usize pixels = usize(width) * usize(height);
    u8* dst = out->rgba.data();
    const u8* src = raw.data();

    for (usize i = 0; i < pixels; ++i) {
        u8 r, g, b, a;
        switch (colourType) {
            case 0:
                r = g = b = src[i];
                a = (haveColourKey && src[i] == colourKey[0]) ? 0 : 255;
                break;
            case 2:
                r = src[i * 3 + 0];
                g = src[i * 3 + 1];
                b = src[i * 3 + 2];
                a = (haveColourKey && r == colourKey[0] && g == colourKey[1]
                     && b == colourKey[2])
                        ? 0
                        : 255;
                break;
            case 3: {
                const int index = src[i];
                if (index >= paletteEntries) {
                    // An out-of-range index is a corrupt file, not something to
                    // clamp into a plausible colour.
                    out->rgba.clear();
                    out->width = 0;
                    out->height = 0;
                    return PngError::BadHeader;
                }
                r = palette[index][0];
                g = palette[index][1];
                b = palette[index][2];
                a = palette[index][3];
                break;
            }
            case 4:
                r = g = b = src[i * 2 + 0];
                a = src[i * 2 + 1];
                break;
            default:
                r = src[i * 4 + 0];
                g = src[i * 4 + 1];
                b = src[i * 4 + 2];
                a = src[i * 4 + 3];
                break;
        }
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = a;
    }

    return PngError::Ok;
}

}  // namespace mc::texture
