#include "framework.hpp"

#include "core/util/about.hpp"

#include <cstring>

using namespace mc;

namespace {

bool present(const char* text)
{
    return text != nullptr && text[0] != '\0';
}

int licensedCredits()
{
    int count = 0;
    for (const about::Credit& credit : about::credits()) {
        if (credit.licence != nullptr) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// The version reaches the binary from CMake and from nowhere else, so the thing
// worth testing is the wiring rather than the number: a build that lost
// -DMC_APP_VERSION still compiles, and the header's fallback is the only sign.
TEST(the_version_comes_from_the_build)
{
    CHECK(present(about::kVersion));
    CHECK(std::strcmp(about::kVersion, "unconfigured") != 0);
}

TEST(every_credit_names_someone_and_something)
{
    CHECK(!about::credits().empty());
    for (const about::Credit& credit : about::credits()) {
        CHECK(present(credit.who));
        CHECK(present(credit.what));
        // A licence is optional; an empty one is not the same thing as none and
        // would print as "()" on the screen.
        CHECK(credit.licence == nullptr || present(credit.licence));
    }
}

// The ordering the Info screen relies on to read as one list rather than two:
// everything that is a condition of distributing the binary comes before
// everything that is a courtesy.
TEST(licensed_credits_come_first)
{
    bool seenCourtesy = false;
    for (const about::Credit& credit : about::credits()) {
        if (credit.licence == nullptr) {
            seenCourtesy = true;
        } else {
            CHECK(!seenCourtesy);
        }
    }
}

// **The invariant that makes the screen a notice rather than a credits roll.**
// Naming a licence on a credit is a promise that its text is reproduced below;
// adding one without the other is the mistake that would leave the binary
// owing a notice it claims to carry, and it is invisible on a 240-line screen.
TEST(each_named_licence_has_its_notice)
{
    CHECK_EQ(int(about::notices().size()), licensedCredits());
}

// Not a length check for its own sake. Every notice here is a licence that asks
// to be reproduced in full, and each is well over a thousand characters; a
// truncation -- a stray snprintf, a buffer someone sized for a tooltip -- lands
// far below this.
TEST(no_notice_has_been_truncated)
{
    CHECK(!about::notices().empty());
    for (const char* notice : about::notices()) {
        CHECK(present(notice));
        CHECK(std::strlen(notice) > 200);
    }
}

// The Vorbis decoder is optional, and its licence asks for a notice only from a
// binary that carries its code. Both halves are checked: a build with it must
// name Xiph, and a build without it must not -- a notice for code that is not
// there is a claim about the binary that is not true.
TEST(the_xiph_notice_tracks_the_build)
{
    bool named = false;
    for (const about::Credit& credit : about::credits()) {
        if (std::strstr(credit.who, "Xiph") != nullptr) {
            named = true;
        }
    }
#if MC_HAVE_VORBIS
    CHECK(named);
#else
    CHECK(!named);
#endif
}

// The one line on the screen that is there for Mojang's sake rather than for a
// licence holder's. It has no licence behind it to make its absence fail
// anything else, which is exactly why it is pinned here.
TEST(the_disclaimer_says_what_it_has_to)
{
    CHECK(present(about::kDisclaimer));
    CHECK(std::strstr(about::kDisclaimer, "Mojang") != nullptr);
    CHECK(std::strstr(about::kDisclaimer, "not made by") != nullptr);
}
