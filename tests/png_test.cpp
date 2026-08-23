#include "framework.hpp"
#include "texture_support.hpp"

#include "core/texture/png.hpp"

using namespace mc;
using mc::test::makePng;
using mc::test::makeRgbaPng;
using texture::decodePng;
using texture::Image;
using texture::PngError;

namespace {

// One pixel of each corner, so a decode that transposes or flips rows shows up
// as a wrong colour rather than as an equal-looking image.
std::vector<u8> cornerImage()
{
    return {
        255, 0,   0,   255,  // top-left    red
        0,   255, 0,   128,  // top-right   green, half alpha
        0,   0,   255, 255,  // bottom-left blue
        255, 255, 0,   0,    // bottom-right yellow, transparent
    };
}

TEST(decodes_rgba)
{
    const std::vector<u8> png = makeRgbaPng(2, 2, cornerImage());

    Image image;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::Ok));
    CHECK_EQ(image.width, 2);
    CHECK_EQ(image.height, 2);
    CHECK(image.rgba == cornerImage());
}

// Row 0 of the decoded image must be row 0 of the file. The whole upload chain
// downstream -- GX_TRANSFER_FLIP_VERT and the mesher's v -- is calibrated on
// that, so a decoder that helpfully flipped would put the atlas upside down.
TEST(row_zero_is_the_top_row)
{
    std::vector<u8> rgba(4 * 4 * 4, 0);
    rgba[3] = 255;  // top-left texel opaque, everything else transparent
    const std::vector<u8> png = makeRgbaPng(4, 4, rgba);

    Image image;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::Ok));
    CHECK_EQ(int(image.rgba[3]), 255);
    CHECK_EQ(int(image.rgba[usize(4 * 3) * 4 + 3]), 0);
}

// Every one of the five filters, chosen deliberately rather than left to an
// encoder. Each row of the source is a ramp, which none of the filters reduces
// to a no-op, so a filter implemented wrong produces different pixels rather
// than the same ones.
TEST(decodes_every_scanline_filter)
{
    constexpr int kEdge = 5;

    std::vector<u8> expected(kEdge * kEdge * 3);
    for (int y = 0; y < kEdge; ++y) {
        for (int x = 0; x < kEdge; ++x) {
            u8* p = expected.data() + (usize(y) * kEdge + usize(x)) * 3;
            p[0] = u8(x * 37 + y * 11);
            p[1] = u8(x * 5 + y * 61);
            p[2] = u8(x * 91 ^ (y * 13));
        }
    }

    // Filter each row with filter number == its index, so one image covers all
    // five. The encoder side is the inverse of the decoder's, written out here
    // rather than shared, so a sign error cannot cancel itself.
    std::vector<u8> filtered;
    const usize stride = kEdge * 3;
    for (int y = 0; y < kEdge; ++y) {
        const u8 filter = u8(y);
        filtered.push_back(filter);
        for (usize i = 0; i < stride; ++i) {
            const int raw = expected[usize(y) * stride + i];
            const int a = i >= 3 ? expected[usize(y) * stride + i - 3] : 0;
            const int b = y > 0 ? expected[usize(y - 1) * stride + i] : 0;
            const int c = (y > 0 && i >= 3) ? expected[usize(y - 1) * stride + i - 3] : 0;

            int predictor = 0;
            switch (filter) {
                case 0: predictor = 0; break;
                case 1: predictor = a; break;
                case 2: predictor = b; break;
                case 3: predictor = (a + b) >> 1; break;
                default: {
                    const int p = a + b - c;
                    const int pa = p > a ? p - a : a - p;
                    const int pb = p > b ? p - b : b - p;
                    const int pc = p > c ? p - c : c - p;
                    predictor = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
            }
            filtered.push_back(u8(raw - predictor));
        }
    }

    const std::vector<u8> png = makePng(kEdge, kEdge, 2, filtered);

    Image image;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::Ok));
    for (int i = 0; i < kEdge * kEdge; ++i) {
        CHECK_EQ(int(image.rgba[usize(i) * 4 + 0]), int(expected[usize(i) * 3 + 0]));
        CHECK_EQ(int(image.rgba[usize(i) * 4 + 1]), int(expected[usize(i) * 3 + 1]));
        CHECK_EQ(int(image.rgba[usize(i) * 4 + 2]), int(expected[usize(i) * 3 + 2]));
        CHECK_EQ(int(image.rgba[usize(i) * 4 + 3]), 255);
    }
}

// Four of the jar's 58 files are palette images, so this is not a hypothetical
// path.
TEST(decodes_palette_with_transparency)
{
    const std::vector<u8> palette = {255, 0, 0, 0, 255, 0, 0, 0, 255};
    const std::vector<u8> trns = {0, 128};  // entry 2 has no tRNS byte: opaque
    const std::vector<u8> filtered = {0, 0, 1, 2};  // one row, three indices

    const std::vector<u8> png = makePng(3, 1, 3, filtered, palette, trns);

    Image image;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::Ok));
    CHECK_EQ(int(image.rgba[0]), 255);
    CHECK_EQ(int(image.rgba[3]), 0);    // fully transparent
    CHECK_EQ(int(image.rgba[7]), 128);  // half
    CHECK_EQ(int(image.rgba[11]), 255); // beyond tRNS, so opaque
}

TEST(decodes_greyscale_with_colour_key)
{
    std::vector<u8> trns = {0, 40};  // sample 40 is the transparent one
    const std::vector<u8> filtered = {0, 10, 40, 200};

    const std::vector<u8> png = makePng(3, 1, 0, filtered, {}, trns);

    Image image;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::Ok));
    CHECK_EQ(int(image.rgba[0]), 10);
    CHECK_EQ(int(image.rgba[3]), 255);
    CHECK_EQ(int(image.rgba[4]), 40);
    CHECK_EQ(int(image.rgba[7]), 0);
    CHECK_EQ(int(image.rgba[11]), 255);
}

// A PNG may split its zlib stream across any number of IDAT chunks at any byte
// boundary, so a decoder that inflates each chunk on its own fails on every
// image above a few kilobytes -- which is most of a real terrain.png.
TEST(concatenates_split_idat)
{
    std::vector<u8> rgba(32 * 32 * 4);
    for (usize i = 0; i < rgba.size(); ++i) {
        rgba[i] = u8(i * 7);
    }

    std::vector<u8> filtered;
    for (int y = 0; y < 32; ++y) {
        filtered.push_back(0);
        const usize offset = usize(y) * 32 * 4;
        filtered.insert(filtered.end(), rgba.begin() + long(offset),
                        rgba.begin() + long(offset) + 32 * 4);
    }
    const std::vector<u8> png = makePng(32, 32, 6, filtered, {}, {}, /*splitIdat=*/true);

    Image image;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::Ok));
    CHECK(image.rgba == rgba);
}

TEST(refuses_what_it_does_not_decode)
{
    Image image;

    const std::vector<u8> notPng = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                    11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    CHECK_EQ(int(decodePng(notPng, &image)), int(PngError::NotPng));

    // A truncated file: everything after the signature cut away mid-chunk.
    std::vector<u8> png = makeRgbaPng(2, 2, cornerImage());
    const std::vector<u8> cut(png.begin(), png.begin() + 30);
    const PngError error = decodePng(cut, &image);
    CHECK(error == PngError::Truncated || error == PngError::NotPng);
    CHECK(image.rgba.empty());

    // Interlaced: the interlace byte is the last of IHDR's 13, which begins at
    // offset 16 in the file.
    png = makeRgbaPng(2, 2, cornerImage());
    png[16 + 12] = 1;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::UnsupportedInterlace));

    // 16 bits per sample.
    png = makeRgbaPng(2, 2, cornerImage());
    png[16 + 8] = 16;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::UnsupportedDepth));

    // A colour type PNG does not define.
    png = makeRgbaPng(2, 2, cornerImage());
    png[16 + 9] = 5;
    CHECK_EQ(int(decodePng(png, &image)), int(PngError::UnsupportedColour));
}

// A header that claims more pixels than the caller will allow must be refused
// before anything is allocated for it, not after.
TEST(refuses_an_image_beyond_the_ceiling)
{
    const std::vector<u8> png = makeRgbaPng(4, 4, std::vector<u8>(4 * 4 * 4, 0x40));

    Image image;
    CHECK_EQ(int(decodePng(png, &image, /*maxPixels=*/8)), int(PngError::TooLarge));
    CHECK(image.rgba.empty());
}

// Corrupting the compressed stream must fail, not produce half an image. The
// decoder bounds inflation at exactly the size the header implies, so a stream
// that expands further is refused rather than truncated into something
// plausible.
TEST(refuses_corrupt_pixel_data)
{
    std::vector<u8> png = makeRgbaPng(8, 8, std::vector<u8>(8 * 8 * 4, 0x77));

    // The IDAT body starts 8 (signature) + 25 (IHDR) + 8 (length and type)
    // bytes in; flipping a byte well inside it breaks the deflate stream
    // without touching the chunk framing.
    png[8 + 25 + 8 + 6] ^= 0xFF;

    Image image;
    const PngError error = decodePng(png, &image);
    CHECK(error != PngError::Ok);
    CHECK(image.rgba.empty());
}

}  // namespace
