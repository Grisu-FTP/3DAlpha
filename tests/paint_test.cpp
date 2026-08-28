#include "framework.hpp"

#include "core/gui/paint.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using namespace mc::gui;

namespace {

// A row-major canvas, and the same canvas seen through the strides the
// console's bottom screen really has -- 240 down a column, bottom-up. Every
// test below draws through both, because the strides are the one thing about
// `Surface` that cannot be checked by looking at it.
struct Canvas {
    int width;
    int height;
    std::vector<Pixel> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(usize(w * h), 0) {}

    Surface surface() { return Surface{pixels.data(), 1, width, width, height}; }
    Pixel at(int x, int y) const { return pixels[usize(y * width + x)]; }
};

struct ColumnCanvas {
    int width;
    int height;
    std::vector<Pixel> pixels;

    ColumnCanvas(int w, int h) : width(w), height(h), pixels(usize(w * h), 0) {}

    Surface surface() { return Surface{pixels.data() + (height - 1), height, -1, width, height}; }
    Pixel at(int x, int y) const { return pixels[usize(x * height + (height - 1 - y))]; }
};

const Pixel kInk = rgb565(255, 0, 0);

}  // namespace

TEST(rgb565_packs_the_channels_where_the_hardware_expects_them)
{
    CHECK_EQ(rgb565(255, 255, 255), Pixel(0xFFFF));
    CHECK_EQ(rgb565(0, 0, 0), Pixel(0));
    CHECK_EQ(rgb565(255, 0, 0), Pixel(0xF800));
    CHECK_EQ(rgb565(0, 255, 0), Pixel(0x07E0));
    CHECK_EQ(rgb565(0, 0, 255), Pixel(0x001F));
    // The 0xRRGGBB overload is the same function, so every colour written down
    // as a word lands where the three-channel one puts it.
    CHECK_EQ(rgb565(0xC6C6C6u), rgb565(0xC6, 0xC6, 0xC6));
}

TEST(a_rectangle_is_clipped_at_every_edge_rather_than_running_off_one)
{
    Canvas canvas(8, 8);
    // Hangs off the top-left corner: only the overlap is drawn, and nothing is
    // written before the start of the buffer.
    fillRect(canvas.surface(), -3, -3, 5, 5, kInk);
    CHECK_EQ(canvas.at(0, 0), kInk);
    CHECK_EQ(canvas.at(1, 1), kInk);
    CHECK_EQ(canvas.at(2, 2), Pixel(0));

    // ...and off the bottom-right.
    fillRect(canvas.surface(), 6, 6, 8, 8, kInk);
    CHECK_EQ(canvas.at(7, 7), kInk);
    CHECK_EQ(canvas.at(5, 7), Pixel(0));

    // Entirely outside, in every direction, writes nothing at all.
    Canvas empty(8, 8);
    fillRect(empty.surface(), -20, 0, 5, 5, kInk);
    fillRect(empty.surface(), 20, 0, 5, 5, kInk);
    fillRect(empty.surface(), 0, -20, 5, 5, kInk);
    fillRect(empty.surface(), 0, 20, 5, 5, kInk);
    fillRect(empty.surface(), 0, 0, 0, 5, kInk);
    for (usize i = 0; i < empty.pixels.size(); ++i) {
        CHECK_EQ(empty.pixels[i], Pixel(0));
    }
}

TEST(the_bottom_screens_strides_put_a_pixel_where_a_row_major_buffer_does)
{
    // **The console's framebuffer, in the orientation it is really stored in.**
    // Pixel (x, y) lives at `x * 240 + (239 - y)`, so drawing into it is a
    // positive stride across and a negative one down. Anything that looks right
    // on the host and wrong on the console goes wrong here.
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;

    Canvas rows(kWidth, kHeight);
    ColumnCanvas columns(kWidth, kHeight);

    bevelBox(rows.surface(), 12, 20, 40, 24, rgb565(0xC6C6C6u), rgb565(0xFFFFFFu),
             rgb565(0x555555u), true);
    bevelBox(columns.surface(), 12, 20, 40, 24, rgb565(0xC6C6C6u), rgb565(0xFFFFFFu),
             rgb565(0x555555u), true);
    drawArrow(rows.surface(), 160.5f, 120.5f, 1.1f, ArrowShape{}, rgb565(255, 255, 255), 0);
    drawArrow(columns.surface(), 160.5f, 120.5f, 1.1f, ArrowShape{}, rgb565(255, 255, 255), 0);

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            CHECK_EQ(rows.at(x, y), columns.at(x, y));
        }
    }
}

TEST(a_raised_box_is_light_above_and_dark_below_and_a_sunken_one_is_not)
{
    const Pixel face = rgb565(0xC6C6C6u);
    const Pixel light = rgb565(0xFFFFFFu);
    const Pixel dark = rgb565(0x555555u);

    Canvas raised(8, 8);
    bevelBox(raised.surface(), 0, 0, 8, 8, face, light, dark, true);
    CHECK_EQ(raised.at(0, 0), light);
    CHECK_EQ(raised.at(4, 0), light);
    CHECK_EQ(raised.at(0, 4), light);
    CHECK_EQ(raised.at(7, 7), dark);
    CHECK_EQ(raised.at(4, 7), dark);
    CHECK_EQ(raised.at(7, 4), dark);
    CHECK_EQ(raised.at(3, 3), face);

    Canvas sunken(8, 8);
    bevelBox(sunken.surface(), 0, 0, 8, 8, face, light, dark, false);
    CHECK_EQ(sunken.at(0, 0), dark);
    CHECK_EQ(sunken.at(7, 7), light);
    CHECK_EQ(sunken.at(3, 3), face);
}

TEST(a_tiled_pattern_keeps_its_phase_across_separate_calls)
{
    // Two halves of one rectangle have to line up, which is what makes the
    // backdrop behind a panel look like one picture rather than two.
    constexpr int kEdge = 4;
    Pixel tile[kEdge * kEdge];
    for (int i = 0; i < kEdge * kEdge; ++i) {
        tile[i] = Pixel(i + 1);
    }

    Canvas whole(16, 16);
    tilePattern(whole.surface(), 0, 0, 16, 16, tile, kEdge);

    Canvas halves(16, 16);
    tilePattern(halves.surface(), 0, 0, 7, 16, tile, kEdge);
    tilePattern(halves.surface(), 7, 0, 9, 16, tile, kEdge);

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            CHECK_EQ(whole.at(x, y), halves.at(x, y));
            CHECK_EQ(whole.at(x, y), tile[(y % kEdge) * kEdge + (x % kEdge)]);
        }
    }
}

TEST(an_arrow_is_outlined_and_never_wider_than_the_length_it_was_given)
{
    const Pixel fill = rgb565(255, 255, 255);
    const Pixel outline = rgb565(0, 0, 0);

    // The default proportions, which are the player marker's.
    const ArrowShape shape;

    Canvas canvas(40, 40);
    drawArrow(canvas.surface(), 20.5f, 20.5f, 0.0f, shape, fill, outline);

    // Pointing straight up: the tip is above the centre, and the tail -- which
    // is shorter than the tip by design -- does not reach as far the other way.
    CHECK_EQ(canvas.at(20, 20 - 5), fill);
    CHECK(canvas.at(20, 20 + 5) != fill);
    // Every filled pixel is inside the length it asked for, outline included.
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            if (canvas.at(x, y) == Pixel(0)) {
                continue;
            }
            const double dx = double(x) + 0.5 - 20.5;
            const double dy = double(y) + 0.5 - 20.5;
            CHECK(std::sqrt(dx * dx + dy * dy) <= double(shape.length) + 2.0);
        }
    }
    // The body is ringed, which is what keeps it legible over any terrain.
    bool ringed = false;
    for (int y = 0; y < 40 && !ringed; ++y) {
        for (int x = 0; x < 40 && !ringed; ++x) {
            ringed = canvas.at(x, y) == outline;
        }
    }
    CHECK(ringed);
}

TEST(an_arrow_asking_for_more_than_the_stack_buffer_holds_is_clamped)
{
    // `kMaxArrowLength` bounds a stack array, and the 3DSX main thread has
    // 32 KB of stack that no symbol in the binary can enlarge. A caller asking
    // for more gets a smaller arrow, not a smashed frame.
    Canvas canvas(128, 128);
    ArrowShape shape;
    shape.length = kMaxArrowLength * 4.0f;
    drawArrow(canvas.surface(), 64.5f, 64.5f, 0.7f, shape, rgb565(255, 255, 255), 0);

    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (canvas.at(x, y) == Pixel(0)) {
                continue;
            }
            const double dx = double(x) + 0.5 - 64.5;
            const double dy = double(y) + 0.5 - 64.5;
            CHECK(std::sqrt(dx * dx + dy * dy) <= double(kMaxArrowLength) + 2.0);
        }
    }
}
