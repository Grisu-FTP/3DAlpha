// The Controls row: three schemes, and the pairing each one makes.
//
// The claims worth a test are the ones the row would be useless without -- that
// no scheme asks one device to walk and look at once, that the two Old schemes
// really are each other swapped, and that a word in `3ds.ini` survives a round
// trip. See core/settings/control_scheme.hpp.

#include "framework.hpp"

#include "core/settings/control_scheme.hpp"

#include <string>

using namespace mc;
using namespace mc::settings;

namespace {

const ControlScheme kAll[] = {
    ControlScheme::New3DS,
    ControlScheme::Old3DS,
    ControlScheme::Old3DSAlt,
};

}  // namespace

TEST(every_control_scheme_walks_and_looks_with_two_different_devices)
{
    // The one invariant the whole row rests on. A scheme that moved and looked
    // off the same stick would be a scheme in which the player cannot stand
    // still and turn, which is not a control scheme at all.
    CHECK_EQ(int(sizeof(kAll) / sizeof(kAll[0])), kControlSchemeCount);
    for (const ControlScheme scheme : kAll) {
        CHECK(moveStick(scheme) != lookStick(scheme));
    }
}

TEST(the_new_3ds_scheme_is_the_pairing_this_port_shipped_with)
{
    // Circle pad walks, C-stick looks. Stated here so a later rearrangement of
    // the enum cannot quietly change what an existing `3ds.ini` means.
    CHECK(moveStick(ControlScheme::New3DS) == Stick::CirclePad);
    CHECK(lookStick(ControlScheme::New3DS) == Stick::CStick);
}

TEST(the_alt_scheme_is_the_old_3ds_scheme_with_its_two_devices_swapped)
{
    // What the row's name promises, and the only difference between them.
    CHECK(moveStick(ControlScheme::Old3DSAlt) == lookStick(ControlScheme::Old3DS));
    CHECK(lookStick(ControlScheme::Old3DSAlt) == moveStick(ControlScheme::Old3DS));
}

TEST(neither_old_3ds_scheme_asks_for_a_c_stick)
{
    // The point of both of them: an Old 3DS has no C-stick, so a scheme that
    // reached for one would leave that console exactly where it was.
    CHECK(moveStick(ControlScheme::Old3DS) != Stick::CStick);
    CHECK(lookStick(ControlScheme::Old3DS) != Stick::CStick);
    CHECK(moveStick(ControlScheme::Old3DSAlt) != Stick::CStick);
    CHECK(lookStick(ControlScheme::Old3DSAlt) != Stick::CStick);
}

TEST(a_control_scheme_survives_the_round_trip_through_its_token)
{
    for (const ControlScheme scheme : kAll) {
        ControlScheme parsed = ControlScheme::New3DS;
        CHECK(controlSchemeFromToken(controlSchemeToken(scheme), &parsed));
        CHECK(parsed == scheme);
    }
}

TEST(an_unknown_controls_token_leaves_the_callers_default_alone)
{
    // A hand-edited card, or a file written by a later build that added a
    // scheme this one has never heard of. The row keeps what it had rather than
    // landing on whichever scheme happens to be ordinal zero.
    ControlScheme scheme = ControlScheme::Old3DSAlt;
    CHECK(!controlSchemeFromToken("", &scheme));
    CHECK(!controlSchemeFromToken("old3ds ", &scheme));
    CHECK(!controlSchemeFromToken("wii-u", &scheme));
    CHECK(scheme == ControlScheme::Old3DSAlt);
}

TEST(control_scheme_tokens_and_labels_are_distinct_and_non_empty)
{
    for (int i = 0; i < kControlSchemeCount; ++i) {
        for (int j = i + 1; j < kControlSchemeCount; ++j) {
            CHECK(std::string(controlSchemeToken(ControlScheme(i)))
                  != controlSchemeToken(ControlScheme(j)));
            CHECK(std::string(controlSchemeLabel(ControlScheme(i)))
                  != controlSchemeLabel(ControlScheme(j)));
        }
        CHECK(!std::string(controlSchemeToken(ControlScheme(i))).empty());
        CHECK(!std::string(controlSchemeLabel(ControlScheme(i))).empty());
    }
    CHECK(std::string(stickLabel(Stick::CirclePad)) != stickLabel(Stick::Dpad));
    CHECK(std::string(stickLabel(Stick::CStick)) != stickLabel(Stick::Dpad));
}

TEST(a_console_with_no_saved_scheme_gets_the_one_named_after_it)
{
    // Which is a change for an Old 3DS whose `3ds.ini` predates this row: it
    // was steering with one stick and looking with a finger, and the row is
    // there because that is not how the game is played.
    CHECK(defaultControlScheme(true) == ControlScheme::New3DS);
    CHECK(defaultControlScheme(false) == ControlScheme::Old3DS);
}
