#include "framework.hpp"

#include "core/world/daylight.hpp"

#include <cmath>

using namespace mc;

// The whole point of these is that the numbers came out of the jar rather than
// out of anyone's idea of what dusk should look like. They are transcriptions,
// so what they need pinning against is the transcription being edited later.

TEST(the_brightness_table_matches_the_loop_that_builds_it)
{
    // cn.<clinit>, exactly:
    //   float f = 0.05f;
    //   float f1 = 1.0f - j / 15.0f;
    //   table[j] = (1.0f - f1) / (f1 * 3.0f + 1.0f) * (1.0f - f) + f;
    const float ambient = 0.05f;
    for (int j = 0; j <= 15; ++j) {
        const float f1 = 1.0f - float(j) / 15.0f;
        const float want = (1.0f - f1) / (f1 * 3.0f + 1.0f) * (1.0f - ambient) + ambient;
        CHECK(std::fabs(world::lightBrightness(j) - want) < 1e-5f);
    }

    // The two ends are the ones a bug would show up at: caves must not be pure
    // black, and full daylight must be exactly full.
    CHECK(std::fabs(world::lightBrightness(0) - 0.05f) < 1e-6f);
    CHECK(std::fabs(world::lightBrightness(15) - 1.0f) < 1e-6f);

    // Out of range clamps rather than reading off the end.
    CHECK_EQ(world::lightBrightness(-3), world::lightBrightness(0));
    CHECK_EQ(world::lightBrightness(99), world::lightBrightness(15));
}

TEST(alpha_holds_full_daylight_for_half_the_day)
{
    // This is the bug the curve replaced. A squared sine peaks at noon and is
    // at a quarter brightness at dawn; Alpha is flat out until tick 12040.
    // A world saved at Time=412 -- the real test world -- must render at full
    // brightness, not at a third of it.
    CHECK_EQ(world::skyLightSubtracted(0), 0);
    CHECK_EQ(world::skyLightSubtracted(412), 0);
    CHECK_EQ(world::skyLightSubtracted(6000), 0);
    CHECK_EQ(world::skyLightSubtracted(12040), 0);

    CHECK_EQ(world::effectiveLightLevel(15, 0, world::skyLightSubtracted(412)), 15);
    CHECK(std::fabs(world::lightBrightness(15) - 1.0f) < 1e-6f);
}

TEST(dusk_steps_down_eleven_times_and_dawn_climbs_back)
{
    // The exact ticks each step lands on. Derived, and worth pinning precisely:
    // they are what makes dusk take about eighty seconds rather than being
    // instant or taking an hour.
    CHECK_EQ(world::skyLightSubtracted(12041), 1);
    CHECK_EQ(world::skyLightSubtracted(12866), 6);
    CHECK_EQ(world::skyLightSubtracted(13670), 11);

    // Night is a plateau, not a continuing fade.
    CHECK_EQ(world::skyLightSubtracted(18000), 11);
    CHECK_EQ(world::skyLightSubtracted(22330), 11);

    CHECK_EQ(world::skyLightSubtracted(22331), 10);
    CHECK_EQ(world::skyLightSubtracted(23960), 0);

    // Never outside the range the lightmap is sized for.
    for (i64 t = 0; t < 24000; ++t) {
        const int s = world::skyLightSubtracted(t);
        CHECK(s >= 0 && s <= 11);
    }
}

TEST(the_day_is_monotonic_down_then_up)
{
    // One fall and one rise, no wobble. A transcription slip in the cosine ease
    // shows up here as extra turning points.
    int falls = 0;
    int rises = 0;
    for (i64 t = 1; t < 24000; ++t) {
        const int a = world::skyLightSubtracted(t - 1);
        const int b = world::skyLightSubtracted(t);
        if (b > a) {
            ++falls;
        }
        if (b < a) {
            ++rises;
        }
    }
    CHECK_EQ(falls, 11);
    CHECK_EQ(rises, 11);
}

TEST(night_is_dim_but_not_black_and_caves_ignore_the_sun)
{
    const int night = world::skyLightSubtracted(18000);

    // Sky 15 at midnight lands on level 4, which is the original's night.
    CHECK_EQ(world::effectiveLightLevel(15, 0, night), 4);
    CHECK(std::fabs(world::lightBrightness(4) - 0.12916666f) < 1e-6f);

    // Block light is not touched by the sun: a torch is a torch at midnight.
    CHECK_EQ(world::effectiveLightLevel(0, 14, night), 14);
    CHECK_EQ(world::effectiveLightLevel(0, 14, 0), 14);

    // And the brighter of the two wins.
    CHECK_EQ(world::effectiveLightLevel(15, 7, 0), 15);
    CHECK_EQ(world::effectiveLightLevel(15, 7, night), 7);
}

TEST(a_negative_or_huge_stored_time_stays_in_range)
{
    // level.dat is not ours to trust; a level written by something other than
    // the original must not index the lightmap out of bounds.
    for (i64 t : {i64(-1), i64(-24001), i64(1), i64(1LL << 40), i64(-(1LL << 40))}) {
        const int s = world::skyLightSubtracted(t);
        CHECK(s >= 0 && s <= 11);
        CHECK(world::effectiveLightLevel(15, 0, s) >= 0);
        CHECK(world::effectiveLightLevel(15, 0, s) <= 15);
    }
}
