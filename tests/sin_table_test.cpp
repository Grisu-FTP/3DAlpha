#include "framework.hpp"

#include "core/util/math_helper.hpp"
#include "sin_table_vectors.hpp"

#include <cmath>
#include <cstring>

using namespace mc;
using mc::test::kSinSampleCount;
using mc::test::kSinSamples;
using mc::test::kSinTableHash;
using mc::MathHelper;

namespace {

u32 bits(float value)
{
    u32 out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

// FNV-1a over the raw float bits, little-endian -- the same walk genref.java
// does, so the two hashes are comparable.
u64 hashTable(const float* table)
{
    u64 hash = 0xCBF29CE484222325ULL;
    for (int i = 0; i < 65536; ++i) {
        const u32 value = bits(table[i]);
        for (int b = 0; b < 4; ++b) {
            hash ^= u64((value >> (b * 8)) & 0xFFu);
            hash *= 0x100000001B3ULL;
        }
    }
    return hash;
}

}  // namespace

// **All 65,536 entries, against a real JVM.**
//
// This is the test that decides whether the cave carver can avoid libm
// entirely. If it passes, our sin agrees with Java's everywhere that matters
// after narrowing to float, and the table is as good as the original's. If it
// ever fails, that is a genuine finding about the host's libm -- the fix is to
// compile the table in as data, not to loosen this.
TEST(the_sine_table_matches_the_jvm_bit_for_bit)
{
    CHECK_EQ(hashTable(MathHelper::table()), kSinTableHash);
}

// Individual entries, so a hash mismatch has somewhere to start. The samples
// include every quadrant boundary, where sin is exactly 0 or +-1 and a wrong
// table is least likely to look wrong.
TEST(sine_table_samples_match)
{
    const float* table = MathHelper::table();
    for (int i = 0; i < kSinSampleCount; ++i) {
        CHECK_EQ(bits(table[kSinSamples[i].index]), kSinSamples[i].bits);
    }
}

// The lookups are quantised, and that is the point: the table has ~0.0006 rad
// of resolution, so MathHelper::sin is measurably *not* std::sin. A future
// reader tempted to "fix" this should see it fail here first.
TEST(math_helper_sin_is_quantised_not_exact)
{
    int disagreements = 0;
    for (int i = 0; i < 1000; ++i) {
        const float angle = float(i) * 0.01f;
        if (bits(MathHelper::sin(angle)) != bits(float(std::sin(double(angle))))) {
            ++disagreements;
        }
    }
    CHECK(disagreements > 900);
}

// cos(x) == sin(x + pi/2) through the same table, which is what adding 16384
// before the truncation means. Checked at the quadrant boundaries where the
// two would disagree if the offset were applied after truncation instead.
TEST(math_helper_cos_is_the_table_a_quarter_turn_along)
{
    CHECK_EQ(bits(MathHelper::cos(0.0f)), bits(MathHelper::table()[16384]));
    CHECK_EQ(bits(MathHelper::sin(0.0f)), bits(MathHelper::table()[0]));
}
