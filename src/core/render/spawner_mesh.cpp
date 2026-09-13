// See spawner_mesh.hpp.

#include "core/render/spawner_mesh.hpp"

#include "core/render/draw_budget.hpp"
#include "core/util/math_helper.hpp"

namespace mc::render {
namespace {

constexpr float kPi = 3.1415927f;

// The one entity the renderer builds, minus the reflective constructor: a mob
// of the named kind that has never ticked. Everything `poseMob` and `placeMob`
// read -- the limb swing, the head turn, the death spin, the creeper's fuse --
// is zero in a fresh one, which is exactly the state `ew.createEntityInWorld`
// hands back.
entity::Mob displayMob(entity::MobType type)
{
    entity::Mob mob{};
    mob.alive = true;
    mob.type = type;
    const entity::MobDef& def = entity::mobDef(type);
    mob.body.setSize(def.width, def.height, 0.0f);
    mob.body.setFeet(0.0, 0.0, 0.0);
    return mob;
}

// `glRotatef(spin, 0, 1, 0)` then `glRotatef(-30, 1, 0, 0)`, as one rotation of
// a vector: Rx runs first because it is the nearer of the two to the vertex.
struct SpawnerTurn {
    float sinY = 0.0f, cosY = 1.0f;
    float sinX = 0.0f, cosX = 1.0f;

    void apply(const float* v, float* out) const
    {
        // Rx(-30): y' = y cos - z sin, z' = y sin + z cos.
        const float ry = v[1] * cosX - v[2] * sinX;
        const float rz = v[1] * sinX + v[2] * cosX;
        // Ry(spin): x' = x cos + z sin, z' = -x sin + z cos.
        out[0] = v[0] * cosY + rz * sinY;
        out[1] = ry;
        out[2] = -v[0] * sinY + rz * cosY;
    }

    void applyD(double x, double y, double z, double* out) const
    {
        const double ry = y * double(cosX) - z * double(sinX);
        const double rz = y * double(sinX) + z * double(cosX);
        out[0] = x * double(cosY) + rz * double(sinY);
        out[1] = ry;
        out[2] = -x * double(sinY) + rz * double(cosY);
    }
};

SpawnerTurn turnFor(const entity::MobSpawnerBlock& spawner, float partial)
{
    // `(prevYaw + (yaw - prevYaw) * partial) * 10`, interpolated as a double
    // and narrowed on the way into `glRotatef` -- which is what the jar does,
    // `d2f` sitting between the interpolation and the multiply.
    const double lerped =
        spawner.prevYaw + (spawner.yaw - spawner.prevYaw) * double(partial);
    const float degrees = float(lerped) * kSpawnerSpinFactor;

    SpawnerTurn turn;
    turn.sinY = MathHelper::sin(degrees * kPi / 180.0f);
    turn.cosY = MathHelper::cos(degrees * kPi / 180.0f);
    turn.sinX = MathHelper::sin(kSpawnerTilt * kPi / 180.0f);
    turn.cosX = MathHelper::cos(kSpawnerTilt * kPi / 180.0f);
    return turn;
}

}  // namespace

Placement placeSpawnerMob(const entity::MobSpawnerBlock& spawner, double originX,
                          double originY, double originZ, float partial)
{
    // **`placeMob` supplies the inner half and is not duplicated here.** The
    // display mob sits at the local origin, so what comes back is exactly
    // `RenderLiving`'s own flip-and-lift with nothing of the world in it -- the
    // `glScalef(-1,-1,1)`, the 24-model-unit drop and the eighth of a unit of
    // foot lift. Rewriting those three here is how they would drift apart.
    const entity::Mob mob = displayMob(spawner.mob);
    const Placement base = placeMob(mob, 0.0, 0.0, 0.0, partial);

    const SpawnerTurn turn = turnFor(spawner, partial);

    Placement place;
    float scaled[3];
    for (int axis = 0; axis < 3; ++axis) {
        const float* src = axis == 0 ? base.ax : (axis == 1 ? base.ay : base.az);
        float* dst = axis == 0 ? place.ax : (axis == 1 ? place.ay : place.az);
        scaled[0] = src[0] * kSpawnerModelScale;
        scaled[1] = src[1] * kSpawnerModelScale;
        scaled[2] = src[2] * kSpawnerModelScale;
        turn.apply(scaled, dst);
    }

    // The origin, through the same chain: scale, drop by 0.4, turn, lift by
    // 0.4, and only then the block's own corner-plus-a-half.
    double moved[3];
    turn.applyD(base.x * double(kSpawnerModelScale),
                base.y * double(kSpawnerModelScale) - kSpawnerPivotLift,
                base.z * double(kSpawnerModelScale), moved);

    place.x = double(spawner.x) + 0.5 - originX + moved[0];
    place.y = double(spawner.y) + kSpawnerPivotLift - originY + moved[1];
    place.z = double(spawner.z) + 0.5 - originZ + moved[2];
    return place;
}

int buildSpawnerMobs(const entity::MobSpawnerStore& store, double originX, double originY,
                     double originZ, float partial, mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kBoxVertices) {
        return 0;
    }

    constexpr double kUnits = double(mesh::kDetailUnitsPerBlock);
    constexpr double kLimit = 32000.0 / kUnits;

    // **A spawner with a name this build cannot make draws nothing**, which is
    // the same answer the tick gives it -- see `MobSpawnerBlock::known`.
    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::MobSpawnerBlock& s = store[index];
        if (!s.used || !s.known) {
            return false;
        }
        *rx = double(s.x) + 0.5 - originX;
        *ry = double(s.y) + 0.5 - originY;
        *rz = double(s.z) + 0.5 - originZ;
        return *rx >= -kLimit && *rx <= kLimit && *ry >= -kLimit && *ry <= kLimit
               && *rz >= -kLimit && *rz <= kLimit;
    };

    const auto cost = [&](int index) {
        ModelPart parts[kMobMaxParts];
        texture::EntitySkin skins[kMobMaxParts];
        const entity::Mob mob = displayMob(store[index].mob);
        return poseMob(mob, partial, parts, skins, kMobMaxParts) * kBoxVertices;
    };

    DrawCutoff cutoff;
    if (store.count() * kMobVerticesEach > max) {
        cutoff.compute(store.count(), max, place, cost);
    }

    int written = 0;
    for (int index = 0; index < store.count(); ++index) {
        const entity::MobSpawnerBlock& s = store[index];
        double rx = 0.0;
        double ry = 0.0;
        double rz = 0.0;
        if (!place(index, &rx, &ry, &rz)) {
            continue;
        }

        ModelPart parts[kMobMaxParts];
        texture::EntitySkin skins[kMobMaxParts];
        const entity::Mob mob = displayMob(s.mob);
        const int count = poseMob(mob, partial, parts, skins, kMobMaxParts);
        if (count == 0) {
            continue;
        }
        const int vertices = count * kBoxVertices;
        if (!cutoff.admit(rx, ry, rz, vertices) || written + vertices > max) {
            continue;
        }

        // **The cell's light, not the miniature's.** The mob is inside the
        // block, so sampling where its feet are would read the cage.
        const Placement placement = placeSpawnerMob(s, originX, originY, originZ, partial);
        for (int part = 0; part < count; ++part) {
            written += buildBox(parts[part], placement, skins[part], s.light, out + written,
                                max - written);
        }
    }
    return written;
}

}  // namespace mc::render
