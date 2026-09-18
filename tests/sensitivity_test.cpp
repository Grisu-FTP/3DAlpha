// The Sensitivity row's curve and its labels, both of which are a1.1.2's.
//
// The claim worth a test is the one the whole design rests on: **100% is
// exactly 1.0**, so the touch and C-stick rates in `platform/ctr/main.cpp` --
// which were tuned on this console and not derived from anything -- are
// untouched by a player who never moves the row. See
// core/settings/sensitivity.hpp for where the curve was read off.

#include "framework.hpp"

#include "core/settings/sensitivity.hpp"

#include <cmath>
#include <string>

using namespace mc;
using namespace mc::settings;

namespace {

// The jar's arithmetic, written out here rather than called, so the test is a
// second statement of `iq.a(F)` and not a restatement of the implementation.
float jarGain(float sliderZeroToOne)
{
    const float f = sliderZeroToOne * 0.6f + 0.2f;
    return f * f * f * 8.0f;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-5f;
}

std::string label(int percent)
{
    char text[32];
    sensitivityLabel(percent, text, sizeof(text));
    return text;
}

}  // namespace

TEST(the_default_sensitivity_is_exactly_unity_so_the_tuned_look_rates_are_untouched)
{
    // `fr.c` defaults to 0.5, which the label prints as 100%, and
    // (0.5 * 0.6 + 0.2)^3 * 8 == 0.5^3 * 8 == 1. Not "about one": the whole
    // reason the port could adopt the jar's curve without retuning either look
    // device is that this lands on the identity.
    CHECK_EQ(kDefaultSensitivity, 100);
    CHECK(sensitivityGain(kDefaultSensitivity) == 1.0f);
}

TEST(the_sensitivity_curve_is_the_jars_at_every_step)
{
    for (int percent = kMinSensitivity; percent <= kMaxSensitivity;
         percent += kSensitivityStep) {
        const float slider = float(percent) / float(kMaxSensitivity);
        CHECK(near(sensitivityGain(percent), jarGain(slider)));
    }
}

TEST(the_sensitivity_ends_are_the_numbers_the_jars_curve_gives_them)
{
    // 0.2^3 * 8 and 0.8^3 * 8: the slider does not reach zero at either end,
    // which is what stops the bottom of it being a camera that cannot turn.
    CHECK(near(sensitivityGain(kMinSensitivity), 0.064f));
    CHECK(near(sensitivityGain(kMaxSensitivity), 4.096f));
}

TEST(the_sensitivity_curve_only_ever_rises)
{
    float previous = 0.0f;
    for (int percent = kMinSensitivity; percent <= kMaxSensitivity;
         percent += kSensitivityStep) {
        const float gain = sensitivityGain(percent);
        CHECK(gain > previous);
        previous = gain;
    }
}

TEST(a_hand_edited_sensitivity_is_clamped_and_snapped_to_a_step)
{
    // The file can be edited on a PC, and a value off the step grid would leave
    // the row unable to reach 100% again with Left and Right.
    CHECK_EQ(clampSensitivity(-40), kMinSensitivity);
    CHECK_EQ(clampSensitivity(9999), kMaxSensitivity);
    CHECK_EQ(clampSensitivity(97), 95);
    CHECK_EQ(clampSensitivity(98), 100);
    CHECK_EQ(clampSensitivity(100), 100);

    // Every step is already a step, so clamping is idempotent.
    for (int percent = kMinSensitivity; percent <= kMaxSensitivity;
         percent += kSensitivityStep) {
        CHECK_EQ(clampSensitivity(percent), percent);
    }
}

TEST(the_sensitivity_row_prints_a1_1_2s_own_two_named_ends)
{
    // `fr` returns these instead of a number when the slider is against either
    // stop, and they are the labels rather than a joke of this port's.
    CHECK_EQ(label(kMinSensitivity), std::string("*yawn*"));
    CHECK_EQ(label(kMaxSensitivity), std::string("HYPERSPEED!!!"));
    CHECK_EQ(label(100), std::string("100%"));
    CHECK_EQ(label(5), std::string("5%"));
    CHECK_EQ(label(195), std::string("195%"));

    // And a value out of an edited file is shown as what it will actually be.
    CHECK_EQ(label(-40), std::string("*yawn*"));
    CHECK_EQ(label(9999), std::string("HYPERSPEED!!!"));
}

TEST(a_sensitivity_step_moves_the_row_and_reaches_both_ends_and_the_default)
{
    // Left and Right walk the whole range without overshooting either stop, and
    // land on 100% on the way.
    CHECK_EQ(kMaxSensitivity % kSensitivityStep, 0);
    CHECK_EQ(kDefaultSensitivity % kSensitivityStep, 0);
}
