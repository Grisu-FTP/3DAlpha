#include "framework.hpp"

#include "core/gui/progress.hpp"

#include <vector>

using namespace mc;
using namespace mc::gui;

namespace {

// Both stride conventions, exactly as paint_test.cpp does it: a row-major
// canvas and the console's own -- 240 down a column, bottom-up. The square and
// the bar are drawn through the second one on the hardware and through the
// first one nowhere at all, so anything checked through only one of them is
// checked in the wrong place.
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

}  // namespace

TEST(nothing_owed_fills_the_bar_rather_than_emptying_it)
{
    // The world the pause menu saved a moment ago owes nothing, and it has not
    // failed to save. This is the case the old console text answered with the
    // words "already saved"; the bar has to answer it too.
    CHECK_EQ(barFillWidth(100, 0, 0), 100);
    CHECK_EQ(barFillWidth(100, 7, 0), 100);
}

TEST(the_bar_fills_in_proportion_and_never_past_the_end)
{
    CHECK_EQ(barFillWidth(100, 0, 4), 0);
    CHECK_EQ(barFillWidth(100, 1, 4), 25);
    CHECK_EQ(barFillWidth(100, 2, 4), 50);
    CHECK_EQ(barFillWidth(100, 4, 4), 100);
    // More done than owed is not a state the streamer produces, but the save
    // path subtracts two counters read at different moments and a negative one
    // wraps. Clamped rather than trusted.
    CHECK_EQ(barFillWidth(100, 9, 4), 100);

    // A track with no pixels in it, which is what a bar drawn before its
    // rectangle has been sized would ask for.
    CHECK_EQ(barFillWidth(0, 1, 2), 0);
    CHECK_EQ(barFillWidth(-4, 1, 2), 0);
}

TEST(the_bar_multiplies_in_64_bits_rather_than_wrapping)
{
    // 32 bits would overflow here: 3,000,000,000 x 280 is well past 2^32, and
    // the answer would come back as a bar that is nearly empty at 90 per cent.
    // A column count is a u32 off the chunk cache, so the width has to be
    // computed in something wider.
    CHECK_EQ(barFillWidth(280, 3000000000u, 4000000000u), 210);
}

TEST(the_bar_is_a_frame_a_track_and_a_fill_with_two_tone_edges)
{
    Canvas canvas(20, 10);
    BarStyle style;
    progressBar(canvas.surface(), 2, 2, 16, 6, 1, 2);

    // The frame is the outside pixel all the way round.
    CHECK_EQ(canvas.at(2, 2), style.frame);
    CHECK_EQ(canvas.at(17, 7), style.frame);
    CHECK_EQ(canvas.at(9, 2), style.frame);

    // The track is 14 wide, so half of it is 7: x = 3..9 filled, 10..16 not.
    CHECK_EQ(canvas.at(3, 4), style.fill);
    CHECK_EQ(canvas.at(9, 4), style.fill);
    CHECK_EQ(canvas.at(10, 4), style.track);
    CHECK_EQ(canvas.at(16, 4), style.track);

    // The gloss and the shade only run as far as the fill does -- a bar with a
    // bright line across an empty track would read as full.
    CHECK_EQ(canvas.at(3, 3), style.gloss);
    CHECK_EQ(canvas.at(9, 3), style.gloss);
    CHECK_EQ(canvas.at(10, 3), style.track);
    CHECK_EQ(canvas.at(3, 6), style.shade);
    CHECK_EQ(canvas.at(10, 6), style.track);

    // Nothing outside the rectangle it was given.
    CHECK_EQ(canvas.at(1, 2), Pixel(0));
    CHECK_EQ(canvas.at(18, 5), Pixel(0));
}

TEST(the_bar_draws_the_same_through_the_consoles_strides)
{
    Canvas rowMajor(20, 10);
    ColumnCanvas console(20, 10);
    progressBar(rowMajor.surface(), 2, 2, 16, 6, 3, 4);
    progressBar(console.surface(), 2, 2, 16, 6, 3, 4);

    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 20; ++x) {
            CHECK_EQ(rowMajor.at(x, y), console.at(x, y));
        }
    }
}

TEST(a_bar_too_small_to_have_an_inside_draws_nothing)
{
    Canvas canvas(8, 8);
    progressBar(canvas.surface(), 1, 1, 3, 6, 1, 1);
    progressBar(canvas.surface(), 1, 1, 6, 3, 1, 1);
    for (const Pixel pixel : canvas.pixels) {
        CHECK_EQ(pixel, Pixel(0));
    }
}

TEST(a_short_bar_keeps_its_fill_rather_than_spending_it_on_the_two_tone)
{
    // Four pixels of frame and track leaves two of fill; a gloss line and a
    // shade line would take both, and the bar would be the wrong colour at
    // every fraction.
    Canvas canvas(20, 8);
    BarStyle style;
    progressBar(canvas.surface(), 0, 0, 16, 5, 1, 1);
    CHECK_EQ(canvas.at(4, 1), style.fill);
    CHECK_EQ(canvas.at(4, 3), style.fill);
}

TEST(the_square_shrinks_its_cells_rather_than_running_off_the_box)
{
    // The whole reason the pitch is derived rather than fixed: the render
    // distance is a live setting, and the same box has to hold distance 2 and
    // distance 12.
    const GridLayout small = fitChunkGrid(0, 0, 176, 176, 5, 0);
    CHECK_EQ(small.pitch, 35);
    CHECK_EQ(small.size(), 175);

    const GridLayout large = fitChunkGrid(0, 0, 176, 176, 27, 0);
    CHECK_EQ(large.pitch, 6);
    CHECK_EQ(large.size(), 162);
    CHECK(large.size() <= 176);

    // Distance 24 -- the debug page's ceiling -- still fits.
    const GridLayout huge = fitChunkGrid(0, 0, 176, 176, 49, 0);
    CHECK_EQ(huge.pitch, 3);
    CHECK(huge.size() <= 176);
}

TEST(the_square_is_centred_in_whatever_the_pitch_left_over)
{
    // 27 cells at a pitch of 6 is 162 of 176, so seven pixels either side.
    const GridLayout layout = fitChunkGrid(8, 26, 176, 176, 27, 0);
    CHECK_EQ(layout.x, 8 + 7);
    CHECK_EQ(layout.y, 26 + 7);

    // A box that is not square centres in both directions independently.
    const GridLayout wide = fitChunkGrid(0, 0, 200, 100, 10, 0);
    CHECK_EQ(wide.pitch, 10);
    CHECK_EQ(wide.x, 50);
    CHECK_EQ(wide.y, 0);
}

TEST(the_gap_is_taken_out_of_the_pitch_rather_than_added_to_it)
{
    // Turning the gap on must not make a square that was just fitted to a box
    // outgrow it.
    const GridLayout gapped = fitChunkGrid(0, 0, 100, 100, 10, 0);
    CHECK_EQ(gapped.pitch, 10);
    CHECK_EQ(gapped.gap, 1);
    CHECK_EQ(gapped.size(), 100);

    // Below five pixels there is nothing left to spend on a gap.
    const GridLayout tight = fitChunkGrid(0, 0, 100, 100, 25, 0);
    CHECK_EQ(tight.pitch, 4);
    CHECK_EQ(tight.gap, 0);
}

TEST(a_ceiling_on_the_pitch_stops_a_low_distance_becoming_five_huge_blocks)
{
    const GridLayout layout = fitChunkGrid(0, 0, 176, 176, 5, 16);
    CHECK_EQ(layout.pitch, 16);
    CHECK_EQ(layout.size(), 80);
    // ...and it is still centred, on the box rather than on the square.
    CHECK_EQ(layout.x, 48);
}

TEST(a_square_wider_than_its_box_floors_at_one_pixel_and_is_still_clipped)
{
    const GridLayout layout = fitChunkGrid(0, 0, 20, 20, 40, 0);
    CHECK_EQ(layout.pitch, 1);
    CHECK_EQ(layout.gap, 0);

    // maxGridEdge is what a caller clamps against so this never happens; when
    // it does, drawing must still stay inside the surface.
    CHECK_EQ(maxGridEdge(20, 20), 20);
    CHECK_EQ(maxGridEdge(176, 176), 176);
    CHECK_EQ(maxGridEdge(200, 100), 100);

    Canvas canvas(20, 20);
    std::vector<ChunkState> cells(40 * 40, ChunkState::Done);
    drawChunkGrid(canvas.surface(), layout, cells.data());
    // Nothing was written outside the canvas -- ASan would have said so -- and
    // the part that fits was drawn.
    CHECK_EQ(canvas.at(0, 0), chunkStatePalette()[int(ChunkState::Done)]);
    CHECK_EQ(canvas.at(19, 19), chunkStatePalette()[int(ChunkState::Done)]);
}

TEST(the_square_draws_row_major_from_the_north_west_corner)
{
    // North up, west left -- the same orientation the map draws in, because the
    // two screens must not disagree about which way the world faces.
    Canvas canvas(30, 30);
    const GridLayout layout = fitChunkGrid(0, 0, 30, 30, 3, 0);
    CHECK_EQ(layout.pitch, 10);
    CHECK_EQ(layout.gap, 1);

    ChunkState cells[9] = {
        ChunkState::Unstarted, ChunkState::Owed,  ChunkState::Working,
        ChunkState::Ready,     ChunkState::Done,  ChunkState::Unstarted,
        ChunkState::Owed,      ChunkState::Ready, ChunkState::Done,
    };
    drawChunkGrid(canvas.surface(), layout, cells);

    const Pixel* palette = chunkStatePalette();
    // Row 0 is the northern one, and its second entry is the one to the east.
    CHECK_EQ(canvas.at(0, 0), palette[int(ChunkState::Unstarted)]);
    CHECK_EQ(canvas.at(10, 0), palette[int(ChunkState::Owed)]);
    CHECK_EQ(canvas.at(20, 0), palette[int(ChunkState::Working)]);
    // Row 1 is one chunk south.
    CHECK_EQ(canvas.at(0, 10), palette[int(ChunkState::Ready)]);
    CHECK_EQ(canvas.at(10, 10), palette[int(ChunkState::Done)]);
    // Row 2.
    CHECK_EQ(canvas.at(20, 20), palette[int(ChunkState::Done)]);

    // The gap is the last pixel of the pitch and is left as it was, which is
    // what draws the grid lines between chunks without drawing any.
    CHECK_EQ(canvas.at(9, 0), Pixel(0));
    CHECK_EQ(canvas.at(0, 9), Pixel(0));
}

TEST(the_square_draws_the_same_through_the_consoles_strides)
{
    ChunkState cells[9] = {
        ChunkState::Done,      ChunkState::Owed,  ChunkState::Working,
        ChunkState::Ready,     ChunkState::Done,  ChunkState::Unstarted,
        ChunkState::Unstarted, ChunkState::Ready, ChunkState::Owed,
    };
    const GridLayout layout = fitChunkGrid(2, 3, 24, 24, 3, 0);

    Canvas rowMajor(30, 30);
    ColumnCanvas console(30, 30);
    drawChunkGrid(rowMajor.surface(), layout, cells);
    drawChunkGrid(console.surface(), layout, cells);

    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 30; ++x) {
            CHECK_EQ(rowMajor.at(x, y), console.at(x, y));
        }
    }
}

TEST(the_ramp_runs_black_red_orange_yellow_green_and_ends_on_the_xp_bars_own)
{
    const Pixel* palette = chunkStatePalette();

    // Untouched is nearly black, and darker than everything else on the ramp.
    CHECK_EQ(palette[int(ChunkState::Unstarted)], rgb565(0x1A1A1Au));
    // Done is the XP bar's green, which is also the bar's fill -- the square
    // and the bar finish in the same colour.
    CHECK_EQ(palette[int(ChunkState::Done)], rgb565(0x80FF20u));
    CHECK_EQ(palette[int(ChunkState::Done)], BarStyle{}.fill);

    // The middle three are distinguishable from each other, which is the whole
    // job of a ramp drawn at six pixels a cell.
    CHECK(palette[int(ChunkState::Owed)] != palette[int(ChunkState::Working)]);
    CHECK(palette[int(ChunkState::Working)] != palette[int(ChunkState::Ready)]);
    CHECK(palette[int(ChunkState::Owed)] != palette[int(ChunkState::Ready)]);
}

TEST(an_empty_square_or_a_null_one_draws_nothing_rather_than_reading_off_the_end)
{
    Canvas canvas(16, 16);
    const GridLayout layout = fitChunkGrid(0, 0, 16, 16, 4, 0);
    drawChunkGrid(canvas.surface(), layout, nullptr);
    drawChunkGrid(canvas.surface(), GridLayout{}, nullptr);
    for (const Pixel pixel : canvas.pixels) {
        CHECK_EQ(pixel, Pixel(0));
    }
    CHECK(!GridLayout{}.valid());
    CHECK(!fitChunkGrid(0, 0, 100, 100, 0, 0).valid());
}
