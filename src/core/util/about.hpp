#pragma once

// What this build is, and whose work it carries.
//
// **Why the notice is here rather than only in docs/licences.md.** Two of the
// licences below are conditions on handing someone a *binary*, not conditions
// on publishing a repository: zlib's third clause and BSD-3's second both ask
// that the notice reach whoever was given the program. A file in the tree
// satisfies that only for as long as everyone who has the game also has the
// tree, which stops being true the moment a `.3dsx` is posted anywhere. So the
// notice travels inside the binary -- a `const char[]` in `.rodata` that
// nothing can separate from the code it covers -- and the Info row on the
// Options screen is where a player reads it. docs/licences.md records that
// decision and the one it replaced.
//
// **Why it is in core rather than beside the menu that draws it.** The list is
// a fact about the build and not about the 3DS: the host build links the same
// zlib and the same Vorbis decoder, and `MC_HAVE_VORBIS` decides for both
// whether the Xiph notice is owed at all. Core is also the only side a test can
// reach -- `src/platform/ctr` does not build on the host, and a notice that
// silently lost an entry is exactly the kind of thing no one would see.
//
// **Keep this in step with docs/licences.md**; tests/about_test.cpp checks the
// shape of the entries but cannot check that they are the right ones.

#include "core/util/span.hpp"

// Set by CMake from `project(3DAlpha VERSION ...)`, which is the one place the
// client's version number is written down. The fallback is for a translation
// unit parsed outside the build -- an editor's index, say -- and is deliberately
// not a plausible version: a build that shows it is misconfigured, and saying so
// on the screen beats quietly claiming to be 0.1.0.
#ifndef MC_APP_VERSION
#define MC_APP_VERSION "unconfigured"
#endif

namespace mc::about {

// The client's own version. Not the Minecraft version it implements -- that is
// `mcver::kDisplay`, it is a different number, and it always will be.
inline constexpr char kVersion[] = MC_APP_VERSION;

// **The sentence that has to be on the screen even though no licence asks for
// it.** Everything else in this file is somebody's copyright in our binary;
// this one is the opposite -- our binary next to somebody's trademark. It is
// here rather than in the menu because it is a fact about what this build *is*,
// and because the one place it must never quietly disappear from is the screen
// a player checks.
inline constexpr char kDisclaimer[] =
    "3DAlpha is an independent reimplementation of Minecraft Alpha 1.1.2, written from scratch. "
    "It is not made by, endorsed by or affiliated with Mojang or Microsoft, and it contains no "
    "game files of theirs -- the textures and sounds it draws and plays are the ones you put on "
    "your own SD card.";

// One work this binary carries something of, as one line of the credits.
struct Credit {
    // Who to name. The person or project, as they call themselves.
    const char* who;
    // What of theirs is in here, in a player's words rather than a lawyer's.
    const char* what;
    // The licence that asks for the naming, or null for a credit that is owed
    // as courtesy rather than as a condition -- documentation we read, an
    // algorithm we implemented, a toolchain we built with. The distinction
    // matters: an entry with a licence cannot be dropped from a distribution,
    // and an entry without one is ours to word as we like.
    const char* licence;
};

// Everyone the Info screen names, in the order it names them: the licensed
// entries first, because those are the ones that have to be there.
Span<const Credit> credits();

// The notices that have to be reproduced in full rather than summarised, one
// block of text each, already in the order `credits()` introduces them.
//
// Empty entries are impossible by construction; a build without Vorbis simply
// has one block fewer, which is what its licence asks for -- nothing of Xiph's
// is in that binary to carry a notice for.
Span<const char* const> notices();

}  // namespace mc::about
