# Third-party licences

The notices that have to travel with a binary built from this tree.

**They travel inside it.** Every notice below is also compiled into the binary, in
[`src/core/util/about.cpp`](../src/core/util/about.cpp), and a player reads it on the
**Options → Info** row. This file is the same list in prose, for whoever has the repository;
it is no longer the only copy. See [Where the notice lives in a shipped binary](#where-the-notice-lives-in-a-shipped-binary--decided).

Most entries are libraries the build *links*, so their source is not in this repository but
their code ends up inside a distributed `.3dsx` or `.cia`. One — fdlibm — is transcribed
into the tree, and is marked as such.

See [CONTRIBUTING.md](../CONTRIBUTING.md#licensing-discipline) for the rules this file
exists to satisfy, and [assets.md](assets.md#licensing-rules) for the separate question of
game content, which is never bundled at all.

**Transcribed from the installed package's own licence file, not from upstream's website,
and checked on 2026-09-18.** That check is not a formality: it corrected two things this
file previously had wrong, both marked below.

## zlib

Used for the gzip and zlib streams in Alpha chunk files and Map Chunk packets. Source:
`$DEVKITPRO/portlibs/3ds/licenses/3ds-zlib/LICENSE`, with the copyright line from
`zlib.h` (1.3.1). The host build links the system zlib — zlib-ng on this machine — under
the same terms.

**Corrected 2026-09-18:** the first restriction carries a sentence about acknowledgment
that this file had dropped.

> Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
>
> This software is provided 'as-is', without any express or implied warranty. In no event
> will the authors be held liable for any damages arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose, including
> commercial applications, and to alter it and redistribute it freely, subject to the
> following restrictions: 1. The origin of this software must not be misrepresented; you
> must not claim that you wrote the original software. If you use this software in a
> product, an acknowledgment in the product documentation would be appreciated but is not
> required. 2. Altered source versions must be plainly marked as such, and must not be
> misrepresented as being the original software. 3. This notice may not be removed or
> altered from any source distribution.

## fdlibm

**The one third-party work whose code is in this tree.**
[`src/core/util/strict_math.hpp`](../src/core/util/strict_math.hpp) transcribes fdlibm 5.3's
`__ieee754_log` whole, because `java.lang.StrictMath` is specified against exactly those
algorithms and world generation's seeded random stream has to agree with a 2010 JVM bit for
bit. glibc's `log` is correctly rounded; fdlibm's is not; Java specifies the one that is
not. The file's own header explains the rest.

Sun's terms are permissive but conditional on the notice surviving, which makes this the
one entry here that a source release owes as much as a binary release does.

> Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
>
> Developed at SunSoft, a Sun Microsystems, Inc. business.
>
> Permission to use, copy, modify, and distribute this software is freely granted, provided
> that this notice is preserved.

## libvorbisidec (Tremor) and libogg

Xiph.Org's fixed-point Ogg Vorbis decoder and its container library, linked on the 3DS
target to decode the `.ogg` files a player supplies in `sdmc:/3dalpha/resources/`. The host
build links the reference `libvorbisfile` instead, under the same licence. Source:
`$DEVKITPRO/portlibs/3ds/licenses/3ds-libvorbisidec/COPYING` and `.../3ds-libogg/COPYING`,
which are byte-identical to each other.

**Optional.** A build without them produces a binary with no audio rather than no binary,
so a distribution that omits them does not carry this notice — and `MC_HAVE_VORBIS` drops
it from the Info screen too, since a notice for code that is not in the binary is a claim
about that binary that is not true.

**Corrected 2026-09-18:** the installed COPYING says `2002`, not the `2002-2020` range this
file previously claimed.

**The check this file used to defer is done.** Both installed `COPYING` files are the
BSD-3 text below; neither is one of Tremor's differently-licensed branches.

> Copyright (c) 2002, Xiph.org Foundation
>
> Redistribution and use in source and binary forms, with or without modification, are
> permitted provided that the following conditions are met:
>
> - Redistributions of source code must retain the above copyright notice, this list of
>   conditions and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright notice, this list of
>   conditions and the following disclaimer in the documentation and/or other materials
>   provided with the distribution.
> - Neither the name of the Xiph.org Foundation nor the names of its contributors may be
>   used to endorse or promote products derived from this software without specific prior
>   written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND ANY
> EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
> THE FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
> EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
> HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
> TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
> SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## libctru and citro3d — open

**Not settled, and the Info screen says so by omission.** Both are statically linked into
every 3DS build, so a notice is very probably owed. There is nothing here to reproduce:
the devkitPro install ships no licence file for them — `$DEVKITPRO/licenses` covers the
tools (picasso, tex3ds, 3dstools, devkitARM-gdb) and not the libraries, and
`$DEVKITPRO/libctru` and `citro3d` contain only headers and `.a` files.

So devkitPro is named on the Info screen as a **courtesy credit with no licence attached**,
which is honest about what is known. Inventing plausible notice text would be worse than
owing it: a notice that misquotes the licence it claims to satisfy satisfies nothing.

**To settle it**, take the text from upstream's own repository (`devkitPro/libctru` and
`devkitPro/citro3d`), confirm it applies to the version installed, and move the entry up
with the others. **This blocks the first release handed to someone outside the repo**, in
the same way the Tremor check used to.

## Where the notice lives in a shipped binary — decided

**In `.rodata`, behind the Options → Info row.** `src/core/util/about.cpp` holds every
notice above as a `const char[]`; `src/platform/ctr/menu.cpp` pages it onto the bottom
screen. Nothing can separate it from the code it covers, it is identical for 3DSX and CIA,
and no packaging changed to get it there — `packaging/3dalpha.rsf.in:9-13` keeps its
"nothing ships inside the title" property and `ctr_create_3dsx` keeps its arguments.

**This replaces the earlier decision**, which was that this file *was* the notice and
nothing went in the binary. That was sound only for as long as everyone who had the game
also had the repository: BSD-3's second clause and zlib's third both ask that the notice
reach whoever was handed the *program*, and a bare `.3dsx` carries no documentation. The
earlier entry named the trigger for revisiting — "a release, a link to a `.3dsx`, a `.cia`
posted anywhere" — and named this exact route as the cheapest one. It was taken before the
trigger rather than after, which is the cheaper order.

**What still has to hold.** `tests/about_test.cpp` pins the parts that are invisible on a
240-line screen: every credit that names a licence has that licence's text in the binary,
no notice has been truncated, and the Xiph entry appears if and only if `MC_HAVE_VORBIS`.
A notice added here without being added to `about.cpp` will not fail a test — the two lists
are kept in step by hand, and this paragraph is the reminder.
