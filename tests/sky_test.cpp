// The sky: its colours over a day, and the geometry they are painted on.
//
// The colour half is checked against `tests/sky_vectors.hpp`, which came out of
// a **real a1.1.2 World** -- getSkyColor, getFogColor and getStarBrightness
// asked of the original at fourteen times of day. The geometry half cannot be:
// `RenderGlobal` builds it into an OpenGL display list, so what is checked here
// is the star field's random stream (which the vectors do carry) and then the
// invariants a transcription of the corner arithmetic either has or does not --
// that every star is on the sphere, that every quad is square and flat, and
// that the two planes are where the original puts them.

#include "framework.hpp"

#include "core/mesh/vertex.hpp"
#include "core/render/sky.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/world/daylight.hpp"
#include "sky_vectors.hpp"

#include <cmath>
#include <vector>

using namespace mc;

namespace {

// A colour channel is a 255th, so a thousandth of one is comfortably finer than
// anything a screen could show and far tighter than a transcription error.
constexpr double kColourEpsilon = 1e-4;

double blocks(i16 units)
{
    return double(units) / double(render::kSkyUnitsPerBlock);
}

// The whole sky, built once per test. The count is checked by the caller
// rather than here: CHECK_EQ returns, and this function owes one a vector.
std::vector<mesh::DetailVertex> builtSky(int* written)
{
    std::vector<mesh::DetailVertex> verts(usize(render::kSkyVertexCount));
    *written = render::buildSky(verts.data(), render::kSkyVertexCount);
    return verts;
}

}  // namespace

TEST(the_sky_and_fog_colours_match_the_original_across_a_whole_day)
{
    for (int i = 0; i < test::kSkyCaseCount; ++i) {
        const test::SkyCase& c = test::kSkyCases[i];
        const world::SkyColour sky = world::skyColour(c.time, c.partial);
        const world::SkyColour fog = world::fogColour(c.time, c.partial);

        CHECK(std::fabs(double(sky.r) - c.skyR) < kColourEpsilon);
        CHECK(std::fabs(double(sky.g) - c.skyG) < kColourEpsilon);
        CHECK(std::fabs(double(sky.b) - c.skyB) < kColourEpsilon);
        CHECK(std::fabs(double(fog.r) - c.fogR) < kColourEpsilon);
        CHECK(std::fabs(double(fog.g) - c.fogG) < kColourEpsilon);
        CHECK(std::fabs(double(fog.b) - c.fogB) < kColourEpsilon);
        CHECK(std::fabs(double(world::starBrightness(c.time, c.partial)) - c.stars)
              < kColourEpsilon);
    }
}

TEST(noon_is_the_base_colour_undimmed_and_midnight_is_black)
{
    // The two ends, stated as the packed constants they come from: 0x88BBFF for
    // the sky and 0xC0D8FF for the fog. Full daylight leaves the first exactly
    // as it is; midnight takes it to nothing, while the fog keeps the floor
    // that stops a night from being a void.
    const world::SkyColour noon = world::skyColour(6000);
    CHECK(std::fabs(noon.r - 0x88 / 255.0f) < 1e-6f);
    CHECK(std::fabs(noon.g - 0xBB / 255.0f) < 1e-6f);
    CHECK(std::fabs(noon.b - 0xFF / 255.0f) < 1e-6f);

    const world::SkyColour night = world::skyColour(18000);
    CHECK_EQ(night.r, 0.0f);
    CHECK_EQ(night.g, 0.0f);
    CHECK_EQ(night.b, 0.0f);

    const world::SkyColour nightFog = world::fogColour(18000);
    CHECK(std::fabs(nightFog.r - 0xC0 / 255.0f * 0.06f) < 1e-6f);
    CHECK(std::fabs(nightFog.b - 0xFF / 255.0f * 0.09f) < 1e-6f);
    // Bluer than it is red, at every hour: the blue channel has both the
    // largest base and the largest floor.
    CHECK(nightFog.b > nightFog.r);
}

TEST(the_stars_come_out_after_the_sky_has_already_darkened)
{
    // The gap is the point of the 0.75 in getStarBrightness where the colours
    // use 0.5. At 12800 the sky is already half dark and the stars are barely
    // there; they do not reach full strength until the middle of the night.
    CHECK_EQ(world::starBrightness(6000), 0.0f);
    CHECK_EQ(world::starBrightness(12000), 0.0f);
    CHECK(world::starBrightness(12800) < 0.05f);
    CHECK(world::starBrightness(12800) > 0.0f);
    CHECK_EQ(world::starBrightness(18000), 0.5f);
    // And never brighter than half, which is what keeps them from washing the
    // moon out.
    for (i64 t = 0; t < 24000; t += 37) {
        CHECK(world::starBrightness(t) <= 0.5f);
        CHECK(world::starBrightness(t) >= 0.0f);
    }
}

TEST(the_view_fog_is_the_fog_colour_pulled_towards_the_sky_by_the_render_distance)
{
    // `1 - pow(1 / (4 - renderDistance), 0.25)` at the four settings the
    // original has, recovered from a chunk count. Far pulls hardest; Tiny does
    // not pull at all, so there the view fog *is* getFogColor.
    const i64 t = 6000;
    const world::SkyColour sky = world::skyColour(t);
    const world::SkyColour fog = world::fogColour(t);

    const world::SkyColour tiny = world::viewFogColour(t, 0.0f, 2);
    CHECK(std::fabs(tiny.r - fog.r) < 1e-6f);
    CHECK(std::fabs(tiny.g - fog.g) < 1e-6f);
    CHECK(std::fabs(tiny.b - fog.b) < 1e-6f);

    const double mix[4] = {
        1.0 - std::pow(1.0 / 4.0, 0.25),
        1.0 - std::pow(1.0 / 3.0, 0.25),
        1.0 - std::pow(1.0 / 2.0, 0.25),
        0.0,
    };
    const int chunks[4] = {16, 8, 4, 2};
    for (int i = 0; i < 4; ++i) {
        const world::SkyColour got = world::viewFogColour(t, 0.0f, chunks[i]);
        const double want = double(fog.r) + (double(sky.r) - double(fog.r)) * mix[i];
        CHECK(std::fabs(double(got.r) - want) < kColourEpsilon);
    }

    // A render distance between two settings lands between them rather than
    // snapping or diverging, and one outside the original's four is clamped
    // rather than dividing by zero. **The shorter distance is the redder one**
    // at noon, because the sky it is pulled towards has less red in it than the
    // fog does -- so eight chunks is furthest from the fog and four is nearest.
    const world::SkyColour six = world::viewFogColour(t, 0.0f, 6);
    const world::SkyColour eight = world::viewFogColour(t, 0.0f, 8);
    const world::SkyColour four = world::viewFogColour(t, 0.0f, 4);
    CHECK(six.r > eight.r);
    CHECK(six.r < four.r);
    CHECK(std::fabs(world::viewFogColour(t, 0.0f, 64).r - world::viewFogColour(t, 0.0f, 16).r)
          < 1e-6f);
    CHECK(std::fabs(world::viewFogColour(t, 0.0f, 1).r - world::viewFogColour(t, 0.0f, 2).r)
          < 1e-6f);
}

TEST(the_void_below_is_a_fifth_of_the_sky_and_bluer_than_it)
{
    const world::SkyColour sky = world::skyColour(6000);
    const world::SkyColour below = render::voidPlaneColour(sky);
    CHECK(std::fabs(below.r - (sky.r * 0.2f + 0.04f)) < 1e-6f);
    CHECK(std::fabs(below.g - (sky.g * 0.2f + 0.04f)) < 1e-6f);
    CHECK(std::fabs(below.b - (sky.b * 0.6f + 0.1f)) < 1e-6f);
    // Darker than the sky it hangs under, at noon and at midnight both.
    CHECK(below.r < sky.r);
    CHECK(below.b > below.r);
}

TEST(the_two_planes_are_grids_of_sixty_four_block_cells_above_and_below)
{
    int written = 0;
    const std::vector<mesh::DetailVertex> verts = builtSky(&written);
    CHECK_EQ(written, render::kSkyVertexCount);

    // 169 cells each, and the grid runs -384 to +448 in both axes.
    double minX = 1e9, maxX = -1e9, minZ = 1e9, maxZ = -1e9;
    for (int i = 0; i < render::kSkyPlaneVertices; ++i) {
        const mesh::DetailVertex& v = verts[usize(render::kSkyPlaneFirst + i)];
        CHECK_EQ(blocks(v.y), 16.0);
        minX = blocks(v.x) < minX ? blocks(v.x) : minX;
        maxX = blocks(v.x) > maxX ? blocks(v.x) : maxX;
        minZ = blocks(v.z) < minZ ? blocks(v.z) : minZ;
        maxZ = blocks(v.z) > maxZ ? blocks(v.z) : maxZ;
    }
    CHECK_EQ(minX, -384.0);
    CHECK_EQ(maxX, 448.0);
    CHECK_EQ(minZ, -384.0);
    CHECK_EQ(maxZ, 448.0);

    // The void plane is the same grid, mirrored in height and wound the other
    // way -- so its first quad's first two corners are the sky plane's second
    // and first.
    for (int i = 0; i < render::kVoidPlaneVertices; ++i) {
        CHECK_EQ(blocks(verts[usize(render::kVoidPlaneFirst + i)].y), -16.0);
    }
    const mesh::DetailVertex* sky = &verts[usize(render::kSkyPlaneFirst)];
    const mesh::DetailVertex* below = &verts[usize(render::kVoidPlaneFirst)];
    CHECK_EQ(below[0].x, sky[1].x);
    CHECK_EQ(below[0].z, sky[1].z);
    CHECK_EQ(below[1].x, sky[0].x);
    CHECK_EQ(below[1].z, sky[0].z);

    // Every cell is exactly 64 blocks square, which is what makes the fog fade
    // radially rather than across one enormous quad's diagonal.
    for (int q = 0; q < render::kSkyPlaneQuads; ++q) {
        const mesh::DetailVertex* cell = &verts[usize(render::kSkyPlaneFirst + q * 4)];
        CHECK_EQ(blocks(cell[1].x) - blocks(cell[0].x), 64.0);
        CHECK_EQ(blocks(cell[3].z) - blocks(cell[0].z), 64.0);
    }
}

TEST(the_sun_and_the_moon_hang_a_hundred_blocks_apart_on_their_own_pages)
{
    int written = 0;
    const std::vector<mesh::DetailVertex> verts = builtSky(&written);
    CHECK_EQ(written, render::kSkyVertexCount);

    const mesh::DetailVertex* sun = &verts[usize(render::kSunFirst)];
    const mesh::DetailVertex* moon = &verts[usize(render::kMoonFirst)];
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(blocks(sun[i].y), 100.0);
        CHECK_EQ(blocks(moon[i].y), -100.0);
        CHECK_EQ(std::fabs(blocks(sun[i].x)), 30.0);
        CHECK_EQ(std::fabs(blocks(sun[i].z)), 30.0);
        CHECK_EQ(std::fabs(blocks(moon[i].x)), 20.0);
        CHECK_EQ(std::fabs(blocks(moon[i].z)), 20.0);
    }

    // Each samples its own page of the entity sheet and nothing beyond it. The
    // moon's UVs are the sun's reversed, which is the original's winding.
    int sunX = 0, sunY = 0, moonX = 0, moonY = 0;
    texture::skinOrigin(texture::EntitySkin::Sun, &sunX, &sunY);
    texture::skinOrigin(texture::EntitySkin::Moon, &moonX, &moonY);
    const auto texelU = [](i16 u) {
        return double(u) * double(texture::kEntitySheetWidth) / double(mesh::kUvUnitsPerAtlas);
    };
    const auto texelV = [](i16 v) {
        return double(v) * double(texture::kEntitySheetHeight) / double(mesh::kUvUnitsPerAtlas);
    };
    for (int i = 0; i < 4; ++i) {
        CHECK(texelU(sun[i].u) >= double(sunX) - 0.5);
        CHECK(texelU(sun[i].u) <= double(sunX + texture::kCelestialPagePixels) + 0.5);
        CHECK(texelV(sun[i].v) >= double(sunY) - 0.5);
        CHECK(texelV(sun[i].v) <= double(sunY + texture::kCelestialPagePixels) + 0.5);
        CHECK(texelU(moon[i].u) >= double(moonX) - 0.5);
        CHECK(texelU(moon[i].u) <= double(moonX + texture::kCelestialPagePixels) + 0.5);
    }
    // Opposite corners: the sun's first corner is its page's top left, the
    // moon's first is its page's bottom right.
    CHECK_EQ(texelU(sun[0].u), double(sunX));
    CHECK_EQ(texelV(sun[0].v), double(sunY));
    CHECK_EQ(texelU(moon[0].u), double(moonX + texture::kCelestialPagePixels));
    CHECK_EQ(texelV(moon[0].v), double(moonY + texture::kCelestialPagePixels));
}

TEST(the_star_field_draws_the_same_random_stream_the_original_does)
{
    int written = 0;
    const std::vector<mesh::DetailVertex> verts = builtSky(&written);
    CHECK_EQ(written, render::kSkyVertexCount);

    // The count first: 780 of 1500 candidates survive, and a stream drawn in
    // the wrong order or short-circuited past a rejection would not land here.
    CHECK_EQ(render::kStarQuads, test::kStarCount);

    // Then the sampled centres. A star's four corners straddle its centre, so
    // the centre is their mean -- and comparing that against the original's is
    // what pins the rejection rule, the order of the four draws per candidate,
    // and the `nextDouble` that only a surviving candidate draws.
    for (int s = 0; s < test::kStarSampleCount; ++s) {
        const test::StarSample& want = test::kStarSamples[s];
        const mesh::DetailVertex* quad =
            &verts[usize(render::kStarsFirst + want.index * 4)];

        double cx = 0.0, cy = 0.0, cz = 0.0;
        double arm = 0.0;
        for (int i = 0; i < 4; ++i) {
            cx += blocks(quad[i].x) / 4.0;
            cy += blocks(quad[i].y) / 4.0;
            cz += blocks(quad[i].z) / 4.0;
        }
        for (int i = 0; i < 4; ++i) {
            const double dx = blocks(quad[i].x) - cx;
            const double dy = blocks(quad[i].y) - cy;
            const double dz = blocks(quad[i].z) - cz;
            arm += std::sqrt(dx * dx + dy * dy + dz * dz) / 4.0;
        }

        // A 64th of a block is the format's resolution and the corners are
        // rounded individually, so their mean can be half a unit out.
        constexpr double kUnit = 1.0 / double(render::kSkyUnitsPerBlock);
        CHECK(std::fabs(cx - want.x) < kUnit);
        CHECK(std::fabs(cy - want.y) < kUnit);
        CHECK(std::fabs(cz - want.z) < kUnit);
        // The size draw, recovered from the quad: the corners are `size` out
        // along both of the billboard's axes, so the arm is size * root two.
        CHECK(std::fabs(arm - want.size * 1.4142135623730951) < kUnit);
    }
}

TEST(every_star_sits_on_the_sphere_and_faces_the_middle_of_it)
{
    int written = 0;
    const std::vector<mesh::DetailVertex> verts = builtSky(&written);
    CHECK_EQ(written, render::kSkyVertexCount);

    for (int q = 0; q < render::kStarQuads; ++q) {
        const mesh::DetailVertex* star = &verts[usize(render::kStarsFirst + q * 4)];
        double cx = 0.0, cy = 0.0, cz = 0.0;
        for (int i = 0; i < 4; ++i) {
            cx += blocks(star[i].x) / 4.0;
            cy += blocks(star[i].y) / 4.0;
            cz += blocks(star[i].z) / 4.0;
        }
        // On the sphere of radius 100, to within the rounding the vertex format
        // costs: the corners are a fraction of a block out from the centre in
        // both directions, so their mean is the centre to within a 128th.
        const double radius = std::sqrt(cx * cx + cy * cy + cz * cz);
        CHECK(std::fabs(radius - double(render::kStarRadiusBlocks)) < 0.02);

        // Square, and in a plane through the centre: the diagonals are equal
        // and every corner is the same distance from the middle.
        double arms[4];
        for (int i = 0; i < 4; ++i) {
            const double dx = blocks(star[i].x) - cx;
            const double dy = blocks(star[i].y) - cy;
            const double dz = blocks(star[i].z) - cz;
            arms[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
            // Facing the origin: the arm is perpendicular to the line from
            // the camera, so their dot product is zero. **The bound is two and
            // not zero because of the units** -- each corner is rounded to a
            // 64th of a block and the radius it is dotted against is a
            // hundred, so a perfect quad still lands up to 1.4 out. Against a
            // full |arm| * |centre| of about 70, two is a degree and a half.
            const double dot = dx * cx + dy * cy + dz * cz;
            CHECK(std::fabs(dot) < 2.0);
        }
        for (int i = 1; i < 4; ++i) {
            CHECK(std::fabs(arms[i] - arms[0]) < 0.03);
        }
        // A quarter to a half of a block across the half-diagonal, which is
        // `0.25F + nextFloat() * 0.25F` times the root of two.
        CHECK(arms[0] > 0.25 * 1.414 - 0.03);
        CHECK(arms[0] < 0.5 * 1.414 + 0.03);
    }
}

TEST(the_whole_sky_fits_the_vertex_format_it_is_written_in)
{
    // The reason the sky has units of its own: 448 blocks does not fit the
    // detail format's 1/1024 of a block, and everything here has to survive the
    // narrowing to a signed short.
    int written = 0;
    const std::vector<mesh::DetailVertex> verts = builtSky(&written);
    CHECK_EQ(written, render::kSkyVertexCount);
    for (const mesh::DetailVertex& v : verts) {
        CHECK(std::fabs(blocks(v.x)) <= 512.0);
        CHECK(std::fabs(blocks(v.y)) <= 512.0);
        CHECK(std::fabs(blocks(v.z)) <= 512.0);
    }
    // And it refuses rather than overruns when the buffer is short.
    std::vector<mesh::DetailVertex> small(4);
    CHECK_EQ(render::buildSky(small.data(), 4), 0);
    CHECK_EQ(render::buildSky(nullptr, render::kSkyVertexCount), 0);
}
