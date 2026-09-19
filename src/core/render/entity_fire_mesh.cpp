// See entity_fire_mesh.hpp.

#include "core/render/entity_fire_mesh.hpp"

#include "core/render/draw_budget.hpp"
#include "core/render/entity_range.hpp"
#include "core/texture/texture_fx.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr float kPi = 3.1415927f;

// 1/256 of a block, which reaches 125 blocks either way of the origin -- see
// the header. `kLimit` is what one sheet's corner may reach; how far an
// entity's flames are drawn at all is its own range, in `buildPool`.
constexpr double kUnits = double(kEntityUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

// `float f1 = 1.4F` -- a sheet is taller than the unit it is stepped by, which
// is what overlaps it with the one above.
constexpr float kSheetHeight = 1.4f;

// `f6 *= 0.9F` per sheet, and the +x edge is the only one that moves.
constexpr float kSheetShrink = 0.9f;

// `-0.4F + (int)f9 * 0.02F`, then `-0.04F` a sheet: how far toward the camera
// the stack sits, in the same scaled units as everything else here.
constexpr float kStackDepth = -0.4f;
constexpr float kDepthPerRatio = 0.02f;
constexpr float kDepthPerSheet = -0.04f;

}  // namespace

FireFacing fireFacing(float viewYawDegrees)
{
    // `glRotatef(-playerViewY, 0, 1, 0)`. A rotation about +y by this angle
    // takes local +x to `(cos, 0, -sin)` and local +z to `(sin, 0, cos)` --
    // the same basis `buildItemEntities` builds from `180 - playerViewY`.
    const float turn = -viewYawDegrees / 180.0f * kPi;
    return FireFacing{MathHelper::sin(turn), MathHelper::cos(turn)};
}

int entityFireLayers(float width, float height)
{
    // The loop tests the ratio before its first pass, so an entity with no
    // height gets no sheets -- and one with no width would divide by zero and
    // spin, which is what the cap below is for.
    if (!(height > 0.0f)) {
        return 0;
    }
    if (!(width > 0.0f)) {
        return 1;
    }
    // `while (f9 > 0) { ...; f9 -= 1; }` is `ceil(height / width)` iterations,
    // and a ratio at or below one still draws its first sheet.
    float ratio = height / width;
    int layers = 0;
    while (ratio > 0.0f && layers < kEntityFireMaxLayers) {
        ++layers;
        ratio -= 1.0f;
    }
    return layers;
}

int buildEntityFire(double relX, double relY, double relZ, float width, float height,
                    int tile, FireFacing facing, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < 4 || tile < 0 || tile >= mesh::kAtlasTileCount
        || !(width > 0.0f)) {
        return 0;
    }

    const float scale = width * kEntityFireScale;
    const int layers = entityFireLayers(width, height);

    // The tile whole, inset an eighth of a texel like every other emitter --
    // the original's `(j + 15.99F) / 256F` is the same idea a little coarser.
    const int column = tile % mesh::kAtlasTilesPerEdge;
    const int row = tile / mesh::kAtlasTilesPerEdge;
    const i16 u0 = mesh::tileUvMin(column);
    const i16 u1 = mesh::tileUvMax(column);
    const i16 v0 = mesh::tileUvMin(row);
    const i16 v1 = mesh::tileUvMax(row);

    // **`(int)f9` and the sheet count disagree, and both are right.** The
    // depth term truncates the ratio and the loop takes its ceiling, so a
    // zombie gets three sheets placed as though it had two -- and it is worse
    // than that: `1.8F / 0.6F` is 2.99999976, so the truncation gives 2 rather
    // than the 3 the arithmetic on paper says. Java divides the same two floats
    // and gets the same quotient, so this is the original's own depth.
    float depth = kStackDepth + float(int(height / width)) * kDepthPerRatio;
    float shrink = 1.0f;

    int written = 0;
    for (int layer = 0; layer < layers && written + 4 <= max; ++layer) {
        const float x0 = -0.5f;
        const float x1 = shrink - 0.5f;
        const float y0 = float(layer);
        const float y1 = kSheetHeight + float(layer);

        // The original's own corner order: bottom right, bottom left, top left,
        // top right, with the low u on the two left corners and the tile's
        // bottom row on the two at y0.
        const float cornerX[4] = {x1, x0, x0, x1};
        const float cornerY[4] = {y0, y0, y1, y1};
        const i16 cornerU[4] = {u1, u0, u0, u1};
        const i16 cornerV[4] = {v1, v1, v0, v0};

        // Scale, then the yaw-only rotation, then the entity's position -- a
        // uniform scale commutes with the rotation, so the order the GL matrix
        // stack composes them in does not change the result.
        double cx[4];
        double cy[4];
        double cz[4];
        bool fits = true;
        for (int c = 0; c < 4; ++c) {
            const float lx = cornerX[c] * scale;
            const float ly = cornerY[c] * scale;
            const float lz = depth * scale;
            cx[c] = relX + double(lx * facing.cosYaw + lz * facing.sinYaw);
            cy[c] = relY + double(ly);
            cz[c] = relZ + double(lz * facing.cosYaw - lx * facing.sinYaw);
            fits = fits && cx[c] >= -kLimit && cx[c] <= kLimit && cy[c] >= -kLimit
                   && cy[c] <= kLimit && cz[c] >= -kLimit && cz[c] <= kLimit;
        }

        // **A sheet the position cannot express is dropped, not clamped**, the
        // same call `buildParticles` makes for a particle out of range -- and
        // the *sheet* rather than the stack, because the caller's own range
        // check is on the entity's feet: a mob near the top of the window has
        // its high sheets outside it and a mob near the bottom has its low
        // ones, so a `break` here would lose the wrong half.
        if (!fits) {
            shrink *= kSheetShrink;
            depth += kDepthPerSheet;
            continue;
        }

        for (int c = 0; c < 4; ++c) {
            mesh::DetailVertex& v = out[written++];
            v.x = toUnits(cx[c]);
            v.y = toUnits(cy[c]);
            v.z = toUnits(cz[c]);
            v.face = 0;
            v.u = cornerU[c];
            v.v = cornerV[c];
            v.r = 0xFF;
            v.g = 0xFF;
            v.b = 0xFF;
            v.light = 0xFF;
        }

        shrink *= kSheetShrink;
        depth += kDepthPerSheet;
    }
    return written;
}

namespace {

// One burning entity, reduced to what a sheet stack needs: where it is
// relative to the caller's origin, and how big its box is. `alight` is the
// pool's own `fire > 0`.
struct Burning {
    double x, y, z;
    float width, height;
};

// The loop every pool below runs. `at(i, &out)` answers whether entity `i` is
// alight and fills in where it is; everything else -- the range check, the
// cutoff and the sheets -- is the same for all six.
template <class At>
int buildPool(int count, At at, double originX, double originY, double originZ, int tile,
              FireFacing facing, mesh::DetailVertex* out, int max, DrawCutoff* cutoff)
{
    int written = 0;
    for (int i = 0; i < count && written + 4 <= max; ++i) {
        Burning b{};
        if (!at(i, &b)) {
            continue;
        }
        const double px = b.x - originX;
        const double py = b.y - originY;
        const double pz = b.z - originZ;
        // The same `kh.a(D)Z` the entity's own pass admits it by.
        if (!entityInDrawRange(px, py, pz, b.width, b.height)) {
            continue;
        }
        const int cost = entityFireLayers(b.width, b.height) * 4;
        if (!cutoff->admit(px, py, pz, cost)) {
            continue;
        }
        written += buildEntityFire(px, py, pz, b.width, b.height, tile, facing,
                                   out + written, max - written);
    }
    return written;
}

// `prev + (pos - prev) * partialTicks`, which every pool interpolates the same
// way and which must agree with the pass that draws the entity itself.
double lerp(double prev, double now, float partial)
{
    return prev + (now - prev) * double(partial);
}

}  // namespace

int buildEntityFires(const FireScene& scene, float viewYawDegrees, double originX,
                     double originY, double originZ, float partial,
                     mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < 4) {
        return 0;
    }

    // **Asked of the block table, not of an id**: whichever block renders as
    // fire owns the tile, and a version without one draws no entity fire
    // either. It is the same tile `FlameAnimation` animates.
    const int tile = texture::flameTile(0);
    if (tile < 0) {
        return 0;
    }

    const FireFacing facing = fireFacing(viewYawDegrees);

    // **One cutoff across every pool**, settled lazily: nothing here knows how
    // many entities are alight until it has looked, and the common case is
    // none at all. `drawAll` is the honest answer for a buffer this size --
    // 32 burning entities at once is a fire in a storeroom, and past that the
    // pools are simply taken in order until the buffer is full.
    DrawCutoff cutoff;
    cutoff.drawAll();

    int written = 0;

    if (scene.mobs != nullptr) {
        const entity::MobSystem& pool = *scene.mobs;
        written += buildPool(
            pool.count(),
            [&](int i, Burning* b) {
                const entity::Mob& m = pool[i];
                if (!m.alive || m.fire <= 0) {
                    return false;
                }
                // A mob's `yOffset` is zero, so this is its feet -- the same
                // origin `buildMobs` poses the model from.
                b->x = m.body.renderX(partial);
                b->y = m.body.renderEyeY(partial);
                b->z = m.body.renderZ(partial);
                b->width = m.body.width;
                b->height = m.body.height;
                return true;
            },
            originX, originY, originZ, tile, facing, out + written, max - written, &cutoff);
    }

    if (scene.items != nullptr) {
        const entity::ItemEntitySystem& pool = *scene.items;
        written += buildPool(
            pool.count(),
            [&](int i, Burning* b) {
                const entity::ItemEntity& e = pool[i];
                if (!e.alive() || e.fire <= 0) {
                    return false;
                }
                b->x = lerp(e.prevX, e.x, partial);
                b->y = lerp(e.prevY, e.y, partial);
                b->z = lerp(e.prevZ, e.z, partial);
                b->width = float(entity::kItemSize);
                b->height = float(entity::kItemSize);
                return true;
            },
            originX, originY, originZ, tile, facing, out + written, max - written, &cutoff);
    }

    if (scene.boats != nullptr) {
        const entity::BoatSystem& pool = *scene.boats;
        written += buildPool(
            pool.count(),
            [&](int i, Burning* b) {
                const entity::Boat& e = pool[i];
                if (!e.alive || e.fire <= 0) {
                    return false;
                }
                b->x = lerp(e.prevX, e.x, partial);
                b->y = lerp(e.prevY, e.y, partial);
                b->z = lerp(e.prevZ, e.z, partial);
                b->width = float(entity::kBoatWidth);
                b->height = float(entity::kBoatYOffset * 2.0);
                return true;
            },
            originX, originY, originZ, tile, facing, out + written, max - written, &cutoff);
    }

    if (scene.minecarts != nullptr) {
        const entity::MinecartSystem& pool = *scene.minecarts;
        written += buildPool(
            pool.count(),
            [&](int i, Burning* b) {
                const entity::Minecart& e = pool[i];
                if (!e.alive || e.fire <= 0) {
                    return false;
                }
                b->x = lerp(e.prevX, e.x, partial);
                b->y = lerp(e.prevY, e.y, partial);
                b->z = lerp(e.prevZ, e.z, partial);
                b->width = float(entity::kMinecartWidth);
                b->height = float(entity::kMinecartYOffset * 2.0);
                return true;
            },
            originX, originY, originZ, tile, facing, out + written, max - written, &cutoff);
    }

    if (scene.fallingBlocks != nullptr) {
        const entity::FallingBlockSystem& pool = *scene.fallingBlocks;
        written += buildPool(
            pool.count(),
            [&](int i, Burning* b) {
                const entity::FallingBlock& e = pool[i];
                if (!e.alive() || e.fire <= 0) {
                    return false;
                }
                b->x = lerp(e.prevX, e.x, partial);
                b->y = lerp(e.prevY, e.y, partial);
                b->z = lerp(e.prevZ, e.z, partial);
                b->width = float(entity::kFallingBlockSize);
                b->height = float(entity::kFallingBlockSize);
                return true;
            },
            originX, originY, originZ, tile, facing, out + written, max - written, &cutoff);
    }

    if (scene.primedTnt != nullptr) {
        const entity::PrimedTntSystem& pool = *scene.primedTnt;
        written += buildPool(
            pool.count(),
            [&](int i, Burning* b) {
                const entity::PrimedTnt& e = pool[i];
                if (!e.alive() || e.fire <= 0) {
                    return false;
                }
                b->x = lerp(e.prevX, e.x, partial);
                b->y = lerp(e.prevY, e.y, partial);
                b->z = lerp(e.prevZ, e.z, partial);
                b->width = float(entity::kPrimedTntSize);
                b->height = float(entity::kPrimedTntSize);
                return true;
            },
            originX, originY, originZ, tile, facing, out + written, max - written, &cutoff);
    }

    return written;
}

}  // namespace mc::render
