// Under water: the fog's colour, mode and brightness (`iq.i(F)`, `iq.a(I)`,
// `iq.a()`), and the sheet of `water.png` over the view (`jh.c(F)`).
//
// The constants are the jar's, read off the bytecode; what is checked here is
// the arithmetic around them -- the ease, the render-distance lift, the UV
// scroll and its wrap -- since those are where a transcription goes wrong.

#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/render/water_overlay.hpp"
#include "core/texture/water_overlay_image.hpp"
#include "core/world/view_fog.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace mc;

namespace {

constexpr double kEpsilon = 1e-5;

bool near(double a, double b, double epsilon = kEpsilon)
{
    return std::fabs(a - b) <= epsilon;
}

}  // namespace

TEST(fog_brightness_eases_a_tenth_of_the_way_from_zero)
{
    world::FogBrightness fog;
    // Tiny: the lift is zero, so the target is the light itself.
    fog.tick(0.5f, 2);
    CHECK(near(fog.previous, 0.0));
    CHECK(near(fog.current, 0.05));
    fog.tick(0.5f, 2);
    CHECK(near(fog.previous, 0.05));
    CHECK(near(fog.current, 0.05 + 0.45 * 0.1));
    CHECK(near(fog.at(0.5f), (fog.previous + fog.current) / 2.0));
}

TEST(fog_brightness_ignores_the_dark_at_far_and_follows_it_at_tiny)
{
    world::FogBrightness far;
    world::FogBrightness normal;
    world::FogBrightness tiny;
    for (int i = 0; i < 400; ++i) {
        far.tick(0.05f, 16);
        normal.tick(0.05f, 8);
        tiny.tick(0.05f, 2);
    }
    // `(3 - renderDistance) / 3` is 1 at Far: always full brightness.
    CHECK(near(far.current, 1.0, 1e-4));
    // 2/3 at Normal: 0.05 * 1/3 + 2/3.
    CHECK(near(normal.current, 0.05 / 3.0 + 2.0 / 3.0, 1e-4));
    CHECK(near(tiny.current, 0.05, 1e-4));
}

TEST(the_head_in_water_or_lava_replaces_the_fog_and_makes_it_exponential)
{
    const world::ViewFog water =
        world::viewFog(6000, 0.0f, 8, world::FogMedium::Water, 0.5f);
    CHECK(near(water.colour.r, 0.01));
    CHECK(near(water.colour.g, 0.01));
    CHECK(near(water.colour.b, 0.1));
    CHECK(near(water.density, 0.1));

    const world::ViewFog lava = world::viewFog(6000, 0.0f, 8, world::FogMedium::Lava, 1.0f);
    CHECK(near(lava.colour.r, 0.6));
    CHECK(near(lava.colour.g, 0.1));
    CHECK(near(lava.colour.b, 0.0));
    CHECK(near(lava.density, 2.0));

    // Out of both, the ordinary lerp -- scaled by the brightness too -- and
    // the ordinary line.
    const world::ViewFog air = world::viewFog(6000, 0.0f, 8, world::FogMedium::Air, 0.5f);
    const world::SkyColour plain = world::viewFogColour(6000, 0.0f, 8);
    CHECK(near(air.colour.r, plain.r * 0.5));
    CHECK(near(air.colour.b, plain.b * 0.5));
    CHECK_EQ(air.density, 0.0f);
}

TEST(the_water_overlay_covers_the_view_mirrored_and_four_times_over)
{
    mesh::DetailVertex v[render::kWaterOverlayVertices];
    CHECK_EQ(render::buildWaterOverlayQuad(0.0f, 0.0f, 0xF0, v, 4), 4);
    CHECK_EQ(render::buildWaterOverlayQuad(0.0f, 0.0f, 0xF0, v, 3), 0);
    CHECK_EQ(render::buildWaterOverlayQuad(0.0f, 0.0f, 0xF0, v, 4), 4);

    const int unit = mesh::kDetailUnitsPerBlock;
    for (const mesh::DetailVertex& c : v) {
        CHECK_EQ(int(c.z), -unit / 2);
        CHECK_EQ(std::abs(int(c.x)), unit);
        CHECK_EQ(std::abs(int(c.y)), unit);
        CHECK_EQ(int(c.light), 0xF0);
    }
    // Low x takes the high u -- the four repeats run right to left -- and the
    // bottom takes the high v. One sheet is all four repeats.
    CHECK_EQ(int(v[0].u) - int(v[1].u), 16384);
    CHECK_EQ(int(v[0].v) - int(v[3].v), 16384);
    CHECK(v[0].x < v[1].x);
    CHECK(v[0].y < v[3].y);
}

TEST(the_water_overlay_scrolls_with_the_view_and_wraps_a_whole_repeat)
{
    mesh::DetailVertex still[4];
    mesh::DetailVertex turned[4];
    mesh::DetailVertex around[4];
    render::buildWaterOverlayQuad(0.0f, 0.0f, 0xFF, still, 4);
    // A sixty-fourth of a repeat per degree: 16 degrees is a quarter repeat,
    // a sixteenth of the sheet.
    render::buildWaterOverlayQuad(-16.0f, 0.0f, 0xFF, turned, 4);
    CHECK_EQ(int(turned[1].u) - int(still[1].u), 1024);
    render::buildWaterOverlayQuad(0.0f, 16.0f, 0xFF, turned, 4);
    CHECK_EQ(int(turned[1].v) - int(still[1].v), 1024);

    // 64 degrees is a whole repeat, which is the same picture.
    render::buildWaterOverlayQuad(64.0f, -128.0f, 0xFF, around, 4);
    CHECK(std::memcmp(still, around, sizeof(still)) == 0);

    // Yaw is never wrapped; a player who has spun for an hour still fits.
    render::buildWaterOverlayQuad(-123456.0f, 89.9f, 0xFF, around, 4);
    for (const mesh::DetailVertex& c : around) {
        CHECK(c.u >= 0 && c.u <= 16384 + 4096);
        CHECK(c.v >= 0 && c.v <= 16384 + 4096);
    }
}

TEST(a_pack_without_water_png_still_gets_a_repeated_blue_sheet)
{
    io::PosixFileSystem fs;
    std::vector<u8> sheet;
    texture::buildWaterOverlay(fs, std::string(), &sheet);
    CHECK_EQ((long long) sheet.size(), (long long) texture::kWaterOverlayBytes);

    std::vector<u8> standIn;
    texture::buildDevArtWaterOverlay(&standIn);
    CHECK(sheet == standIn);

    // Every texel matches the one a whole tile up and left of it, and every
    // one is blue and see-through.
    const int edge = texture::kWaterOverlayEdge;
    const int tile = texture::kWaterOverlayTileEdge;
    for (int y = 0; y < edge; ++y) {
        for (int x = 0; x < edge; ++x) {
            const u8* t = sheet.data() + (usize(y) * usize(edge) + usize(x)) * 4;
            const u8* base = sheet.data() + (usize(y % tile) * usize(edge) + usize(x % tile)) * 4;
            CHECK(std::memcmp(t, base, 4) == 0);
            CHECK(t[2] > t[1] && t[1] > t[0]);
            CHECK(t[3] > 96 && t[3] < 192);
        }
    }
}
