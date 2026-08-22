#include "framework.hpp"

#include "core/util/seed_text.hpp"

#include <string>

using namespace mc;

namespace {

i64 seedOf(const char* text)
{
    i64 seed = 0;
    return seedFromText(text, &seed) ? seed : 0;
}

}  // namespace

TEST(a_blank_seed_means_roll_one)
{
    i64 seed = 12345;
    CHECK(!seedFromText("", &seed));
    CHECK(!seedFromText("   ", &seed));
    CHECK(!seedFromText("\t\n", &seed));
    // Untouched on the way out, so a caller that ignores the return value
    // cannot silently plant a seed of zero.
    CHECK_EQ(seed, i64(12345));
}

TEST(a_number_is_that_number)
{
    CHECK_EQ(seedOf("0"), i64(0));
    CHECK_EQ(seedOf("1"), i64(1));
    CHECK_EQ(seedOf("-1"), i64(-1));
    CHECK_EQ(seedOf("+42"), i64(42));
    CHECK_EQ(seedOf("  1234567890  "), i64(1234567890));
    CHECK_EQ(seedOf("2836933080"), i64(2836933080LL));

    // The two ends of a long, including the asymmetric one: Long.parseLong
    // accepts -9223372036854775808 and rejects its positive twin.
    CHECK_EQ(seedOf("9223372036854775807"), i64(9223372036854775807LL));
    CHECK_EQ(seedOf("-9223372036854775808"), i64(-9223372036854775807LL - 1));
}

TEST(anything_that_is_not_a_number_is_hashed_the_way_java_hashes_it)
{
    // Read off a real JVM (`java H.java`, printing String.hashCode) rather
    // than recalled: the arithmetic is specified, so any JVM agrees, and these
    // are the numbers one printed.
    CHECK_EQ(javaStringHash(""), i32(0));
    CHECK_EQ(javaStringHash("a"), i32(97));
    CHECK_EQ(javaStringHash("hello"), i32(99162322));
    CHECK_EQ(javaStringHash("gargamel"), i32(-1623774494));
    CHECK_EQ(javaStringHash("glacier"), i32(108181935));
}

TEST(a_number_too_big_for_a_long_falls_through_to_the_hash)
{
    // Exactly the case Long.parseLong throws on, which is what sends Minecraft
    // to the hash. Off by one from the largest long, so the boundary is the
    // thing under test.
    CHECK_EQ(seedOf("9223372036854775808"), i64(-1773151197));
    CHECK_EQ(seedOf("-9223372036854775809"), i64(1304595159));
    CHECK_EQ(seedOf("99999999999999999999999"), i64(-1845299751));

    // Not numbers at all.
    CHECK_EQ(seedOf("1 2"), i64(48131));
    CHECK_EQ(seedOf("12a"), i64(48736));
    CHECK_EQ(seedOf("-"), i64(45));
    CHECK_EQ(seedOf("+"), i64(43));
}

TEST(the_hash_is_sign_extended_like_java_widening_an_int)
{
    // "gargamel" hashes negative, and `(long)"gargamel".hashCode()` keeps the
    // sign rather than becoming a large positive number.
    CHECK_EQ(seedOf("gargamel"), i64(-1623774494LL));
}

TEST(a_seed_above_the_basic_plane_hashes_as_a_surrogate_pair)
{
    // U+1F600. Java holds it as two UTF-16 units and hashes both -- the JVM
    // prints 1772899 for it -- so a decoder that treated it as one code point
    // would disagree with a PC.
    CHECK_EQ(javaStringHash("\xF0\x9F\x98\x80"), i32(1772899));
}

TEST(malformed_utf8_hashes_as_replacement_characters_rather_than_running_off)
{
    // A lone continuation byte and a truncated three-byte sequence. Neither is
    // text; both have to terminate.
    CHECK_EQ(javaStringHash("\x80"), i32(0xFFFD));
    // Two replacements, not one: the lead byte is consumed alone and the
    // orphaned continuation byte is then a second malformed sequence.
    CHECK_EQ(javaStringHash("\xE2\x82"), i32(31 * 0xFFFD + 0xFFFD));
}
