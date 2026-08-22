#include "core/world/daylight.hpp"

#include <cmath>

namespace mc::world {

namespace {

constexpr float kPi = 3.1415927f;  // the jar's own literal, not M_PI

// cn.<clinit>: 16 entries, ambient 0.05, built once.
//
//   float f = 0.05f;
//   for (int j = 0; j <= 15; ++j) {
//       float f1 = 1.0f - j / 15.0f;
//       table[j] = (1.0f - f1) / (f1 * 3.0f + 1.0f) * (1.0f - f) + f;
//   }
//
// Spelled out rather than computed at startup so it is inspectable, and
// pinned against that loop by tests/daylight_test.cpp.
constexpr float kBrightness[16] = {
    0.05000000f, 0.06666667f, 0.08518519f, 0.10588235f,
    0.12916666f, 0.15555556f, 0.18571429f, 0.22051282f,
    0.26111111f, 0.30909091f, 0.36666667f, 0.43703705f,
    0.52499998f, 0.63809526f, 0.78888887f, 1.00000000f,
};

}  // namespace

// cn.c(F)F
//
//   int i = (int)(time % 24000L);
//   float f = ((float)i + partialTicks) / 24000.0f - 0.25f;
//   if (f < 0.0f) f += 1.0f;
//   if (f > 1.0f) f -= 1.0f;
//   float f1 = f;
//   f = 1.0f - (float)((Math.cos(f * Math.PI) + 1.0) / 2.0);
//   return f1 + (f - f1) / 3.0f;
float celestialAngle(i64 timeTicks, float partialTicks)
{
    // Java's % keeps the sign of the dividend and so does C++'s, so a negative
    // stored time behaves the same in both. It should not happen, but a level
    // written by something other than the original is not ours to trust.
    const int ticks = int(timeTicks % 24000);

    float f = (float(ticks) + partialTicks) / 24000.0f - 0.25f;
    if (f < 0.0f) {
        f += 1.0f;
    }
    if (f > 1.0f) {
        f -= 1.0f;
    }

    const float linear = f;
    // The cosine ease, computed in double exactly as the jar does -- the result
    // feeds an integer threshold below, so where it rounds is observable.
    f = 1.0f - float((std::cos(double(f) * 3.141592653589793) + 1.0) / 2.0);
    return linear + (f - linear) / 3.0f;
}

// cn.a(F)I
//
//   float f = getCelestialAngle(partialTicks);
//   float f1 = 1.0f - (MathHelper.cos(f * PI * 2.0f) * 2.0f + 0.5f);
//   if (f1 < 0.0f) f1 = 0.0f;
//   if (f1 > 1.0f) f1 = 1.0f;
//   return (int)(f1 * 11.0f);
//
// MathHelper.cos is a 65536-entry sine table (eo.<clinit>), so the original is
// slightly quantised where this is exact. The difference is under 1e-4 and the
// thresholds it could move are the tick a step lands on, not the shape.
int skyLightSubtracted(i64 timeTicks, float partialTicks)
{
    const float angle = celestialAngle(timeTicks, partialTicks);

    float f = 1.0f - (std::cos(angle * kPi * 2.0f) * 2.0f + 0.5f);
    if (f < 0.0f) {
        f = 0.0f;
    }
    if (f > 1.0f) {
        f = 1.0f;
    }
    return int(f * 11.0f);
}

float lightBrightness(int level)
{
    const int clamped = level < 0 ? 0 : (level > 15 ? 15 : level);
    return kBrightness[clamped];
}

}  // namespace mc::world
