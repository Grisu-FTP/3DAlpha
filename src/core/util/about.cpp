// The credits and the notices themselves. See about.hpp for why they live in
// the binary, and docs/licences.md for the same list in prose.
//
// **Every notice below is transcribed from the licence file the installed
// package actually ships**, not from memory and not from the upstream website:
//
//   $DEVKITPRO/portlibs/3ds/licenses/3ds-zlib/LICENSE
//   $DEVKITPRO/portlibs/3ds/licenses/3ds-libvorbisidec/COPYING
//   $DEVKITPRO/portlibs/3ds/licenses/3ds-libogg/COPYING
//
// That is not pedantry. Checked on 2026-09-18, two of them differed from what
// docs/licences.md had recorded -- zlib's first restriction carries a sentence
// about acknowledgment that had been dropped, and both Xiph files say 2002
// rather than the 2002-2020 range the doc claimed. A notice that is nearly the
// text is not the text.
//
// **Nothing here is wrapped by hand.** The bottom screen's font comes off
// whichever texture pack is loaded, so its character widths are not known until
// a pack is, and a line broken to fit the built-in font would break badly under
// a pack whose glyphs are wider. Every newline below is therefore structural --
// a paragraph, or a clause in a list -- and the wrapping is left to
// `texture::wrapText`, which measures the font that is actually on screen.

#include "core/util/about.hpp"

namespace mc::about {

namespace {

// Ordered as the Info screen reads them: the entries a distribution cannot drop
// first, then the ones named because it is right to and not because we are
// asked. `notices()` must stay in step with the licensed half of this.
constexpr Credit kCredits[] = {
    {"Jean-loup Gailly and Mark Adler",
     "zlib, the compression every chunk file and every Map Chunk packet is written with",
     "zlib licence"},
    {"Sun Microsystems",
     "fdlibm, the logarithm world generation draws its seeded numbers through",
     "fdlibm notice"},
#if MC_HAVE_VORBIS
    {"the Xiph.Org Foundation",
     "libvorbisidec and libogg, which decode the music and sounds you supply",
     "BSD 3-clause"},
#endif

    // **A courtesy credit and not a notice, deliberately.** libctru and citro3d
    // are linked into this binary and their licence very probably does ask for
    // one -- but no licence file ships in the devkitPro install
    // (`$DEVKITPRO/licenses` covers the tools, not the libraries), so there is
    // nothing here to transcribe. Inventing the text would be worse than owing
    // it: a notice that misquotes the licence it satisfies satisfies nothing.
    // Tracked as an open item in docs/licences.md, to be settled from upstream
    // before a release.
    {"devkitPro",
     "devkitARM, libctru and citro3d -- the toolchain and the libraries this runs on", nullptr},
    {"RSDuck",
     "craftus_reloaded, the 3DS Minecraft clone whose rendering and threading this one learned "
     "from",
     nullptr},
    {"RaphiMC and ViaVersion",
     "ViaLegacy, which documents the 2010 protocol. Read as documentation; none of its code is "
     "here",
     nullptr},
    {"OrnitheMC",
     "the mappings that make the original jar readable, so every number here could be checked "
     "against it",
     nullptr},
    {"Tommaso Checchi",
     "the visibility-graph algorithm that decides which chunks are worth drawing", nullptr},
};

// Reproduced in full rather than summarised, because each of these says so
// itself. The Xiph blocks keep the original's ``AS IS'' quoting: it is what the
// file says, and a notice is quoted, not tidied.
constexpr const char* kNotices[] = {
    "zlib\n"
    "Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler\n"
    "\n"
    "This software is provided 'as-is', without any express or implied warranty. In no event "
    "will the authors be held liable for any damages arising from the use of this software.\n"
    "\n"
    "Permission is granted to anyone to use this software for any purpose, including commercial "
    "applications, and to alter it and redistribute it freely, subject to the following "
    "restrictions:\n"
    "\n"
    "1. The origin of this software must not be misrepresented; you must not claim that you "
    "wrote the original software. If you use this software in a product, an acknowledgment in "
    "the product documentation would be appreciated but is not required.\n"
    "2. Altered source versions must be plainly marked as such, and must not be misrepresented "
    "as being the original software.\n"
    "3. This notice may not be removed or altered from any source distribution.",

    "fdlibm\n"
    "Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.\n"
    "\n"
    "Developed at SunSoft, a Sun Microsystems, Inc. business.\n"
    "\n"
    "Permission to use, copy, modify, and distribute this software is freely granted, provided "
    "that this notice is preserved.",

#if MC_HAVE_VORBIS
    "libvorbisidec (Tremor) and libogg\n"
    "Copyright (c) 2002, Xiph.org Foundation\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without modification, are "
    "permitted provided that the following conditions are met:\n"
    "\n"
    "- Redistributions of source code must retain the above copyright notice, this list of "
    "conditions and the following disclaimer.\n"
    "\n"
    "- Redistributions in binary form must reproduce the above copyright notice, this list of "
    "conditions and the following disclaimer in the documentation and/or other materials "
    "provided with the distribution.\n"
    "\n"
    "- Neither the name of the Xiph.org Foundation nor the names of its contributors may be used "
    "to endorse or promote products derived from this software without specific prior written "
    "permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND ANY "
    "EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF "
    "MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE "
    "FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, "
    "EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF "
    "SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) "
    "HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR "
    "TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS "
    "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.",
#endif
};

}  // namespace

Span<const Credit> credits()
{
    return Span<const Credit>(kCredits, sizeof(kCredits) / sizeof(kCredits[0]));
}

Span<const char* const> notices()
{
    return Span<const char* const>(kNotices, sizeof(kNotices) / sizeof(kNotices[0]));
}

}  // namespace mc::about
