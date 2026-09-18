// The name over another player's head, and the one thing about it that a
// billboard cannot be eyeballed for: which way the text reads.
//
// `EffectRenderer`'s basis -- `(cos yaw, 0, sin yaw)` -- is the vector a1.1.2
// hands `EntityFX.renderParticle`, and it points at the camera's **left**: at
// yaw 0 the camera looks along +Z, whose right hand is -X, and the basis gives
// +X. The jar knows it and pairs its low u with the `-right` corners, which is
// invisible on a four-pixel chip of stone and very visible on a word. Written
// along the basis instead of against it, every name was a mirror of itself.

#include "framework.hpp"

#include "core/net/entities.hpp"
#include "core/render/remote_player_mesh.hpp"
#include "core/texture/font.hpp"

#include <vector>

using namespace mc;

namespace {

texture::FontImage evenFont()
{
    texture::FontImage font;
    font.rgba.assign(texture::kFontBytes, 0);
    for (int i = 0; i < 256; ++i) {
        font.widths[i] = 6;
    }
    return font;
}

// One player standing at the origin, named `name`, close enough to be drawn.
net::RemoteEntities oneNamed(const char* name)
{
    net::RemoteEntities entities;
    net::Packet spawn;
    spawn.reset(net::packet::NamedEntitySpawn);
    spawn.pushInt(net::kFirstPlayerEntityId + 1);
    spawn.pushString(name);
    spawn.pushInt(0);  // x, y, z in 1/32 of a block
    spawn.pushInt(0);
    spawn.pushInt(0);
    spawn.pushInt(0);  // yaw, pitch
    spawn.pushInt(0);
    spawn.pushInt(0);  // held item
    entities.apply(spawn, nullptr);
    return entities;
}

}  // namespace

TEST(a_nametag_reads_left_to_right_across_the_camera)
{
    const texture::FontImage font = evenFont();
    net::RemoteEntities entities = oneNamed("AB");

    // Yaw 0: the camera looks along +Z, so its right is -X and the billboard's
    // own horizontal -- which is what this is handed -- is +X.
    const render::Billboard basis{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

    std::vector<mesh::DetailVertex> out(64);
    const int written = render::buildNameTags(entities, font, basis, 0.0, 0.0, 0.0, 0.0f,
                                              out.data(), int(out.size()));
    CHECK_EQ(written, 8);  // two glyphs, four corners each

    // The first glyph is the first four vertices and the second glyph the next
    // four. Reading order is screen left to right, and screen right at yaw 0
    // is -X -- so the second glyph must sit at a *smaller* x than the first.
    i16 firstX = out[0].x;
    i16 secondX = out[4].x;
    for (int i = 0; i < 4; ++i) {
        firstX = out[i].x < firstX ? out[i].x : firstX;
        secondX = out[4 + i].x < secondX ? out[4 + i].x : secondX;
    }
    CHECK(secondX < firstX);

    // And each glyph is not mirrored within itself: its low u is on its own
    // screen-left edge, which is the larger x.
    CHECK(out[0].u < out[1].u);
    CHECK(out[0].x > out[1].x);
}

TEST(a_nametag_is_drawn_the_right_way_up)
{
    const texture::FontImage font = evenFont();
    net::RemoteEntities entities = oneNamed("A");

    const render::Billboard basis{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::vector<mesh::DetailVertex> out(16);
    const int written = render::buildNameTags(entities, font, basis, 0.0, 0.0, 0.0, 0.0f,
                                              out.data(), int(out.size()));
    CHECK_EQ(written, 4);

    // The font sheet's v grows downward, so the top of the glyph -- the small
    // v -- has to land at the larger y.
    CHECK(out[0].v < out[3].v);
    CHECK(out[0].y > out[3].y);

    // Above the head rather than through it: `kNameTagHeight` over the feet.
    CHECK(out[0].y > 0);
}

TEST(a_nametag_wears_the_colour_that_player_wears_everywhere_else)
{
    const texture::FontImage font = evenFont();
    net::RemoteEntities entities = oneNamed("A");

    const render::Billboard basis{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::vector<mesh::DetailVertex> out(16);
    CHECK_EQ(render::buildNameTags(entities, font, basis, 0.0, 0.0, 0.0, 0.0f, out.data(),
                                   int(out.size())),
             4);

    u8 r = 0;
    u8 g = 0;
    u8 b = 0;
    net::playerColour(net::kFirstPlayerEntityId + 1, &r, &g, &b);
    CHECK_EQ(int(out[0].r), int(r));
    CHECK_EQ(int(out[0].g), int(g));
    CHECK_EQ(int(out[0].b), int(b));
}
