// See particle_mesh.hpp.

#include "core/render/particle_mesh.hpp"

#include "core/render/draw_budget.hpp"

namespace mc::render {
namespace {

// The detail position is a signed short of 1/1024 blocks, so a little under 32
// blocks either way of the origin.
constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

// **0.6 on all three channels**, which is `EntityDiggingFX`'s
// `particleRed = particleGreen = particleBlue = 0.6F`: every digging particle
// is a darkened copy of the block's texture, which is what stops a cloud of
// them reading as brighter than the block it came off.
constexpr u8 kDiggingShade = u8(0.6f * 255.0f);

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

i16 uvOf(float atlasFraction)
{
    const float units = atlasFraction * float(mesh::kUvUnitsPerAtlas);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

}  // namespace

int buildParticles(const entity::ParticleSystem& system, const Billboard& camera,
                   double originX, double originY, double originZ, float partial,
                   mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < 4) {
        return 0;
    }

    // `prev + (pos - prev) * partialTicks`, the interpolation every entity is
    // drawn with, done in double and only then made relative. Shared by the
    // nearest-first count and the build, which must agree.
    const auto place = [&](int i, double* px, double* py, double* pz) {
        const entity::Particle& p = system[i];
        *px = p.prevX + (p.x - p.prevX) * double(partial) - originX;
        *py = p.prevY + (p.y - p.prevY) * double(partial) - originY;
        *pz = p.prevZ + (p.z - p.prevZ) * double(partial) - originZ;
        return *px >= -kLimit && *px <= kLimit && *py >= -kLimit && *py <= kLimit
               && *pz >= -kLimit && *pz <= kLimit;
    };
    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    DrawCutoff cutoff;
    if (system.count() * 4 > max) {
        cutoff.compute(system.count(), max, place, [](int) { return 4; });
    }

    int written = 0;
    for (int i = 0; i < system.count() && written + 4 <= max; ++i) {
        const entity::Particle& p = system[i];
        double px = 0.0;
        double py = 0.0;
        double pz = 0.0;
        if (!place(i, &px, &py, &pz) || !cutoff.admit(px, py, pz, 4)) {
            continue;
        }

        const float size = kParticleQuadScale * p.scale;

        // The quarter-tile this fleck came from. `jitter` is 0..3 in quarters,
        // so `tile + jitter/4` walks across the tile in sixteenths of the
        // atlas -- and the *high* u is written first, which is the original's
        // winding and not a transposition here.
        const float uLo = (float(p.tile % mesh::kAtlasTilesPerEdge) + p.jitterU / 4.0f)
                          / float(mesh::kAtlasTilesPerEdge);
        const float vLo = (float(p.tile / mesh::kAtlasTilesPerEdge) + p.jitterV / 4.0f)
                          / float(mesh::kAtlasTilesPerEdge);
        const i16 u0 = uvOf(uLo);
        const i16 u1 = uvOf(uLo + kParticleUvSpan);
        const i16 v0 = uvOf(vLo);
        const i16 v1 = uvOf(vLo + kParticleUvSpan);

        // right * ±size and up * ±size, the four corners in the original's
        // order: bottom-left, top-left, top-right, bottom-right.
        const float rx = camera.rightX * size;
        const float ry = camera.rightY * size;
        const float rz = camera.rightZ * size;
        const float ux = camera.upX * size;
        const float uy = camera.upY * size;
        const float uz = camera.upZ * size;

        const float cornerX[4] = {-rx - ux, -rx + ux, rx + ux, rx - ux};
        const float cornerY[4] = {-ry - uy, -ry + uy, ry + uy, ry - uy};
        const float cornerZ[4] = {-rz - uz, -rz + uz, rz + uz, rz - uz};
        const i16 cornerU[4] = {u1, u1, u0, u0};
        const i16 cornerV[4] = {v1, v0, v0, v1};

        for (int c = 0; c < 4; ++c) {
            mesh::DetailVertex& v = out[written++];
            v.x = toUnits(px + double(cornerX[c]));
            v.y = toUnits(py + double(cornerY[c]));
            v.z = toUnits(pz + double(cornerZ[c]));
            v.face = 0;
            v.u = cornerU[c];
            v.v = cornerV[c];
            v.r = kDiggingShade;
            v.g = kDiggingShade;
            v.b = kDiggingShade;
            v.light = p.light;
        }
    }
    return written;
}

}  // namespace mc::render
