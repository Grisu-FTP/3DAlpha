// See minecart_mesh.hpp. `hj`'s constructor and `kt.a(Loc;DDDFF)V`.

#include "core/render/minecart_mesh.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/box.hpp"
#include "core/render/box_model.hpp"
#include "core/render/draw_budget.hpp"
#include "core/render/entity_range.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cmath>

namespace mc::render {
namespace {

// Both passes write 1/256 of a block, so a cart reaches the 57 blocks
// `kh.a(D)Z` gives a 0.98 x 0.7 box -- see core/render/entity_range.hpp.
constexpr double kUnits = double(kEntityUnitsPerBlock);
constexpr double kLimit = kEntityPlacementLimit;

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

// **Which block a cart carries**, and 0 for a plain one. `kt.a` names
// `ly.av` -- the chest -- and `ly.aC`, the *idle* furnace: a burning furnace
// cart does not light up in a1.1.2.
u16 cartBlock(entity::MinecartType type)
{
    switch (type) {
        case entity::MinecartType::Chest: return u16(mcver::Block::Chest);
        case entity::MinecartType::Furnace: return u16(mcver::Block::Furnace);
        default: return 0;
    }
}

i16 toUnits(double blocks)
{
    const double units = blocks * kUnits;
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

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

// **Where a cart is drawn and which way it is turned**, worked out of the track
// rather than out of the cart -- see the header. The axes are unit vectors in
// blocks: the cart's own model multiplies them by `kModelUnit` and folds in the
// (-1, -1, 1) flip every entity model gets, and the block a chest cart carries
// uses them as they are, because `renderBlockOnInventory` runs *before* that
// flip and in block units.
//
// Shared so the two agree. A block that worked out its own heading would part
// company with the cart on the first curve.
struct CartFrame {
    double x = 0.0, y = 0.0, z = 0.0;
    float ax[3] = {1.0f, 0.0f, 0.0f};
    float ay[3] = {0.0f, 1.0f, 0.0f};
    float az[3] = {0.0f, 0.0f, 1.0f};
};

bool cartFrame(const entity::Minecart& c, const tick::TickWorld& world, double originX,
               double originY, double originZ, float partial, CartFrame* out)
{
    double px = c.prevX + (c.x - c.prevX) * double(partial);
    double py = c.prevY + (c.y - c.prevY) * double(partial);
    double pz = c.prevZ + (c.z - c.prevZ) * double(partial);

    float yaw = c.prevYaw + (c.yaw - c.prevYaw) * partial;
    float pitch = 0.0f;

    // **The tilt comes off the track, not off the cart.** Sample the rail
    // three tenths of a block either side and read the heading and slope
    // out of the vector between them.
    const entity::RailPoint here = entity::MinecartSystem::railPointAt(world, px, py, pz);
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

    out->x = px - originX;
    out->y = py - originY;
    out->z = pz - originZ;
    if (out->x < -kLimit || out->x > kLimit || out->y < -kLimit || out->y > kLimit
        || out->z < -kLimit || out->z > kLimit) {
        return false;
    }

    // `glRotatef(180 - yaw, 0,1,0)` then `glRotatef(-pitch, 0,0,1)`.
    const float yawRad = (180.0f - yaw) * kDegrees;
    const float rollRad = -pitch * kDegrees;

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

    const float sinRock = MathHelper::sin(rock);
    const float cosRock = MathHelper::cos(rock);
    const float sinRoll = MathHelper::sin(rollRad);
    const float cosRoll = MathHelper::cos(rollRad);
    const float sinYaw = MathHelper::sin(yawRad);
    const float cosYaw = MathHelper::cos(yawRad);

    const auto turn = [&](float* v) {
        // Rx(rock)
        const float y1 = v[1] * cosRock - v[2] * sinRock;
        const float z1 = v[1] * sinRock + v[2] * cosRock;
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
    out->ax[0] = 1.0f; out->ax[1] = 0.0f; out->ax[2] = 0.0f;
    out->ay[0] = 0.0f; out->ay[1] = 1.0f; out->ay[2] = 0.0f;
    out->az[0] = 0.0f; out->az[1] = 0.0f; out->az[2] = 1.0f;
    turn(out->ax);
    turn(out->ay);
    turn(out->az);
    return true;
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
        return entityInDrawRange(*rx, *ry, *rz, entity::kMinecartWidth,
                                 entity::kMinecartHeight);
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

        CartFrame frame;
        if (!cartFrame(c, world, originX, originY, originZ, partial, &frame)) {
            continue;
        }

        // The (-1, -1, 1) flip every entity model gets, and the model unit,
        // folded into the frame's axes -- both are linear, so scaling the
        // turned axes is the same as turning the scaled ones.
        const float s = kModelUnit;
        const float ax[3] = {-s * frame.ax[0], -s * frame.ax[1], -s * frame.ax[2]};
        const float ay[3] = {-s * frame.ay[0], -s * frame.ay[1], -s * frame.ay[2]};
        const float az[3] = {s * frame.az[0], s * frame.az[1], s * frame.az[2]};
        const double rx = frame.x;
        const double ry = frame.y;
        const double rz = frame.z;

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
                                c.light, out + written, max - written, kEntityUnitsPerBlock);
        }
    }
    return written;
}

int buildMinecartBlocks(const entity::MinecartSystem& system, const tick::TickWorld& world,
                        double originX, double originY, double originZ, float partial,
                        mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kMinecartBlockVerticesEach) {
        return 0;
    }

    // Only the two carts that carry something are candidates, so the budget is
    // counted over those rather than over every cart on the track.
    const auto place = [&](int index, double* rx, double* ry, double* rz) {
        const entity::Minecart& c = system[index];
        if (!c.alive || cartBlock(c.type) == 0) {
            return false;
        }
        *rx = c.prevX + (c.x - c.prevX) * double(partial) - originX;
        *ry = c.prevY + (c.y - c.prevY) * double(partial) - originY;
        *rz = c.prevZ + (c.z - c.prevZ) * double(partial) - originZ;
        return entityInDrawRange(*rx, *ry, *rz, entity::kMinecartWidth,
                                 entity::kMinecartHeight);
    };
    DrawCutoff cutoff;
    if (system.count() * kMinecartBlockVerticesEach > max) {
        cutoff.compute(system.count(), max, place,
                       [](int) { return kMinecartBlockVerticesEach; });
    }

    // Corner indices per face, in `mc::mesh::Face` order -- `mesh::kFaceCorner`
    // rewritten as indices into the eight, an index being `x + 2y + 4z`. The
    // same table `item_entity_mesh.cpp` uses, and for the same reason: the
    // winding has to survive an arbitrary rotation.
    static constexpr int kFace[6][4] = {
        {0, 1, 5, 4},  // -Y
        {6, 7, 3, 2},  // +Y
        {1, 0, 2, 3},  // -Z
        {4, 5, 7, 6},  // +Z
        {0, 4, 6, 2},  // -X
        {5, 1, 3, 7},  // +X
    };

    int written = 0;
    for (int index = 0; index < system.count(); ++index) {
        const entity::Minecart& c = system[index];
        const u16 carried = cartBlock(c.type);
        double ringX = 0.0;
        double ringY = 0.0;
        double ringZ = 0.0;
        if (!place(index, &ringX, &ringY, &ringZ)
            || !cutoff.admit(ringX, ringY, ringZ, kMinecartBlockVerticesEach)) {
            continue;
        }
        if (written + kMinecartBlockVerticesEach > max) {
            break;
        }

        CartFrame frame;
        if (!cartFrame(c, world, originX, originY, originZ, partial, &frame)) {
            continue;
        }

        // `glScalef(0.75)`, `glTranslatef(0, 0.3125, 0)`, `glRotatef(90, 0,1,0)`
        // and then `renderBlockOnInventory`'s own `glTranslatef(-0.5,-0.5,-0.5)`
        // -- so a corner `p` of the unit cube lands, in the cart's frame, at
        //
        //     0.75 * Ry(90) * (p - 0.5)  +  (0, 0.75 * 0.3125, 0)
        //
        // and `Ry(90)` is `(x, y, z) -> (z, y, -x)`. The lift is inside the
        // scale, which is why it is 0.234375 of a block and not 0.3125.
        constexpr float kCartBlockScale = 0.75f;
        constexpr float kCartBlockLift = kCartBlockScale * 0.3125f;

        double px[8], py[8], pz[8];
        for (int corner = 0; corner < 8; ++corner) {
            const float qx = ((corner & 1) != 0 ? 1.0f : 0.0f) - 0.5f;
            const float qy = ((corner & 2) != 0 ? 1.0f : 0.0f) - 0.5f;
            const float qz = ((corner & 4) != 0 ? 1.0f : 0.0f) - 0.5f;
            // The quarter turn, then the scale and the lift.
            const float lx = kCartBlockScale * qz;
            const float ly = kCartBlockScale * qy + kCartBlockLift;
            const float lz = kCartBlockScale * -qx;
            px[corner] = frame.x + double(lx * frame.ax[0] + ly * frame.ay[0]
                                          + lz * frame.az[0]);
            py[corner] = frame.y + double(lx * frame.ax[1] + ly * frame.ay[1]
                                          + lz * frame.az[1]);
            pz[corner] = frame.z + double(lx * frame.ax[2] + ly * frame.ay[2]
                                          + lz * frame.az[2]);
        }

        const block::BlockDef& def = block::def(block::BlockId(carried));
        for (int face = 0; face < 6; ++face) {
            // **The box is the whole block**, so the slice is the whole tile --
            // but it is asked for the same way every other box in the project
            // asks, rather than assumed.
            const mesh::BoxTileUv uv =
                mesh::boxTileUv(int(def.faces[face]), AABB{0.0, 0.0, 0.0, 1.0, 1.0, 1.0},
                                face);
            // **The block's own face shade, not the direction it ends up
            // pointing.** a1.1.2 lights this cube with the entity lights and a
            // normal per face; this port has the mesher's table instead, and
            // taking it off the unrotated face is what keeps a cart's chest
            // looking the same all the way round a curve.
            const u8 shade = mesh::kFaceShade[face];
            for (int corner = 0; corner < 4; ++corner) {
                const int idx = kFace[face][corner];
                mesh::DetailVertex& v = out[written];
                v.x = toUnits(px[idx]);
                v.y = toUnits(py[idx]);
                v.z = toUnits(pz[idx]);
                v.face = 0;
                v.u = mesh::kFaceCornerUV[face][corner][0] != 0 ? uv.u1 : uv.u0;
                v.v = mesh::kFaceCornerUV[face][corner][1] != 0 ? uv.v1 : uv.v0;
                v.r = shade;
                v.g = shade;
                v.b = shade;
                v.light = c.light;
                ++written;
            }
        }
    }
    return written;
}

}  // namespace mc::render
