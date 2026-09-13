// See particle_mesh.hpp.

#include "core/render/particle_mesh.hpp"

namespace mc::render {
namespace {

using entity::Particle;
using entity::ParticleKind;
using entity::ParticleLight;

// The detail position is a signed short of 1/1024 blocks, so a little under 32
// blocks either way of the origin.
constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

i16 uvOf(float sheetFraction)
{
    const float units = sheetFraction * float(mesh::kUvUnitsPerAtlas);
    return i16(units >= 0.0f ? units + 0.5f : units - 0.5f);
}

float clamp01(float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); }

// How far through its life a particle is, as the render overrides compute it:
// `(particleAge + partialTicks) / particleMaxAge`. **Unclamped**, because two
// of the three callers do not clamp it either -- a flame whose age has passed
// its maximum within a frame is drawn a hair smaller, not clipped.
float elapsed(const Particle& p, float partial)
{
    return p.maxAge > 0 ? (float(p.age) + partial) / float(p.maxAge) : 1.0f;
}

// One nibble of the light byte, lifted toward 15 by `toward`.
u8 fadeLevel(int level, float toward)
{
    const float lifted = float(level) + (15.0f - float(level)) * clamp01(toward);
    const int rounded = int(lifted + 0.5f);
    return u8(rounded > 15 ? 15 : (rounded < 0 ? 0 : rounded));
}

}  // namespace

float particleQuadScale(const Particle& p, float partial)
{
    switch (p.kind) {
    case ParticleKind::Smoke:
    case ParticleKind::LargeSmoke:
    case ParticleKind::Reddust:
        // `nl.a` and `en.a`: the fraction of life elapsed **times 32**, clamped
        // -- so a puff reaches full size in a thirty-second of its life and
        // holds it. What that thirty-second buys is a swell out of nothing
        // rather than a sprite appearing whole.
        return kParticleQuadScale * p.birthScale * clamp01(elapsed(p, partial) * 32.0f);

    case ParticleKind::Flame: {
        // `jb.a`: `1 - f * f * 0.5`, so a flame ends at half the size it
        // started and shrinks slowly at first.
        const float f = elapsed(p, partial);
        return kParticleQuadScale * p.birthScale * (1.0f - f * f * 0.5f);
    }

    case ParticleKind::Lava: {
        // `cq.a`: `1 - f * f`, all the way to nothing.
        const float f = elapsed(p, partial);
        return kParticleQuadScale * p.birthScale * (1.0f - f * f);
    }

    default:
        // The rest never rewrite `particleScale` after their constructor.
        return kParticleQuadScale * p.scale;
    }
}

u8 particleLight(const Particle& p, float partial)
{
    switch (p.lighting) {
    case ParticleLight::Full:
        // `cq.a(float)` is `return 1.0F`.
        return 0xFF;

    case ParticleLight::FadeFromFull: {
        // `jb.a(float)` is `world * f + (1 - f)` with `f` clamped. The
        // original's `world` is a float the quad's colour is multiplied by;
        // here it is a light *level* the shader turns into a colour through the
        // lightmap, so the blend is done on the two levels rather than on the
        // colour they produce. The endpoints are exact -- full bright at birth,
        // the world's own light at death -- and the middle is a shade off,
        // which is the price of letting the shader do the lighting at all.
        const float f = clamp01(elapsed(p, partial));
        const u8 sky = fadeLevel((p.light >> 4) & 0xF, 1.0f - f);
        const u8 block = fadeLevel(p.light & 0xF, 1.0f - f);
        return u8((sky << 4) | block);
    }

    case ParticleLight::World:
    default:
        return p.light;
    }
}

int buildParticles(const entity::ParticleSystem& system, const Billboard& camera,
                   double originX, double originY, double originZ, float partial,
                   entity::ParticleSheet sheet, mesh::DetailVertex* out, int max,
                   DrawCutoff* shared)
{
    if (out == nullptr || max < 4) {
        return 0;
    }

    // `prev + (pos - prev) * partialTicks`, the interpolation every entity is
    // drawn with, done in double and only then made relative. Shared by the
    // nearest-first count and the build, which must agree.
    //
    // **Whether a particle is drawn at all -- by any of the three passes.** The
    // sheet is deliberately *not* asked here: the cutoff is settled once over
    // the whole pool by whichever pass is handed the whole buffer, and a
    // `place` that refused the other two sheets would settle it over a third of
    // what is actually going to be drawn. The sheet filter lives in the write
    // loop below, which is where `buildItemEntities` keeps its own.
    const auto place = [&](int i, double* px, double* py, double* pz) {
        const Particle& p = system[i];
        *px = p.prevX + (p.x - p.prevX) * double(partial) - originX;
        *py = p.prevY + (p.y - p.prevY) * double(partial) - originY;
        *pz = p.prevZ + (p.z - p.prevZ) * double(partial) - originZ;
        return *px >= -kLimit && *px <= kLimit && *py >= -kLimit && *py <= kLimit
               && *pz >= -kLimit && *pz <= kLimit;
    };

    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    // A shared cutoff is settled by the pass that was handed the whole buffer,
    // exactly as the two item-entity sheets share theirs.
    DrawCutoff own;
    DrawCutoff& cutoff = shared != nullptr ? *shared : own;
    if (!cutoff.computed()) {
        if (system.count() * 4 > max) {
            cutoff.compute(system.count(), max, place, [](int) { return 4; });
        } else {
            cutoff.drawAll();
        }
    }

    // A quarter of a tile for the two sheets cut out of an image whose tiles
    // are a whole block or a whole icon; the tile whole for a sprite.
    const bool quartered = sheet != entity::ParticleSheet::Particles;
    const float span = quartered ? kParticleUvSpan : kParticleTileSpan;

    int written = 0;
    for (int i = 0; i < system.count() && written + 4 <= max; ++i) {
        const Particle& p = system[i];
        // **Which sheet owns this particle, asked before the budget is
        // charged.** The three sets are disjoint and the edge ring's remaining
        // room is shared, so between them the three passes take exactly what
        // the cutoff allowed -- the same arrangement `buildItemEntities` makes
        // with its two.
        if (p.sheet() != sheet) {
            continue;
        }
        double px = 0.0;
        double py = 0.0;
        double pz = 0.0;
        if (!place(i, &px, &py, &pz) || !cutoff.admit(px, py, pz, 4)) {
            continue;
        }

        const float size = particleQuadScale(p, partial);

        // `(tile % 16) / 16` and `(tile / 16) / 16`, plus the quarter-tile
        // jitter where there is one. All three sheets are a 16 x 16 grid: the
        // block atlas because the mesher says so, `gui/items.png` because
        // `RenderItem` indexes it the same way, and `particles.png` because
        // `EntityFX.renderParticle` divides by 16 twice.
        const float tileU = float(p.tile % mesh::kAtlasTilesPerEdge);
        const float tileV = float(p.tile / mesh::kAtlasTilesPerEdge);
        const float uLo = (tileU + (quartered ? p.jitterU / 4.0f : 0.0f))
                          / float(mesh::kAtlasTilesPerEdge);
        const float vLo = (tileV + (quartered ? p.jitterV / 4.0f : 0.0f))
                          / float(mesh::kAtlasTilesPerEdge);
        const i16 u0 = uvOf(uLo);
        const i16 u1 = uvOf(uLo + span);
        const i16 v0 = uvOf(vLo);
        const i16 v1 = uvOf(vLo + span);

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
        // **The low u goes with the `-right` corners**, which is the pairing
        // `renderParticle` writes: corner 0 takes `(uLo, vHi)`, not `(uHi,
        // vHi)`. This used to be the other way round, which mirrored every
        // sprite horizontally -- invisible on a four-pixel chip of stone, not
        // invisible on a flame.
        const i16 cornerU[4] = {u0, u0, u1, u1};
        const i16 cornerV[4] = {v1, v0, v0, v1};

        const u8 light = particleLight(p, partial);

        for (int c = 0; c < 4; ++c) {
            mesh::DetailVertex& v = out[written++];
            v.x = toUnits(px + double(cornerX[c]));
            v.y = toUnits(py + double(cornerY[c]));
            v.z = toUnits(pz + double(cornerZ[c]));
            v.face = 0;
            v.u = cornerU[c];
            v.v = cornerV[c];
            v.r = p.red;
            v.g = p.green;
            v.b = p.blue;
            v.light = light;
        }
    }
    return written;
}

}  // namespace mc::render
