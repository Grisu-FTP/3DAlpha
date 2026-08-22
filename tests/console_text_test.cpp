#include "framework.hpp"

#include "core/util/console_text.hpp"

#include <cstring>
#include <string>

using namespace mc;

namespace {

// Clips into a buffer big enough that only `columns` does any cutting, so a
// test that is about width is not quietly about capacity.
std::string clip(const char* in, int columns)
{
    char out[256];
    const usize written = clipToColumns(in, out, sizeof(out), columns);
    return std::string(out, written);
}

}  // namespace

TEST(text_shorter_than_the_console_is_untouched)
{
    CHECK_EQ(clip("frame 16.7 ms", 40), std::string("frame 16.7 ms"));
}

TEST(text_exactly_the_width_is_untouched)
{
    CHECK_EQ(clip("0123456789", 10), std::string("0123456789"));
}

TEST(text_wider_than_the_console_loses_its_tail)
{
    // One column over is the case that mattered: it is what turned one row into
    // two and scrolled the page.
    CHECK_EQ(clip("0123456789X", 10), std::string("0123456789"));
    CHECK_EQ(clip("cols  529 in  999 pending  529 absent!!", 20),
             std::string("cols  529 in  999 pe"));
}

TEST(an_escape_sequence_costs_no_columns)
{
    // The overlay's selection cursor: nine bytes, two drawn characters. A clip
    // that counted bytes would cut this row seven characters early.
    const char* line = "\x1b[33m> \x1b[0mteleport";
    CHECK_EQ(clip(line, 10), std::string(line));
}

TEST(an_escape_sequence_survives_the_clip_whole)
{
    // The cut lands after the two drawn characters, so the trailing reset is
    // still copied: a clip must not leave the console mid-colour.
    CHECK_EQ(clip("\x1b[33m> \x1b[0mabcdef", 2), std::string("\x1b[33m> \x1b[0m"));
}

TEST(the_bracket_is_not_mistaken_for_a_final_byte)
{
    // '[' is inside '@'..'~', so a naive scan ends the sequence on it and then
    // counts "33m" as drawn text.
    CHECK_EQ(clip("\x1b[33mABCDE", 3), std::string("\x1b[33mABC"));
}

TEST(a_truncated_escape_at_the_end_does_not_run_off)
{
    CHECK_EQ(clip("ab\x1b[3", 10), std::string("ab\x1b[3"));
}

TEST(the_output_buffer_is_never_overrun)
{
    char out[8];
    std::memset(out, '#', sizeof(out));
    const usize written = clipToColumns("0123456789ABCDEF", out, sizeof(out), 40);
    CHECK_EQ(written, usize(7));
    CHECK_EQ(out[7], '\0');
    CHECK_EQ(std::string(out), std::string("0123456"));
}

TEST(a_zero_sized_buffer_writes_nothing)
{
    char out[1] = {'#'};
    CHECK_EQ(clipToColumns("abc", out, 0, 40), usize(0));
    CHECK_EQ(out[0], '#');
}

TEST(zero_columns_keeps_escapes_and_no_text)
{
    CHECK_EQ(clip("\x1b[0mabc", 0), std::string("\x1b[0m"));
}
