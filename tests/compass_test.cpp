// The compass needle.
//
// **A compass in a1.1.2 is a texture and nothing else** -- item 345 is a plain
// `di` with no `onItemUse` and no `onItemRightClick` -- so the thing to test is
// `aa.a()`, the FX that rewrites its 16 x 16 icon every tick. See
// core/texture/compass_fx.hpp.
//
// There *is* an oracle for the geometry here, unlike the flame: the needle is a
// pure function of one angle and the angle is a pure function of four numbers,
// with no `Math.random()` anywhere. So these check the real properties -- that
// the needle points at the spawn, that turning the player turns it the other
// way, that the spring settles rather than snapping, and that the pack's own
// dial survives underneath it.

#include "core/item/registry.hpp"
#include "core/texture/compass_fx.hpp"
#include "framework.hpp"

#include <cmath>

using namespace mc;
using mc::texture::CompassTexture;

namespace {

constexpr int kEdge = 16;

// A base tile that is a flat, fully opaque colour, so anything the needle
// writes is distinguishable from what it was drawn over.
struct Dial {
    u8 pixels[kEdge * kEdge * 4];
    Dial()
    {
        for (int i = 0; i < kEdge * kEdge; ++i) {
            pixels[i * 4 + 0] = 8;
            pixels[i * 4 + 1] = 9;
            pixels[i * 4 + 2] = 10;
            pixels[i * 4 + 3] = 255;
        }
    }
};

bool isRed(const u8* p) { return p[0] == 255 && p[1] == 20 && p[2] == 20; }
bool isGrey(const u8* p) { return p[0] == 100 && p[1] == 100 && p[2] == 100; }
bool isDial(const u8* p) { return p[0] == 8 && p[1] == 9 && p[2] == 10; }

const u8* at(const CompassTexture& c, int x, int y)
{
    return c.texels() + (usize(y) * kEdge + usize(x)) * 4;
}

// Where the red half of the needle ends up, as the mean of its texels. The
// needle is drawn from -8 to +16 about the centre with only the non-negative
// half red, so this is a direction and not just a position.
void redCentre(const CompassTexture& c, double* outX, double* outY)
{
    double sx = 0.0, sy = 0.0;
    int n = 0;
    for (int y = 0; y < kEdge; ++y) {
        for (int x = 0; x < kEdge; ++x) {
            if (isRed(at(c, x, y))) {
                sx += x;
                sy += y;
                ++n;
            }
        }
    }
    *outX = n > 0 ? sx / n : -1.0;
    *outY = n > 0 ? sy / n : -1.0;
}

// Run the spring to rest. Two hundred steps is far past the point where a
// damping of 0.8 has stopped moving anything.
void settle(CompassTexture* c, int spawnX, int spawnZ, double px, double pz, float yaw)
{
    for (int i = 0; i < 200; ++i) {
        c->tick(spawnX, spawnZ, px, pz, yaw);
    }
}

}  // namespace

TEST(an_unticked_compass_is_the_packs_own_dial)
{
    Dial dial;
    CompassTexture compass;
    compass.setBase(dial.pixels);

    CHECK(!compass.ready());
    // Every texel, because a needle drawn before the first tick would mean the
    // FX had state it was never given.
    for (int i = 0; i < kEdge * kEdge; ++i) {
        CHECK(isDial(compass.texels() + usize(i) * 4));
    }
}

TEST(the_needle_is_drawn_over_the_dial_and_does_not_erase_it)
{
    Dial dial;
    CompassTexture compass;
    compass.setBase(dial.pixels);
    settle(&compass, 100, 0, 0.0, 0.0, 0.0f);

    CHECK(compass.ready());

    int red = 0, grey = 0, kept = 0;
    for (int i = 0; i < kEdge * kEdge; ++i) {
        const u8* p = compass.texels() + usize(i) * 4;
        red += isRed(p) ? 1 : 0;
        grey += isGrey(p) ? 1 : 0;
        kept += isDial(p) ? 1 : 0;
    }
    // The two loops write 9 + 25 texels, several of which land on each other,
    // so the count is a range rather than a number -- but most of a 256-texel
    // tile has to survive or the needle is not a needle.
    CHECK(red > 0);
    CHECK(grey > 0);
    CHECK(kept > 200);
}

TEST(the_needle_turns_to_face_the_spawn)
{
    Dial dial;

    // The player at the origin looking along the original's yaw 0, with the
    // spawn a long way off along each axis in turn. Far enough that atan2 is
    // dominated by the one axis.
    struct Case {
        int spawnX, spawnZ;
    };
    const Case cases[] = {{1000, 0}, {-1000, 0}, {0, 1000}, {0, -1000}};

    double cx[4], cy[4];
    for (int i = 0; i < 4; ++i) {
        CompassTexture compass;
        compass.setBase(dial.pixels);
        settle(&compass, cases[i].spawnX, cases[i].spawnZ, 0.0, 0.0, 0.0f);
        redCentre(compass, &cx[i], &cy[i]);
        // Something red exists in every one of them.
        CHECK(cx[i] >= 0.0);
    }

    // Four different spawn directions put the red end in four different
    // places. The absolute direction is the original's convention and is not
    // re-derived here; what is checked is that the needle *distinguishes* them,
    // which is the property a compass has and a static icon does not.
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            const double dx = cx[i] - cx[j];
            const double dy = cy[i] - cy[j];
            CHECK(std::sqrt(dx * dx + dy * dy) > 1.0);
        }
    }

    // And opposite spawns put it on opposite sides of the centre, which is the
    // one absolute statement that holds whatever the sign conventions are.
    CHECK((cx[0] - 8.5) * (cx[1] - 8.5) < 0.0);
    CHECK((cy[2] - 7.5) * (cy[3] - 7.5) < 0.0);
}

TEST(turning_the_player_turns_the_needle_the_other_way)
{
    Dial dial;

    // The same spawn, seen from the same place, with the player facing two
    // directions ninety degrees apart. `rotationYaw` is in the target angle, so
    // the needle is relative to the player's facing -- which is what makes a
    // compass usable while walking rather than a north arrow.
    CompassTexture a, b;
    a.setBase(dial.pixels);
    b.setBase(dial.pixels);
    settle(&a, 1000, 0, 0.0, 0.0, 0.0f);
    settle(&b, 1000, 0, 0.0, 0.0, 90.0f);

    double ax, ay, bx, by;
    redCentre(a, &ax, &ay);
    redCentre(b, &bx, &by);
    CHECK(ax >= 0.0);
    CHECK(bx >= 0.0);
    const double dx = ax - bx;
    const double dy = ay - by;
    CHECK(std::sqrt(dx * dx + dy * dy) > 1.0);
}

TEST(the_spring_settles_rather_than_snapping)
{
    Dial dial;
    CompassTexture compass;
    compass.setBase(dial.pixels);

    // One step is not enough to arrive: the spring takes a tenth of the error
    // and keeps four fifths of its velocity, so a compass that jumped straight
    // to its target on the first tick would mean the damping was not ported.
    compass.tick(1000, 0, 0.0, 0.0, 0.0f);
    double firstX, firstY;
    redCentre(compass, &firstX, &firstY);

    settle(&compass, 1000, 0, 0.0, 0.0, 0.0f);
    double restX, restY;
    redCentre(compass, &restX, &restY);

    CHECK(firstX >= 0.0);
    CHECK(restX >= 0.0);
    const double dx = firstX - restX;
    const double dy = firstY - restY;
    CHECK(std::sqrt(dx * dx + dy * dy) > 0.5);

    // ...and once settled it stays settled, which is the other half of a
    // damped spring and the half an undamped one fails.
    for (int i = 0; i < 40; ++i) {
        compass.tick(1000, 0, 0.0, 0.0, 0.0f);
    }
    double laterX, laterY;
    redCentre(compass, &laterX, &laterY);
    CHECK(std::fabs(laterX - restX) < 0.01);
    CHECK(std::fabs(laterY - restY) < 0.01);
}

TEST(standing_on_the_spawn_still_produces_a_needle)
{
    // `atan2(0, 0)` is 0 in Java and in C++, so this is defined rather than
    // NaN -- but it is the case a compass is most often looked at in, and a
    // needle that vanished at the spawn point would be the bug nobody could
    // reproduce anywhere else.
    Dial dial;
    CompassTexture compass;
    compass.setBase(dial.pixels);
    settle(&compass, 0, 0, 0.0, 0.0, 0.0f);

    double x, y;
    redCentre(compass, &x, &y);
    CHECK(x >= 0.0);
}

TEST(exactly_one_item_carries_the_animated_icon)
{
    // The generator sets `animatedIcon` from the FX class's own tile and sheet.
    // a1.1.2 registers six TextureFX and only the compass is on the items
    // sheet -- there is no clock in this version -- so a second flagged item
    // would mean the derivation had gone wrong rather than that the game had
    // changed.
    int flagged = 0;
    int lastIcon = -1;
    for (int id = 0; id < mcver::kItemTableSize; ++id) {
        const item::ItemDef& def = mcver::kItems[id];
        if (def.known && def.animatedIcon) {
            ++flagged;
            lastIcon = int(def.icon);
            CHECK(def.sheet == item::IconSheet::Items);
        }
    }
    CHECK_EQ(flagged, 1);
    CHECK_EQ(texture::compassTile(), lastIcon);
}

TEST(a_settled_compass_stops_asking_to_be_pushed)
{
    // **The reason `tick` has a return value at all.** Both consumers are
    // expensive -- one is a texture upload, the other repaints the whole hotbar
    // band on a framebuffer the LCD is scanning out of -- and the spring keeps
    // converging in the eleventh digit long after the last texel has stopped
    // moving. So the interesting property is not that it settles but that it
    // says so.
    Dial dial;
    CompassTexture compass;
    compass.setBase(dial.pixels);

    // The first tick always reports: nothing has been pushed yet.
    CHECK(compass.tick(100, 40, 0.0, 0.0, 0.0f));

    settle(&compass, 100, 40, 0.0, 0.0, 0.0f);

    // Nothing about the world has changed, so nothing about the face can.
    for (int i = 0; i < 20; ++i) {
        CHECK(!compass.tick(100, 40, 0.0, 0.0, 0.0f));
    }
}

TEST(a_turn_asks_to_be_pushed_and_a_new_pack_does_too)
{
    Dial dial;
    CompassTexture compass;
    compass.setBase(dial.pixels);
    settle(&compass, 100, 40, 0.0, 0.0, 0.0f);
    CHECK(!compass.tick(100, 40, 0.0, 0.0, 0.0f));

    // Turning a quarter of the way round moves the needle by more than a texel,
    // so somewhere in the swing there has to be a step that reports it. Not
    // every step: the spring converges, and once it is inside a texel again the
    // answer is no.
    bool moved = false;
    for (int i = 0; i < 20; ++i) {
        moved = compass.tick(100, 40, 0.0, 0.0, 90.0f) || moved;
    }
    CHECK(moved);

    // A new pack is a new face under the needle even where the needle has not
    // moved, and what the consumers are holding is the old pack's.
    settle(&compass, 100, 40, 0.0, 0.0, 90.0f);
    CHECK(!compass.tick(100, 40, 0.0, 0.0, 90.0f));

    Dial other;
    for (int i = 0; i < kEdge * kEdge; ++i) {
        other.pixels[i * 4 + 0] = 200;
    }
    compass.setBase(other.pixels);
    CHECK(compass.tick(100, 40, 0.0, 0.0, 90.0f));
}
