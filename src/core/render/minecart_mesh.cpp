// See minecart_mesh.hpp. `hj`'s constructor and `kt.a(Loc;DDDFF)V`.

#include "core/render/minecart_mesh.hpp"

#include "core/render/box_model.hpp"
#include "core/render/draw_budget.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::render {
namespace {

constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
constexpr double kLimit = 32000.0 / kUnits;

constexpr float kPi = 3.1415927f;
constexpr float kDegrees = kPi / 180.0f;
constexpr double kPiD = 3.141592653589793;

// `hj`'s four dimensions.
constexpr int kCartWidth = 20;   // i
constexpr int kSideHeight = 8;   // j
constexpr int kCartLength = 16;  // k
constexpr int kDeck = 4;         // l

// `atan(slope.y) * 73.0F`, and the 73 is the class file's own literal -- not a
// radians-to-degrees conversion, which would be 57.3. The cart leans about a
// quarter more than the track does, on purpose.
constexpr double kCartPitchGain = 73.0;

const ModelPart& cartPart(int index)
{
    static const ModelPart parts[kMinecartParts] = {
        // The floor, laid down by a quarter turn about x.
        {float(-kCartWidth / 2), float(-kCartLength / 2), -1.0f, kCartWidth, kCartLength, 2,
         0.0f, 0, 10, false, 0.0f, float(kDeck), 0.0f, kPi / 2.0f, 0.0f, 0.0f},
        // Left side.
        {float(-kCartWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kCartWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, float(-kCartWidth / 2 + 1), float(kDeck), 0.0f,
         0.0f, 3.0f * kPi / 2.0f, 0.0f},
        // Right side.
        {float(-kCartWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kCartWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, float(kCartWidth / 2 - 1), float(kDeck), 0.0f,
         0.0f, kPi / 2.0f, 0.0f},
        // Back.
        {float(-kCartWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kCartWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, 0.0f, float(kDeck), float(-kCartLength / 2 + 1),
         0.0f, kPi, 0.0f},
        // Front.
        {float(-kCartWidth / 2 + 2), float(-kSideHeight - 1), -1.0f, kCartWidth - 4,
         kSideHeight, 2, 0.0f, 0, 0, false, 0.0f, float(kDeck), float(kCartLength / 2 - 1),
         0.0f, 0.0f, 0.0f},
        // **The underside**, from its own texture offset (44, 10) and turned
        // the other way -- a quarter turn *back* about x rather than forward,
        // which is what puts its face downwards.
        //
        // `hj.render` sets this part's rotationPointY to `4.0F - f2` where the
        // renderer passes `f2 = -0.1F`, so it sits a tenth of a model unit
        // proud of the floor above it. That is the z-fighting guard, and it is
        // the reason this box exists at all.
        {float(-kCartWidth / 2 + 1), float(-kCartLength / 2 + 1), -1.0f, kCartWidth - 2,
         kCartLength - 2, 1, 0.0f, 44, 10, false, 0.0f, float(kDeck) + 0.1f, 0.0f,
         -kPi / 2.0f, 0.0f, 0.0f},
    };
    return parts[index];
}

}  // namespace

int buildMinecarts(const entity::MinecartSystem& system, const tick::TickWorld& world,
                   double originX, double originY, double originZ, float partial,
                   mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kMinecartVerticesEach) {
        return 0;
    }

    // Where a cart is relative to the origin *before* the track lifts it, and
    // whether it could be drawn at all. The build still applies its own range
    // test to the lifted position, so what it draws is a subset of what this
    // counted and the nearest-first budget holds.
    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::Minecart& c = system[index];
        if (!c.alive) {
            return false;
        }
        *rx = c.prevX + (c.x - c.prevX) * double(partial) - originX;
        *ry = c.prevY + (c.y - c.prevY) * double(partial) - originY;
        *rz = c.prevZ + (c.z - c.prevZ) * double(partial) - originZ;
        return *rx >= -kLimit && *rx <= kLimit && *ry >= -kLimit && *ry <= kLimit
               && *rz >= -kLimit && *rz <= kLimit;
    };
    // Nearest first when the buffer cannot take them all; see draw_budget.hpp.
    DrawCutoff cutoff;
    if (system.count() * kMinecartVerticesEach > max) {
        cutoff.compute(system.count(), max, place, [](int) { return kMinecartVerticesEach; });
    }

    int written = 0;
    for (int index = 0; index < system.count(); ++index) {
        const entity::Minecart& c = system[index];
        double ringX = 0.0;
        double ringY = 0.0;
        double ringZ = 0.0;
        if (!place(index, &ringX, &ringY, &ringZ)
            || !cutoff.admit(ringX, ringY, ringZ, kMinecartVerticesEach)) {
            continue;
        }
        if (written + kMinecartVerticesEach > max) {
            break;
        }

        double px = c.prevX + (c.x - c.prevX) * double(partial);
        double py = c.prevY + (c.y - c.prevY) * double(partial);
        double pz = c.prevZ + (c.z - c.prevZ) * double(partial);

        float yaw = c.prevYaw + (c.yaw - c.prevYaw) * partial;
        float pitch = 0.0f;

        // **The tilt comes off the track, not off the cart.** Sample the rail
        // three tenths of a block either side and read the heading and slope
        // out of the vector between them.
        const entity::RailPoint here =
            entity::MinecartSystem::railPointAt(world, px, py, pz);
        if (here.valid) {
            const double probe = entity::MinecartSystem::kRenderRailProbe;
            entity::RailPoint ahead =
                entity::MinecartSystem::railPointAlong(world, px, py, pz, probe);
            entity::RailPoint behind =
                entity::MinecartSystem::railPointAlong(world, px, py, pz, -probe);
            if (!ahead.valid) {
                ahead = here;
            }
            if (!behind.valid) {
                behind = here;
            }

            px += here.x - px;
            // The mean of the two samples, which is what lifts the cart onto
            // the rail rather than into it.
            py += (ahead.y + behind.y) / 2.0 - py;
            pz += here.z - pz;

            double sx = behind.x - ahead.x;
            double sy = behind.y - ahead.y;
            double sz = behind.z - ahead.z;
            const double length = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (length != 0.0) {
                sx /= length;
                sy /= length;
                sz /= length;
                yaw = float(std::atan2(sz, sx) * 180.0 / kPiD);
                pitch = float(std::atan(sy) * kCartPitchGain);
            }
        }

        const double rx = px - originX;
        const double ry = py - originY;
        const double rz = pz - originZ;
        if (rx < -kLimit || rx > kLimit || ry < -kLimit || ry > kLimit || rz < -kLimit
            || rz > kLimit) {
            continue;
        }

        // `glRotatef(180 - yaw, 0,1,0)` then `glRotatef(-pitch, 0,0,1)`.
        const float yawRad = (180.0f - yaw) * kDegrees;
        float rollRad = -pitch * kDegrees;

        // The damage rock, which never fires in this build for the same reason
        // the boat's does not.
        const float hit = float(c.timeSinceHit) - partial;
        float damage = float(c.damage) - partial;
        if (damage < 0.0f) {
            damage = 0.0f;
        }
        float rock = 0.0f;
        if (hit > 0.0f) {
            rock = MathHelper::sin(hit) * hit * damage / 10.0f * float(c.forwardDirection)
                   * kDegrees;
        }

        // The (-1, -1, 1) flip every entity model gets, folded into the axes.
        const float s = kModelUnit;
        float ax[3] = {-s, 0.0f, 0.0f};
        float ay[3] = {0.0f, -s, 0.0f};
        float az[3] = {0.0f, 0.0f, s};

        const float sinRock = MathHelper::sin(rock);
        const float cosRock = MathHelper::cos(rock);
        const float sinRoll = MathHelper::sin(rollRad);
        const float cosRoll = MathHelper::cos(rollRad);
        const float sinYaw = MathHelper::sin(yawRad);
        const float cosYaw = MathHelper::cos(yawRad);

        auto turn = [&](float* v) {
            // Rx(rock)
            float y1 = v[1] * cosRock - v[2] * sinRock;
            float z1 = v[1] * sinRock + v[2] * cosRock;
            // Rz(-pitch)
            const float x2 = v[0] * cosRoll - y1 * sinRoll;
            const float y2 = v[0] * sinRoll + y1 * cosRoll;
            // Ry(180 - yaw)
            const float x3 = x2 * cosYaw + z1 * sinYaw;
            const float z3 = -x2 * sinYaw + z1 * cosYaw;
            v[0] = x3;
            v[1] = y2;
            v[2] = z3;
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

        for (int part = 0; part < kMinecartParts; ++part) {
            written += buildBox(cartPart(part), place, texture::EntitySkin::Minecart,
                                c.light, out + written, max - written);
        }
    }
    return written;
}

}  // namespace mc::render
