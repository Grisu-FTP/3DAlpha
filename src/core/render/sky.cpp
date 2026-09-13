// RenderGlobal's sky, as vertices. See the header for what it is made of.

#include "core/render/sky.hpp"

#include "core/texture/entity_skins.hpp"
#include "core/util/java_random.hpp"

#include <cmath>

namespace mc::render {

namespace {

// Blocks into the vertex format's own units, rounded rather than truncated:
// every coordinate in this file is either a whole number of blocks or a star
// offset of a fraction of one, and truncating the negative half of a star's
// quad would make it a unit wider on one side than the other.
i16 toUnits(double blocks)
{
    const double units = blocks * double(kSkyUnitsPerBlock);
    return i16(units >= 0.0 ? units + 0.5 : units - 0.5);
}

// One texel of the entity sheet as a u, in the 1/16384 units the vertex format
// uses across a whole texture -- the same conversion arrow_mesh.cpp makes, and
// for the same sheet.
i16 sheetU(float texels)
{
    const float units =
        texels * float(mesh::kUvUnitsPerAtlas) / float(texture::kEntitySheetWidth);
    return i16(units + 0.5f);
}

i16 sheetV(float texels)
{
    const float units =
        texels * float(mesh::kUvUnitsPerAtlas) / float(texture::kEntitySheetHeight);
    return i16(units + 0.5f);
}

// Every vertex in this pass carries the same colour and the same light, and
// neither is read: the planes and the stars take their colour from a combiner
// constant, the sun and the moon from the texture, and no range of this pass
// binds the lightmap. White and full-bright is what they would mean if one of
// those ever changed.
mesh::DetailVertex vertex(double x, double y, double z, i16 u, i16 v)
{
    mesh::DetailVertex out{};
    out.x = toUnits(x);
    out.y = toUnits(y);
    out.z = toUnits(z);
    out.face = 0;
    out.u = u;
    out.v = v;
    out.r = 255;
    out.g = 255;
    out.b = 255;
    out.light = 0xFF;
    return out;
}

// The grid the two planes share: `for (i = -64 * 6; i <= 64 * 6; i += 64)` in
// both axes, one quad per cell. `flip` swaps the winding, which is the only
// difference between the sky's plane and the void's -- the original walks the
// corners the other way round so the plane below the camera faces up.
int buildPlane(mesh::DetailVertex* out, double height, bool flip)
{
    constexpr int kCell = kSkyPlaneCellBlocks;
    constexpr int kEnd = kCell * (kSkyPlaneCells / 2);  // 384

    int written = 0;
    for (int i = -kEnd; i <= kEnd; i += kCell) {
        for (int j = -kEnd; j <= kEnd; j += kCell) {
            const double x0 = double(i);
            const double x1 = double(i + kCell);
            const double z0 = double(j);
            const double z1 = double(j + kCell);
            if (!flip) {
                // `z`, the sky: (i, h, j), (i+64, h, j), (i+64, h, j+64), (i, h, j+64)
                out[written++] = vertex(x0, height, z0, 0, 0);
                out[written++] = vertex(x1, height, z0, 0, 0);
                out[written++] = vertex(x1, height, z1, 0, 0);
                out[written++] = vertex(x0, height, z1, 0, 0);
            } else {
                // `A`, the void: (i+64, h, j), (i, h, j), (i, h, j+64), (i+64, h, j+64)
                out[written++] = vertex(x1, height, z0, 0, 0);
                out[written++] = vertex(x0, height, z0, 0, 0);
                out[written++] = vertex(x0, height, z1, 0, 0);
                out[written++] = vertex(x1, height, z1, 0, 0);
            }
        }
    }
    return written;
}

// The sun and the moon, both square, both flat, both facing the camera because
// the camera is always directly below or above them at the moment they are
// drawn -- the rotation the renderer applies is about X and they sit on the Y
// axis, so they turn edge-on at dawn and dusk exactly as the original's do.
//
// **The moon's UVs are the sun's reversed in both axes.** That is in the
// bytecode, not an accident of transcription: renderSky walks the moon's
// corners from (-f, -100, +f) round to (-f, -100, -f) with (1,1), (0,1), (0,0),
// (1,0) on them. The result is that the moon shows upside down and mirrored
// relative to the sun, which is what a1.1.2 does.
int buildCelestial(mesh::DetailVertex* out, texture::EntitySkin page, double halfWidth,
                   double height, bool moon)
{
    int originX = 0;
    int originY = 0;
    texture::skinOrigin(page, &originX, &originY);
    const float edge = float(texture::kCelestialPagePixels);
    const i16 u0 = sheetU(float(originX));
    const i16 u1 = sheetU(float(originX) + edge);
    const i16 v0 = sheetV(float(originY));
    const i16 v1 = sheetV(float(originY) + edge);

    const double f = halfWidth;
    if (!moon) {
        out[0] = vertex(-f, height, -f, u0, v0);
        out[1] = vertex(+f, height, -f, u1, v0);
        out[2] = vertex(+f, height, +f, u1, v1);
        out[3] = vertex(-f, height, +f, u0, v1);
    } else {
        out[0] = vertex(-f, height, +f, u1, v1);
        out[1] = vertex(+f, height, +f, u0, v1);
        out[2] = vertex(+f, height, -f, u0, v0);
        out[3] = vertex(-f, height, -f, u1, v0);
    }
    return 4;
}

// `e.f()V`, renderStars, with the display list replaced by a buffer.
//
// 1500 candidates from `new Random(10842L)`; the ones inside the unit sphere
// and outside its 0.01 core are pushed out to radius 100 and given a quad of
// their own, facing the origin, rolled by a random angle. **The rejected
// candidates still draw their four floats**, so the stream cannot be
// short-circuited -- every star after the first rejection depends on it.
//
// The three rotations are the original's, in the original's order: roll about
// the billboard's own normal, then pitch, then yaw. Written out rather than
// folded into a matrix because the bytecode is what this is checked against.
int buildStars(mesh::DetailVertex* out, int max)
{
    JavaRandom random(10842);
    int written = 0;

    for (int i = 0; i < 1500; ++i) {
        // Java computes these in float and widens, which is not the same as
        // computing them in double: `nextFloat() * 2.0F - 1.0F` rounds once
        // before the subtraction.
        double x = double(random.nextFloat() * 2.0f - 1.0f);
        double y = double(random.nextFloat() * 2.0f - 1.0f);
        double z = double(random.nextFloat() * 2.0f - 1.0f);
        const double size = double(0.25f + random.nextFloat() * 0.25f);

        double d = x * x + y * y + z * z;
        if (!(d < 1.0 && d > 0.01)) {
            continue;
        }
        d = 1.0 / std::sqrt(d);
        x *= d;
        y *= d;
        z *= d;

        const double cx = x * double(kStarRadiusBlocks);
        const double cy = y * double(kStarRadiusBlocks);
        const double cz = z * double(kStarRadiusBlocks);

        const double yaw = std::atan2(x, z);
        const double sinYaw = std::sin(yaw);
        const double cosYaw = std::cos(yaw);
        const double pitch = std::atan2(std::sqrt(x * x + z * z), y);
        const double sinPitch = std::sin(pitch);
        const double cosPitch = std::cos(pitch);
        const double roll = random.nextDouble() * 3.141592653589793 * 2.0;
        const double sinRoll = std::sin(roll);
        const double cosRoll = std::cos(roll);

        if (written + 4 > max) {
            return written;
        }
        for (int corner = 0; corner < 4; ++corner) {
            // The quad's own plane: (+-size, +-size) walked as a ring, with no
            // thickness. The original carries the zero through both rotations
            // below and so does this.
            const double depth = 0.0;
            const double cu = double((corner & 2) - 1) * size;
            const double cv = double(((corner + 1) & 2) - 1) * size;

            const double rolledU = cu * cosRoll - cv * sinRoll;
            const double rolledV = cv * cosRoll + cu * sinRoll;

            const double pitchedY = rolledU * sinPitch + depth * cosPitch;
            const double pitchedZ = depth * sinPitch - rolledU * cosPitch;

            const double offsetX = pitchedZ * sinYaw - rolledV * cosYaw;
            const double offsetY = pitchedY;
            const double offsetZ = rolledV * sinYaw + pitchedZ * cosYaw;

            out[written++] = vertex(cx + offsetX, cy + offsetY, cz + offsetZ, 0, 0);
        }
    }
    return written;
}

}  // namespace

world::SkyColour voidPlaneColour(const world::SkyColour& sky)
{
    return world::SkyColour{sky.r * 0.2f + 0.04f, sky.g * 0.2f + 0.04f, sky.b * 0.6f + 0.1f};
}

int buildSky(mesh::DetailVertex* out, int max)
{
    if (out == nullptr || max < kSkyVertexCount) {
        return 0;
    }

    int written = buildPlane(out + kSkyPlaneFirst, double(kSkyPlaneHeightBlocks), false);
    written += buildPlane(out + kVoidPlaneFirst, -double(kSkyPlaneHeightBlocks), true);
    written += buildCelestial(out + kSunFirst, texture::EntitySkin::Sun,
                              double(kSunHalfWidthBlocks), double(kCelestialDistanceBlocks),
                              false);
    written += buildCelestial(out + kMoonFirst, texture::EntitySkin::Moon,
                              double(kMoonHalfWidthBlocks), -double(kCelestialDistanceBlocks),
                              true);
    written += buildStars(out + kStarsFirst, max - kStarsFirst);
    return written;
}

}  // namespace mc::render
