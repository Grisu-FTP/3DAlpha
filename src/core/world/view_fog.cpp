// See view_fog.hpp.

#include "core/world/view_fog.hpp"

namespace mc::world {

// iq.a()V
//
//   n = o;
//   float f = world.getLightBrightness(floor(posX), floor(posY), floor(posZ));
//   float f1 = (3 - mc.gameSettings.renderDistance) / 3.0F;
//   float f2 = f * (1.0F - f1) + f1;
//   o += (f2 - o) * 0.1F;
void FogBrightness::tick(float lightBrightness, int renderDistanceChunks)
{
    previous = current;
    const float lift = (3.0f - renderDistanceOption(renderDistanceChunks)) / 3.0f;
    const float target = lightBrightness * (1.0f - lift) + lift;
    current += (target - current) * 0.1f;
}

// iq.i(F)V, from the point the sky lerp is done:
//
//   if (player.isInsideOfMaterial(Material.water)) { e = 0.02F; f = 0.02F; g = 0.2F; }
//   else if (player.isInsideOfMaterial(Material.lava)) { e = 0.6F; f = 0.1F; g = 0.0F; }
//   float f5 = n + (o - n) * partialTicks;
//   e *= f5; f *= f5; g *= f5;
ViewFog viewFog(i64 timeTicks, float partialTicks, int renderDistanceChunks, FogMedium medium,
                float brightness)
{
    ViewFog out{};
    switch (medium) {
    case FogMedium::Water:
        out.colour = SkyColour{0.02f, 0.02f, 0.2f};
        out.density = kWaterFogDensity;
        break;
    case FogMedium::Lava:
        out.colour = SkyColour{0.6f, 0.1f, 0.0f};
        out.density = kLavaFogDensity;
        break;
    case FogMedium::Air:
        out.colour = viewFogColour(timeTicks, partialTicks, renderDistanceChunks);
        out.density = 0.0f;
        break;
    }
    out.colour.r *= brightness;
    out.colour.g *= brightness;
    out.colour.b *= brightness;
    return out;
}

}  // namespace mc::world
