// The flames over the screen while the player burns: two sheets off the two
// fire tiles, where they sit in camera space, and the one number this port
// changes -- how high they reach.
//
// See core/render/fire_overlay.hpp for the transcription of `jh.d(F)V`.

#include "core/render/fire_overlay.hpp"
#include "core/texture/texture_fx.hpp"
#include "framework.hpp"

#include <cmath>

using namespace mc;
using mc::render::buildFireOverlay;
using mc::render::kFireOverlayDrop;
using mc::render::kFireOverlayVertices;

namespace {

float blocks(i16 units) { return float(units) / float(mesh::kDetailUnitsPerBlock); }

bool near(float a, float b, float tolerance = 0.002f) { return std::fabs(a - b) <= tolerance; }

// Where a camera-space point lands on a 70-degree vertical field of view, as
// a fraction of the screen's height from the bottom.
float heightOnScreen(float y, float z)
{
    const float halfTan = std::tan(35.0f / 180.0f * 3.1415927f);
    return 0.5f + 0.5f * (y / -z) / halfTan;
}

}  // namespace

TEST(fire_overlay_is_two_sheets_off_the_two_flame_tiles)
{
    mesh::DetailVertex v[kFireOverlayVertices]{};
    CHECK_EQ(buildFireOverlay(v, kFireOverlayVertices), kFireOverlayVertices);

    // `Block.fire.blockIndexInTexture + i * 16`: the first sheet off the first
    // tile, the second off the one a row below it.
    for (int sheet = 0; sheet < 2; ++sheet) {
        const int tile = texture::flameTile(sheet);
        CHECK(tile >= 0);
        const int column = tile % mesh::kAtlasTilesPerEdge;
        const int row = tile / mesh::kAtlasTilesPerEdge;
        const mesh::DetailVertex* s = v + sheet * 4;
        for (int k = 0; k < 4; ++k) {
            CHECK(s[k].u == mesh::tileUvMin(column) || s[k].u == mesh::tileUvMax(column));
            CHECK(s[k].v == mesh::tileUvMin(row) || s[k].v == mesh::tileUvMax(row));
            // Full bright and white: `glColor4f(1, 1, 1, 0.9F)`, no lighting.
            CHECK_EQ(int(s[k].light), 0xFF);
            CHECK_EQ(int(s[k].r), 0xFF);
        }
        // **Mirrored**: the corner at low x carries the tile's right edge, and
        // the bottom of the tile is at the bottom of the sheet.
        CHECK(s[0].x < s[1].x);
        CHECK_EQ(s[0].u, mesh::tileUvMax(column));
        CHECK_EQ(s[1].u, mesh::tileUvMin(column));
        CHECK(s[0].y < s[3].y);
        CHECK_EQ(s[0].v, mesh::tileUvMax(row));
        CHECK_EQ(s[3].v, mesh::tileUvMin(row));
    }

    // Too small a buffer writes nothing rather than one sheet.
    CHECK_EQ(buildFireOverlay(v, kFireOverlayVertices - 1), 0);
    CHECK_EQ(buildFireOverlay(nullptr, kFireOverlayVertices), 0);
}

TEST(fire_overlay_sheets_lean_in_from_either_side)
{
    mesh::DetailVertex v[kFireOverlayVertices]{};
    CHECK_EQ(buildFireOverlay(v, kFireOverlayVertices), kFireOverlayVertices);

    // Sheet 0 is `translate(0.24, ...)` then `rotate(-10, 0, 1, 0)`: the right
    // one, its outer edge brought toward the eye. The corners are the rotated
    // (+-0.5, z = -0.5) plus the spread.
    const float c = std::cos(10.0f / 180.0f * 3.1415927f);
    const float s = std::sin(10.0f / 180.0f * 3.1415927f);
    CHECK(near(blocks(v[0].x), -0.5f * c + 0.5f * s + 0.24f));
    CHECK(near(blocks(v[0].z), -0.5f * c - 0.5f * s));
    CHECK(near(blocks(v[1].x), 0.5f * c + 0.5f * s + 0.24f));
    CHECK(near(blocks(v[1].z), -0.5f * c + 0.5f * s));

    // Sheet 1 is its mirror image in x, at the same depths the other way round.
    for (int k = 0; k < 4; ++k) {
        CHECK_EQ(v[4 + k].x, i16(-v[k ^ 1].x));
        CHECK_EQ(v[4 + k].z, v[k ^ 1].z);
        CHECK(v[k].z < 0);
    }
}

TEST(fire_overlay_stops_below_where_a1_1_2_put_it)
{
    mesh::DetailVertex v[kFireOverlayVertices]{};
    CHECK_EQ(buildFireOverlay(v, kFireOverlayVertices), kFireOverlayVertices);

    // A unit sheet centred on the drop: the top edge is half a block above it.
    const float top = blocks(v[2].y);
    CHECK(near(top, kFireOverlayDrop + 0.5f));
    CHECK(near(blocks(v[0].y), kFireOverlayDrop - 0.5f));

    // **Lower than the original**, whose -0.3 put the top at 0.2. At the inner
    // corner, the farthest from the eye and so the lowest on screen, that was
    // three quarters of the way up; the port keeps the top under two thirds
    // everywhere on the sheet, including the near corner off the screen's edge.
    CHECK(heightOnScreen(0.2f, blocks(v[3].z)) > 0.74f);
    CHECK(heightOnScreen(top, blocks(v[3].z)) < 0.64f);
    CHECK(heightOnScreen(top, blocks(v[2].z)) < 0.70f);
    // And still over the lower half of the view, so it reads as being alight.
    CHECK(heightOnScreen(top, blocks(v[3].z)) > 0.55f);
}
