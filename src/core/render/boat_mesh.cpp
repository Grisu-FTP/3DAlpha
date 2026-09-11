// See boat_mesh.hpp. `cl`'s constructor and `cp.a(Ldc;DDDFF)V`, transcribed.

#include "core/render/boat_mesh.hpp"

#include "core/render/box_model.hpp"
#include "core/render/draw_budget.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

constexpr float kPi = 3.1415927f;
constexpr float kDegrees = kPi / 180.0f;

// `cl`'s four dimensions, named as the constructor names them.
constexpr int kHullWidth = 24;   // i
constexpr int kSideHeight = 6;   // j
constexpr int kHullLength = 20;  // k
constexpr int kDeck = 4;         // l

// The five parts, built once. `cl` makes box 0 with texture offset (0, 8) and
// the other four with (0, 0) -- the four sides share one UV rectangle because
// they are the same box four times.
const ModelPart& boatPart(int index)
{
    static const ModelPart parts[kBoatParts] = {
        // The hull: a wide flat plate laid down by a quarter turn about x.
        {float(-kHullWidth / 2), float(-kHullLength / 2 + 2), -3.0f, kHullWidth,
         kHullLength - 4, 4, 0.0f, 0, 8, false, 0.0f, float(kDeck), 0.0f, kPi / 2.0f, 0.0f,
         0.0f},
        // Port side.
        {float(-kHullWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kHullWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, float(-kHullWidth / 2 + 1), float(kDeck), 0.0f,
         0.0f, 3.0f * kPi / 2.0f, 0.0f},
        // Starboard side.
        {float(-kHullWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kHullWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, float(kHullWidth / 2 - 1), float(kDeck), 0.0f,
         0.0f, kPi / 2.0f, 0.0f},
        // Stern.
        {float(-kHullWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kHullWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, 0.0f, float(kDeck),
         float(-kHullLength / 2 + 1), 0.0f, kPi, 0.0f},
        // Bow.
        {float(-kHullWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kHullWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, 0.0f, float(kDeck), float(kHullLength / 2 - 1),
         0.0f, 0.0f, 0.0f},
    };
    return parts[index];
}

}  // namespace

int buildBoats(const entity::BoatSystem& system, double originX, double originY,
               double originZ, float partial, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kBoatVerticesEach) {
        return 0;
    }

    // Where a boat is drawn relative to the origin, and whether it is drawn at
    // all. Shared by the nearest-first count and the build, which must agree.
    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::Boat& b = system[index];
        if (!b.alive) {
            return false;
        }
        *rx = b.prevX + (b.x - b.prevX) * double(partial) - originX;
        *ry = b.prevY + (b.y - b.prevY) * double(partial) - originY;
        *rz = b.prevZ + (b.z - b.prevZ) * double(partial) - originZ;
        return *rx >= -kLimit && *rx <= kLimit && *ry >= -kLimit && *ry <= kLimit
               && *rz >= -kLimit && *rz <= kLimit;
    };
    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    DrawCutoff cutoff;
    if (system.count() * kBoatVerticesEach > max) {
        cutoff.compute(system.count(), max, place, [](int) { return kBoatVerticesEach; });
    }

    int written = 0;
    for (int index = 0; index < system.count(); ++index) {
        const entity::Boat& b = system[index];
        double rx = 0.0;
        double ry = 0.0;
        double rz = 0.0;
        if (!place(index, &rx, &ry, &rz) || !cutoff.admit(rx, ry, rz, kBoatVerticesEach)) {
            continue;
        }
        if (written + kBoatVerticesEach > max) {
            break;
        }

        // `glRotatef(180.0F - entityYaw, 0,1,0)`.
        const float yaw = (180.0f - (b.prevYaw + (b.yaw - b.prevYaw) * partial)) * kDegrees;

        // The damage rock about x. Both counters are zero in this build --
        // nothing attacks a boat -- so this is always zero, and it is here
        // because a boat that breaks on a wall already carries the fields.
        float rock = 0.0f;
        const float hit = float(b.timeSinceHit) - partial;
        float damage = float(b.damage) - partial;
        if (damage < 0.0f) {
            damage = 0.0f;
        }
        if (hit > 0.0f) {
            rock = MathHelper::sin(hit) * hit * damage / 10.0f * float(b.forwardDirection)
                   * kDegrees;
        }

        // **The (-1, -1, 1) flip folded into the axes.** Every Minecraft entity
        // model is authored upside down and mirrored, and `RenderBoat` undoes
        // it with a scale rather than by moving the numbers. Doing it here
        // keeps `buildBox` a pure transform and keeps the model table the class
        // file's.
        const float s = kModelUnit;
        float ax[3] = {-s, 0.0f, 0.0f};
        float ay[3] = {0.0f, -s, 0.0f};
        float az[3] = {0.0f, 0.0f, s};

        const float sinRock = MathHelper::sin(rock);
        const float cosRock = MathHelper::cos(rock);
        const float sinYaw = MathHelper::sin(yaw);
        const float cosYaw = MathHelper::cos(yaw);

        auto turn = [&](float* v) {
            // Rx(rock)
            const float y1 = v[1] * cosRock - v[2] * sinRock;
            const float z1 = v[1] * sinRock + v[2] * cosRock;
            // Ry(yaw)
            const float x2 = v[0] * cosYaw + z1 * sinYaw;
            const float z2 = -v[0] * sinYaw + z1 * cosYaw;
            v[0] = x2;
            v[1] = y1;
            v[2] = z2;
        };
        turn(ax);
        turn(ay);
        turn(az);

        Placement place;
        place.x = rx;
        place.y = ry;
        place.z = rz;
        for (int a = 0; a < 3; ++a) {
            place.ax[a] = ax[a];
            place.ay[a] = ay[a];
            place.az[a] = az[a];
        }

        for (int part = 0; part < kBoatParts; ++part) {
            written += buildBox(boatPart(part), place, texture::EntitySkin::Boat, b.light,
                                out + written, max - written);
        }
    }
    return written;
}

}  // namespace mc::render
