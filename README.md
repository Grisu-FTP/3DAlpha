# 3DAlpha

A "Faithful" remake of Minecraft Java for the **Nintendo 3DS**.

Not a port of the Java version — a version specific remake with full Compatibility with the
original generation, save format, servers and more.

Also adds a few optional settings and fixes incase you want to customize your gameplay a bit (for example "Fix Ore Generation" or "Improve Fence Placement"), these usually will be something like optional Bug-Fixes, QoL from later versions, Features from the Bedrock or Legacy edition.


**This Project heavily uses AI!!!**

Despite this, i try to keep the polish high and amount of bugs low. I also tell it exactly how to work and thus it works pretty exact (almost giving pseudocode to translate) and since i have a pretty exact idea how stuff should end up looking this worked out so far. There are also alot of regression tests 


## Features

- **Server Compatible** with real servers (protocol 2, offline/unauthenticated login).
- **Save Compatible** with real worlds (Alpha level format) — read *and* write.
- **Compatible with texture packs** (pre-1.5 `terrain.png` layout).
- **Fast, efficient, configurable** — every option removes real work when turned off. 2 Save formats, one for compatibility and one for speed and size.
- **3D**
- **One binary per game version** Each supported Minecraft version compiles separately, with its
  own 3DS title, icon and title ID, so several install side by side and no build carries another
  version's code. `make VERSION=a1.1.2`. See [docs/build-versions.md](docs/build-versions.md).
- **New 3DS Improved**
- **Bottom Screen support**


## Assets

3DAlpha ships **no Mojang content**. It has its own placeholder art so it is playable out of
the box with nothing to dump, copy or configure.

**Textures and sounds are the only things you may supply, both are optional.** Pu a
`minecraft.jar` or a texture pack zip in `sd:/alpha/packs` for authentic visuals and pick it
from Options → Texture Pack. Put an original `resources/` folder in `sd:/alpha/` for sound,
which a1.1.2 downloaded at runtime and never shipped. Without either, the game is complete and
playable — placeholder textures, no audio.

Music also needs a DSP firmware, dump your own console's with Luma3DS's Rosalina menu → Miscellaneous options → Dump DSP firmware. Without it the game runs silently and Options → Sound says why. With both, background music starts on a1.1.2's
own timer — once in the first ten minutes, then after every 20–40 minutes of quiet. See
[docs/audio-a1.1.2.md](docs/audio-a1.1.2.md).

The jar importer moves files you already own from one file on your own card to another. Nothing is
downloaded, nothing is sent anywhere, and no extracted asset enters this repository or its build.

Everything else — blocks, items, recipes, physics, world generation — is part of the program and
is simply there. See [docs/assets.md](docs/assets.md).

## Acknowledgements

- **RSDuck** — [craftus_reloaded](https://github.com/RSDuck/craftus_reloaded) (MIT), the reference
  3DS Minecraft clone this project learns its rendering and threading patterns from.
- **Tommaso Checchi** — the [chunk visibility-graph culling algorithm](https://tomcc.github.io/2014/08/31/visibility-1.html).
- **RaphiMC / ViaVersion** — [ViaLegacy](https://github.com/ViaVersion/ViaLegacy) documents the
  alpha-era protocol. Used here as *documentation only*; it is GPLv3 and none of its code is copied.
- **OrnitheMC** — mappings that make the original a1.1.2 jar readable for verification.
- **devkitPro** — devkitARM, libctru, citro3d, picasso.
- **Jean-loup Gailly and Mark Adler** — zlib, which every chunk file and every Map Chunk packet
  is written with.
- **Sun Microsystems** — fdlibm, whose `__ieee754_log` is transcribed in
  [`src/core/util/strict_math.hpp`](src/core/util/strict_math.hpp) because `StrictMath` is
  specified against it and seeded world generation has to agree with a 2010 JVM bit for bit.
- **The Xiph.Org Foundation** — libvorbisidec (Tremor) and libogg, which decode the audio a
  player supplies.

The same list, with the notices in full, is on the **Options → Info** row in game and in
[docs/licences.md](docs/licences.md).

## Licence

3DAlpha is free software under the **GNU General Public License, version 3 or later** — see
[LICENSE](LICENSE). A binary you are given comes with the right to the source that built it.

The acknowledgements above are compatible with that: zlib, fdlibm and the Xiph libraries are
permissive, and craftus_reloaded is MIT, which GPLv3 absorbs as long as the attribution stays.
ViaLegacy is GPLv3 itself and is used as documentation only — no code from it is here, and the
licence does not change that.

The GPL covers **this program**. It does not cover, and cannot cover, Mojang's game: no Mojang
code, texture, sound or asset is in this repository or in a binary built from it, and the tables
in `data/` are facts about a 2010 game rather than anything copied out of one. See
[CONTRIBUTING.md](CONTRIBUTING.md#licensing-discipline).
