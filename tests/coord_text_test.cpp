#include "framework.hpp"

#include "core/util/coord_text.hpp"

using namespace mc;

namespace {

// Parses and returns the triple. CHECK returns from its enclosing function, so
// it cannot be used in a helper that returns a value -- instead a rejection
// comes back as a sentinel that every CHECK_EQ below will report loudly, with
// the value it got.
constexpr double kRejected = -999999.0;

CoordTriple ok(const char* text)
{
    CoordTriple out;
    if (parseCoordinateTriple(text, &out) != nullptr) {
        return CoordTriple{kRejected, kRejected, kRejected};
    }
    return out;
}

bool rejected(const char* text)
{
    CoordTriple out{-1.0, -1.0, -1.0};
    const char* error = parseCoordinateTriple(text, &out);
    if (error == nullptr) {
        return false;
    }
    // A rejected parse must not have written anything: the caller keeps its
    // existing position, and a half-applied teleport that moved x but not z
    // would be worse than no teleport at all.
    return out.x == -1.0 && out.y == -1.0 && out.z == -1.0;
}

}  // namespace

TEST(a_plain_triple_parses)
{
    const CoordTriple c = ok("10 64 -20");
    CHECK_EQ(c.x, 10.0);
    CHECK_EQ(c.y, 64.0);
    CHECK_EQ(c.z, -20.0);
}

// Whichever way someone types the separators. A 3DS keyboard makes commas and
// spaces equally awkward, so both work and so does a mixture.
TEST(separators_are_forgiving)
{
    for (const char* text : {"10 64 -20", "10,64,-20", "10, 64, -20", "  10   64   -20  ",
                             "10 ,64, -20", "+10 64 -20"}) {
        const CoordTriple c = ok(text);
        CHECK_EQ(c.x, 10.0);
        CHECK_EQ(c.y, 64.0);
        CHECK_EQ(c.z, -20.0);
    }
}

TEST(decimals_are_kept)
{
    const CoordTriple c = ok("0.5 64.25 -0.75");
    CHECK_EQ(c.x, 0.5);
    CHECK_EQ(c.y, 64.25);
    CHECK_EQ(c.z, -0.75);
}

// The whole reason this feature exists: the Far Lands are at 12,550,824, which
// is six real days of walking away at the free camera's sprint speed. If the
// parser cannot express the one coordinate worth typing, it is decoration.
TEST(the_far_lands_can_be_typed)
{
    const CoordTriple c = ok("12550824 70 0");
    CHECK_EQ(c.x, 12550824.0);
    CHECK_EQ(c.y, 70.0);
    CHECK_EQ(c.z, 0.0);

    const CoordTriple negative = ok("-12550824 70 0");
    CHECK_EQ(negative.x, -12550824.0);

    // Exactly on the world's horizontal limit, which is inside the bound rather
    // than outside it -- the original refuses *past* 32,000,000.
    const CoordTriple edge = ok("32000000 1 -32000000");
    CHECK_EQ(edge.x, 32000000.0);
    CHECK_EQ(edge.z, -32000000.0);
}

TEST(out_of_range_is_refused_rather_than_clamped)
{
    // Silently clamping would be the worst outcome: you type a coordinate, the
    // game moves you somewhere else, and nothing says so.
    CHECK(rejected("32000001 64 0"));
    CHECK(rejected("0 64 -32000001"));
    CHECK(rejected("0 0 0"));    // y below the camera's floor
    CHECK(rejected("0 255 0"));  // and above its ceiling
    CHECK(rejected("0 -5 0"));
}

// The reason this is hand-written instead of three strtod calls. Each of these
// is something strtod accepts and turns into a number.
TEST(strtod_spellings_that_are_not_coordinates_are_refused)
{
    // A NaN coordinate is the dangerous one: it survives every range check
    // written as a comparison, reaches the view matrix, and makes the world
    // stop drawing rather than reporting anything.
    CHECK(rejected("nan 64 0"));
    CHECK(rejected("0 nan 0"));
    CHECK(rejected("inf 64 0"));
    CHECK(rejected("-inf 64 0"));
    CHECK(rejected("infinity 64 0"));

    // Hex floats and exponents. "1e9" is a plausible thing to type and would
    // silently mean a billion.
    CHECK(rejected("0x10 64 0"));
    CHECK(rejected("1e9 64 0"));
    CHECK(rejected("1E9 64 0"));
}

TEST(malformed_input_is_refused)
{
    CHECK(rejected(""));
    CHECK(rejected("   "));
    CHECK(rejected("10"));
    CHECK(rejected("10 64"));
    CHECK(rejected("10 64 20 30"));
    CHECK(rejected("ten 64 20"));
    CHECK(rejected("10 64 20x"));
    CHECK(rejected("10,,64,20"));
    CHECK(rejected("10 64 20,"));
    CHECK(rejected("- 10 64 20"));
    CHECK(rejected("10 64 ."));
    CHECK(rejected("10 64 20."));
    CHECK(rejected("10 64 .5"));
    CHECK(rejected(nullptr));
}

// A number immediately followed by a sign is two numbers to a machine and one
// typo to a person. "1-2 3" has to be refused rather than read as 1, -2, 3.
TEST(a_missing_separator_is_refused_not_guessed)
{
    CHECK(rejected("1-2 3"));
    CHECK(rejected("10 64-20"));
    CHECK(rejected("10 64+20"));
}

// Trailing whitespace on its own is fine -- it is what a keyboard leaves behind
// when someone taps space before OK -- and the case above only rejected it
// because the input was already four numbers.
TEST(surrounding_whitespace_alone_is_accepted)
{
    const CoordTriple c = ok(" \t10 64 -20 \t");
    CHECK_EQ(c.x, 10.0);
    CHECK_EQ(c.z, -20.0);
}

// Every failure has to name itself, because the message is what the keyboard
// shows while staying open. An empty or generic message would leave the player
// pressing OK against a dialog that refuses without saying why.
TEST(every_rejection_carries_a_message)
{
    for (const char* text : {"", "   ", "10", "ten 64 20", "1-2 3", "nan 64 0", "0 255 0",
                             "32000001 64 0", "10 64 20 30"}) {
        CoordTriple out;
        const char* error = parseCoordinateTriple(text, &out);
        CHECK(error != nullptr);
        CHECK(error[0] != '\0');
    }
}
