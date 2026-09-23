#include "core/world/daylight.hpp"

#include "core/util/math_helper.hpp"
#include "core/util/strict_math.hpp"

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

// The one expression all three colours share:
//
//     float f = MathHelper.cos(celestialAngle * PI * 2) * 2 + 0.5;
//     if (f < 0) f = 0;
//     if (f > 1) f = 1;
//
// **MathHelper.cos and not std::cos**, unlike skyLightSubtracted above, and the
// difference between the two calls is the point. That one feeds `(int)(f * 11)`,
// where a table's 1e-4 of error decides which tick a visible step lands on and
// the exact answer is the better one. This one feeds a colour byte, where 1e-4
// is a fortieth of one 8-bit step -- so here the table is simply what the
// original does, at no cost worth weighing.
float daylightFraction(i64 timeTicks, float partialTicks)
{
    const float angle = celestialAngle(timeTicks, partialTicks);
    const float f = MathHelper::cos(angle * kPi * 2.0f) * 2.0f + 0.5f;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// One channel of a packed 0xRRGGBB base colour, as the jar unpacks it: shift,
// mask, widen, divide by 255.
constexpr float channel(i32 packed, int shift)
{
    return float((packed >> shift) & 255) / 255.0f;
}

// cn.D, cn.E: the two base colours, straight out of the constructor.
constexpr i32 kSkyBase = 8961023;   // 0x88BBFF
constexpr i32 kFogBase = 12638463;  // 0xC0D8FF

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

// cn.b(F)Laj
//
//   float f1 = MathHelper.cos(getCelestialAngle(f) * PI * 2) * 2 + 0.5;  clamped
//   float f2 = (skyColor >> 16 & 255) / 255F;   ... and the same for g and b
//   f2 *= f1; f3 *= f1; f4 *= f1;
//   return Vec3D.createVector(f2, f3, f4);
SkyColour skyColour(i64 timeTicks, float partialTicks)
{
    const float f = daylightFraction(timeTicks, partialTicks);
    return SkyColour{channel(kSkyBase, 16) * f, channel(kSkyBase, 8) * f,
                     channel(kSkyBase, 0) * f};
}

// cn.e(F)Laj -- the same shape, over cn.E, with a floor under each channel so
// the fog never reaches black.
SkyColour fogColour(i64 timeTicks, float partialTicks)
{
    const float f = daylightFraction(timeTicks, partialTicks);
    return SkyColour{channel(kFogBase, 16) * (f * 0.94f + 0.06f),
                     channel(kFogBase, 8) * (f * 0.94f + 0.06f),
                     channel(kFogBase, 0) * (f * 0.91f + 0.09f)};
}

// cn.f(F)F
//
//   float f1 = 1 - (MathHelper.cos(getCelestialAngle(f) * PI * 2) * 2 + 0.75F);
//   clamped to 0..1, then f1 * f1 * 0.5F
//
// Note the 0.75 where the other three use 0.5: the stars are still invisible
// when the sky has already begun to dim, which is the gap dusk happens in.
float starBrightness(i64 timeTicks, float partialTicks)
{
    const float angle = celestialAngle(timeTicks, partialTicks);
    float f = 1.0f - (MathHelper::cos(angle * kPi * 2.0f) * 2.0f + 0.75f);
    if (f < 0.0f) {
        f = 0.0f;
    }
    if (f > 1.0f) {
        f = 1.0f;
    }
    return f * f * 0.5f;
}

// iq.h(F)V, the part of updateFogColor that is not water, lava or the blindness
// this version does not have:
//
//   float f1 = 1.0F / (float)(4 - mc.gameSettings.renderDistance);
//   f1 = 1.0F - (float)Math.pow(f1, 0.25D);
//   Vec3D vec3d = world.getSkyColor(f);
//   Vec3D vec3d1 = world.getFogColor(f);
//   fogColorRed   = vec3d1.xCoord + (vec3d.xCoord - vec3d1.xCoord) * f1;   ...
//
// **Two transcendentals, and neither of them has to match Java bit for bit.**
// The pow is the original's own `Math.pow`, which is a platform call on the JVM
// as well; the log is this port's, because the original's `renderDistance` is
// one of four settings where this one is a chunk count. Nothing downstream is a
// seeded stream or an integer threshold -- the result is a colour channel, and
// two libm implementations differ by far less than one 8-bit step. The log
// still goes through strictmath, for the cheaper reason: host and console then
// agree exactly, so a test vector taken on one holds on the other.
float renderDistanceOption(int renderDistanceChunks)
{
    // 256 >> renderDistance is the original's far plane in blocks, so a chunk
    // count of 16, 8, 4 or 2 is settings 0, 1, 2 and 3. log2 of a non-power of
    // two lands between them, which is where a render distance of 6 belongs.
    const int chunks = renderDistanceChunks < 1 ? 1 : renderDistanceChunks;
    float option = 4.0f - float(strictmath::log(double(chunks)) / 0.6931471805599453);
    if (option < 0.0f) {
        option = 0.0f;
    }
    if (option > 3.0f) {
        option = 3.0f;
    }
    return option;
}

SkyColour viewFogColour(i64 timeTicks, float partialTicks, int renderDistanceChunks)
{
    const float option = renderDistanceOption(renderDistanceChunks);
    const float scale = 1.0f - float(std::pow(1.0 / double(4.0f - option), 0.25));
    const SkyColour sky = skyColour(timeTicks, partialTicks);
    const SkyColour fog = fogColour(timeTicks, partialTicks);
    return SkyColour{fog.r + (sky.r - fog.r) * scale, fog.g + (sky.g - fog.g) * scale,
                     fog.b + (sky.b - fog.b) * scale};
}

}  // namespace mc::world
