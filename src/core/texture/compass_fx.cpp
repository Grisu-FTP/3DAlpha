// `aa.a()` -- TextureCompassFX.onTick -- transcribed. See the header for why
// the compass is a texture and not an item.

#include "core/texture/compass_fx.hpp"

#include "core/item/registry.hpp"

#include <cmath>
#include <cstring>

namespace mc::texture {
namespace {

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 6.283185307179586;

constexpr int kTileEdge = 16;

// `d2i` is a narrowing conversion and truncates toward zero, which is what a
// C++ cast to int does. Named so the two loops below read as the class file
// rather than as arithmetic somebody chose.
inline int narrow(double v)
{
    return static_cast<int>(v);
}

// One texel, if it lands on the tile at all.
//
// **The original does not bounds-check and does not need to**: every index it
// computes is inside the tile for every angle, because the needle is 4.8 texels
// long about a centre 8.5 texels in. This checks anyway -- a `d2i` on a NaN
// angle is zero in Java and undefined in C++, and the sanitisers in the host
// build treat an out-of-range store as the fault it would be.
inline void plot(u8* texels, int x, int y, u8 r, u8 g, u8 b, u8 a)
{
    if (x < 0 || x >= kTileEdge || y < 0 || y >= kTileEdge) {
        return;
    }
    u8* p = texels + (usize(y) * kTileEdge + usize(x)) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

}  // namespace

int compassTile()
{
    // **Asked of the item table, not written down.** The generator sets
    // `animatedIcon` from the FX class's own tile and sheet, so this is the
    // game's answer to "which tile does RenderEngine overwrite" rather than
    // ours. A version with no such item -- or a build whose jar had no `aa` --
    // leaves it at -1 and every caller draws the pack's tile unchanged.
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.animatedIcon && def.sheet == item::IconSheet::Items) {
            return int(def.icon);
        }
    }
    return -1;
}

void CompassTexture::setBase(const u8* base)
{
    std::memcpy(base_, base, sizeof(base_));
    // So a compass drawn before the first tick is the pack's face rather than
    // transparent. `ready_` stays false: there is no needle on it yet.
    std::memcpy(texels_, base_, sizeof(texels_));
    // A new pack is a new face even where the needle has not moved, and what
    // the consumers hold is the old one.
    pushDue_ = true;
}

bool CompassTexture::tick(int spawnX, int spawnZ, double playerX, double playerZ,
                          float yawDegrees)
{
    // The first half of `a()`: the tile is repainted from the sheet's own
    // pixels every step. That is what makes the needle a needle rather than a
    // trail -- and it is also why a pack's compass face survives.
    std::memcpy(texels_, base_, sizeof(texels_));

    // `(rotationYaw - 90.0F) * PI / 180.0 - atan2(spawnZ - posZ, spawnX - posX)`.
    //
    // **The subtraction is in float and the widening comes after**, because
    // that is the order in the class file (`fsub` then `f2d`). It is a
    // difference in the eleventh digit and it costs nothing to get right.
    const double heading = double(yawDegrees - 90.0f) * kPi / 180.0;
    const double dx = double(spawnX) - playerX;
    const double dz = double(spawnZ) - playerZ;
    const double target = heading - std::atan2(dz, dx);

    // Wrap the error into (-PI, PI], then clamp it to a radian. The wrap is a
    // pair of `while`s in the original and stays a pair here: an angle that has
    // drifted many turns from the target -- which `angle_` does, because
    // nothing ever normalises it -- takes as many passes as it takes.
    double error = target - angle_;
    while (error < -kPi) {
        error += kTwoPi;
    }
    while (error >= kPi) {
        error -= kTwoPi;
    }
    if (error < -kCompassMaxError) {
        error = -kCompassMaxError;
    }
    if (error > kCompassMaxError) {
        error = kCompassMaxError;
    }

    velocity_ += error * kCompassPull;
    velocity_ *= kCompassDamping;
    angle_ += velocity_;

    const double s = std::sin(angle_);
    const double c = std::cos(angle_);

    // **The crossbar**, nine texels of grey across the needle. Note that its
    // x uses the cosine and its y the sine, and that the y is *subtracted* --
    // the two loops are not the same loop with the arguments swapped, and
    // reading them as if they were puts the bar on the wrong diagonal.
    for (int i = -4; i <= 4; ++i) {
        const int x = narrow(kCompassCentreX + c * i * 0.3);
        const int y = narrow(kCompassCentreY - s * i * 0.3 * 0.5);
        plot(texels_, x, y, 100, 100, 100, 255);
    }

    // **The needle**, twenty-five texels from -8 to +16 -- so it is not
    // centred: the red end reaches twice as far as the grey one, which is what
    // makes a compass readable at 16 x 16. Here the x uses the sine and the y
    // the cosine, and the y is added.
    for (int i = -8; i <= 16; ++i) {
        const int x = narrow(kCompassCentreX + s * i * 0.3);
        const int y = narrow(kCompassCentreY + c * i * 0.3 * 0.5);
        if (i >= 0) {
            plot(texels_, x, y, 255, 20, 20, 255);
        } else {
            plot(texels_, x, y, 100, 100, 100, 255);
        }
    }

    ready_ = true;

    // **Whether anybody needs to be told.** See the header: the spring keeps
    // converging long after the last texel has stopped moving, and every one of
    // those ticks used to cost a texture push and a bottom-screen repaint.
    if (!pushDue_ && std::memcmp(pushed_, texels_, sizeof(texels_)) == 0) {
        return false;
    }
    std::memcpy(pushed_, texels_, sizeof(pushed_));
    pushDue_ = false;
    return true;
}

}  // namespace mc::texture
