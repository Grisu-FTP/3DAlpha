# Third-party licences

The notices that have to travel with a binary built from this tree. Nothing here is
vendored — every entry is a library the build *links*, so its source is not in this
repository, but its code does end up inside a distributed `.3dsx` or `.cia` and its licence
travels with it.

See [CONTRIBUTING.md](../CONTRIBUTING.md#licensing-discipline) for the rules this file
exists to satisfy, and [assets.md](assets.md#licensing-rules) for the separate question of
game content, which is never bundled at all.

## zlib

Used for the gzip and zlib streams in Alpha chunk files and Map Chunk packets.

> (C) 1995-2024 Jean-loup Gailly and Mark Adler
>
> This software is provided 'as-is', without any express or implied warranty. In no event
> will the authors be held liable for any damages arising from the use of this software.
> Permission is granted to anyone to use this software for any purpose, including
> commercial applications, and to alter it and redistribute it freely, subject to the
> following restrictions: 1. The origin of this software must not be misrepresented; you
> must not claim that you wrote the original software. 2. Altered source versions must be
> plainly marked as such, and must not be misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## libvorbisidec (Tremor) and libogg

Xiph.Org's fixed-point Ogg Vorbis decoder and its container library, linked on the 3DS
target to decode the `.ogg` files a player supplies in `sdmc:/3dalpha/resources/`. The host
build links the reference `libvorbisfile` instead, under the same licence.

**Optional.** A build without them produces a binary with no audio rather than no binary,
so a distribution that omits them does not carry this notice.

> Copyright (c) 2002-2020 Xiph.org Foundation
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

**Before the first release that links it, check the installed package's own `COPYING`**
(`pacman -Ql 3ds-libvorbisidec`). Tremor has had differently-licensed branches, and this is
a one-minute check rather than an assumption.

## Where the notice lives in a shipped binary — decided

**This file is the notice.** No RomFS, and nothing compiled into `.rodata`.

`packaging/3dalpha.rsf.in:9-13` keeps its "nothing ships inside the title" property, and
`ctr_create_3dsx` keeps its current arguments. Nothing about packaging changes for the sake
of a text file.

**What that means, so it is a decision and not an omission:** BSD-3's second clause asks for
the copyright notice to accompany a *binary* redistribution, and a bare `.3dsx` carries no
documentation. Anyone with this repository has the notice; anyone handed only a binary does
not.

**Revisit it when a build is given to someone who does not have the repo** — a release, a
link to a `.3dsx`, a `.cia` posted anywhere. CI publishing artifacts on every push is the
edge of this, and if those ever become the way people get the game, the notice has to travel
with them. The cheapest route then is a `const char[]` in `.rodata` behind an About screen:
no packaging change, identical for 3DSX and CIA, and impossible to separate from the binary.

Note that **`CONTRIBUTING.md:77,79` still says `romfs/licenses.txt`**, which does not exist and
is not what this project does. That wording is stale rather than wrong-in-spirit — the intent
was always "the notice ships with the thing" — and changing a review-blocking rule was left
for a change that is about licensing rather than about audio.
