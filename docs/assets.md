# Assets and texture packs

3DAlpha ships **no Mojang content**. It generates its own placeholder art so the game is playable
with no setup, and imports textures from a user-supplied `minecraft.jar` or texture-pack zip for
authenticity.

## What the player has to supply: nothing

This is a design rule, not just a current state. **Textures and sounds are the only things a player
can supply, and both are optional.** Install the game, launch it, play. Blocks, items, recipes,
physics, lighting and world generation are part of the program — the player is never asked to dump,
extract or convert game data, and no feature is gated behind having a jar.

| Asset | Bundled? | If the player supplies nothing |
|---|---|---|
| Block textures | **Generated** — "Dev Art", `core/texture/dev_art.cpp` | Everything renders, in the placeholder's style |
| Fire, water and lava tiles | **Always generated, even from a pack** — the six `TextureFX` the client registers against `terrain.png` overwrite what the pack holds there, which is what the original does (`core/texture/texture_fx.cpp`, `core/texture/fluid_fx.cpp`) | Same picture either way |
| Font | Not bundled. A pack's `default.png` is the menu's font when it has one | The menu draws with the 3DS system font |
| Menu backdrop | Not bundled. A pack's `dirt.png`, tiled and darkened as `GuiScreen` does it | The dirt tile of whatever `terrain.png` is live, Dev Art's included |
| GUI widgets | Not bundled and not read yet | Buttons are drawn rectangles |
| Particle sprites | Not bundled. A pack's root `particles.png`, scaled to 128×128 | **Generated** — white discs on the tiles a1.1.2 names, tinted by the quad as the real sheet is (`core/texture/particle_sheet.cpp`) |
| Sounds | No — a1.1.2 never shipped them | Game runs silently |
| DSP firmware (`dspfirm.cdc`) | Cannot be — Nintendo copyright | Audio disabled, one line in the options screen |
| Block/item/recipe data, worldgen | **Compiled into the binary** | Not applicable — always present |

The last row is the one that is easy to get wrong. Deriving a table from an original jar is
something a *maintainer* does once, and the result is checked in; it never becomes a step in the
player's way. See [Provenance of the generated tables](#provenance-of-the-generated-tables).

## What "an a1.1.2 texture pack" actually is

a1.1.2 predates the in-game texture-pack selector — that arrived in a1.2.2 (2010-11-10). Before it,
changing textures meant overwriting files inside `minecraft.jar`. So "a1.1.2 texture pack" in
practice means **the pre-1.5 jar layout**, which is what every alpha- and beta-era pack uses.

**This table was read out of a real client jar, not written from memory**, and the earlier version of
it here was wrong in three places. `core/texture/pack_list.cpp` holds the same 58 names as
`kA112Files[]`, which is what the pack screen's "n/58" counts against, and `tests/pack_test.cpp`
asserts the three corrections still hold so the table cannot drift back.

| Where | n | Files |
|---|---|---|
| root | 15 | `terrain.png` `char.png` `2char.png` `default.png` `clouds.png` `particles.png` `shadow.png` `rain.png` `snow.png` `fluff.png` `water.png` `waterterrain.png` `dirt.png` `grass.png` `rock.png` |
| `armor/` | 10 | `chain_1/2` `cloth_1/2` `diamond_1/2` `gold_1/2` `iron_1/2` |
| `art/` | 1 | `kz.png` — the painting sheet |
| `gui/` | 8 | `gui` `icons` `items` `logo` `container` `crafting` `furnace` `inventory` |
| `item/` | 5 | `arrows` `boat` `cart` `door` `sign` |
| `misc/` | 3 | `gear` `gearmiddle` `vignette` |
| `mob/` | 12 | `chicken` `cow` `creeper` `pig` `saddle` `sheep` `sheep_fur` `skeleton` `slime` `spider` `spider_eyes` `zombie` |
| `terrain/` | 2 | `sun` `moon` |
| `title/` | 2 | `black` `mojang` |

Three corrections, each of which had been asserted here without being checked:

* **`misc/` holds three files, not the loose-ends drawer this document described.** Water, shadow,
  particles, rain, snow, dirt, grass and rock are all at the **root**.
* **There is no `misc/grasscolor.png` or `misc/foliagecolor.png`.** That is a fourth independent
  confirmation of [the no-tinting finding](#tinting), arrived at from a different direction than the
  three in status.md.
* **`terrain.png` is 256×256, 8-bit RGBA, non-interlaced** — as is every other PNG in the jar. Four
  of the 58 are colour type 3 (palette); the other 54 are colour type 6. **None** is 16-bit and
  **none** is interlaced, which is what sets the scope of `core/texture/png.cpp`.

Packs are plain zips containing that tree at the root, and 3DAlpha also accepts the same tree lying
loose in a directory — a card is mounted on a PC as often as on a console, and a player who unzipped
a pack in place has not done anything wrong. HD packs use the same layout with larger power-of-two
images (32×, 64× per tile).

### What a jar's zip actually looks like

Both of these are load-bearing for the importer and neither is obvious:

* **The jar mixes compression methods** — 41 stored entries and 497 deflated ones. Both paths are
  exercised by the first real file the reader ever sees.
* **497 of the 538 entries set general-purpose flag bit 3**, which means their local headers carry
  **zeroed CRC and size fields** and the real values trail the compressed data in a descriptor. A
  reader that trusts local headers reads garbage lengths for 92 % of this jar. `ZipArchive` therefore
  treats the central directory as the only source of truth and opens a local header for exactly one
  purpose: to learn how many bytes of name and extra field to skip.

There is no Zip64 record, and `ZipArchive` refuses one rather than parsing it.

## Choosing and importing a pack, from the console

**Options → Texture Pack.** The list is Dev Art pinned first, then every `.zip` and every directory
under `sdmc:/3dalpha/packs` that holds a `terrain.png`, sorted by name. Each row shows how many of
the 58 names above the pack carries; a partial pack is fine, because only `terrain.png` is drawn.
The live pack is marked, and the choice is written to `3ds.ini` the moment it is made.

**The pack is loaded and decoded on the screen that chose it**, not in the renderer. A pack that will
not decode is refused in front of the player with a reason, and the pack they had stays live — the
alternative is an untextured world and no explanation.

## Choosing a player skin

**Options → Skin.** The only thing in this build that draws a player skin is the **arm of an empty
hand** — a1.1.2 renders no other biped — so that is what this screen changes. Three groups of row:

- **Default**, first and always present: the *active* texture pack's own `char.png`, or a **solid
  black silhouette** when it has none. Every other page's stand-in is a coloured grid, which suits a
  boat and does not suit an arm — a forearm in placeholder orange reads as a bug where a silhouette
  is honest about being a shape with no skin on it.
- **Every texture pack that carries a `char.png`**, whether or not it is the pack in use. `hasSkin`
  comes off the same central-directory read the file count does, so a pack without one costs
  nothing.
- **Every `.png` in `sdmc:/3dalpha/skins`**, which the screen creates the first time it is opened.

`char.png` sits at the **root** of the pack, not under `item/` — the one thing unusual about it.
Both **64 × 32** and **64 × 64** are read; a 64 × 64 keeps its top half, because that half *is* the
classic layout and the second layer below it is 1.8's, which a1.1.2's `ModelBiped` has nowhere to
draw. A skin drawn for the **slim** body is detected and the row says so, and it is still drawn on
the wide arm: the narrow body arrived with 1.8 and this is Alpha. `versions/<id>.json`'s
`hasSlimSkins` is the gate, and `core/render/held_item.cpp` fails the build if it is turned on
before that version's `ModelBiped` box has been derived.

The choice is written to `3ds.ini` as a **name**, not a path — `pack:<name>` or `file:<name.png>` —
so applying it at boot costs the one file it names rather than a walk of the packs folder, and
moving the card's `3dalpha` folder does not orphan it. A skin that is no longer on the card falls
back to Default rather than leaving the screen pointing at nothing.

**Options → Texture Pack → Extract from a jar** lists every `*.jar` in `sdmc:/3dalpha/packs` and in
`sdmc:/3dalpha` above it, with sizes, and turns the one the player picks into
`packs/<jar name>.zip`. Entries are copied **verbatim** — compressed bytes, method, CRC and sizes
straight out of the source's central directory — so a 900 KB jar becomes a pack without one inflate
or deflate call. Local headers are rewritten with flag bit 3 cleared and the real sizes filled in,
since the directory already gave us them; copying the flag without also copying the trailing
descriptor would produce an archive that lenient readers accept and strict ones reject.

The filter is **every `.png` that is not under `META-INF/`**, rather than an allow-list. It is
simpler, it is exactly the 58 files above, and it keeps working on a jar from a version whose layout
nobody has measured.

**Then it verifies what it wrote**, by re-opening the pack off the card, decoding its `terrain.png`
and building the atlas from it. Only a pack that survives that round trip is reported as a success —
and that is what makes the next screen, which offers to **delete the source jar**, a safe thing to
show. The jar is the only file this project deletes that the player did not create in it, so the
offer appears on no other basis, and `Keep` is the button the screen leads with.

**Nothing is redistributed and nothing enters the repository or the build.** The player's own file is
filtered into another file on the player's own card.

## What a pack actually changes today

**`terrain.png`, `default.png` and `dirt.png`** were the whole of it once: the block atlas, the font
the menu draws every label with, and the backdrop behind them. They are no longer alone. As each
thing that draws from a file landed, that file started being read — `gui/items.png` for icons and
dropped stacks, `particles.png`, `art/kz.png`, `char.png` and the arm, the five `item/` sheets, the
twelve `mob/` ones, and now **`terrain/sun.png` and `terrain/moon.png`**, which are two more pages
of the entity sheet (`core/texture/entity_skins.hpp`). What is still copied, counted and left alone
is the rest of `gui/`, `armor/`, `misc/`, `title/` and the root's water and weather sheets: there is
no consumer for them yet.

**None of the three is required and none of them fails loudly.** A pack with no `default.png` leaves
the menu on the 3DS system font, which is what it drew with before any of this existed. A pack with
no `dirt.png` gets a backdrop cut out of its own `terrain.png` — the dirt block's tile, looked up in
the generated block table rather than written down — which is also what serves Dev Art, since
generated art has no `dirt.png` and never will. The texture-pack screen still counts all 58 files
rather than implying more of them are read than are.

The atlas is **always 256×256**, whatever the pack's tile size, and that is a decision with a
measurement behind it. VRAM is 6 MB, render targets already take 0.8–1.5 MB, and a 64× pack's
1024×1024 atlas is 4 MB at RGBA8. Rather than make the atlas edge a runtime value that every VRAM
number in [3ds-performance.md](3ds-performance.md) would have to be re-measured against, a larger
`terrain.png` is box-filtered down on load. The mesher's UVs are in 1/16384 of the whole atlas
(`core/mesh/vertex.hpp`), so nothing downstream can tell.

Two details of that scaling are not free choices:

* **The box filter averages premultiplied by alpha.** A cutout edge — a torch, a sapling, the rim of
  a leaf — sits beside texels whose alpha is 0 and whose RGB is usually black, and a plain mean drags
  the visible half toward black. Every HD pack would grow dark halos.
* **A pack smaller than 256 is replicated, not interpolated.** The whole look depends on nearest
  filtering.

A `terrain.png` that is not square, or whose edge is not a multiple of 16, is refused: it is not a
tile grid, and scaling it would shift every tile boundary by a fraction of a texel.

### Dev Art

The generated placeholder is now **one selectable pack among the others** rather than the only thing
there is, and it lives in `core/texture/dev_art.cpp` rather than in the platform layer. It needs
nothing off the card, which is what makes the screen a selector rather than something that can leave
the game untextured.

Moving it into core put two derivations under the host suite for the first time — the fluid tile
groups and the torch carve, both read out of the block table and the mesher's own geometry rather
than written down — and immediately found a defect that had been there all along: the per-texel
jitter multiplied three `int`s that overflow for almost every input, which is undefined behaviour.
Nothing in `platform/ctr/` is sanitised, so it had never been reported. The multiply is unsigned now,
which wraps by definition and produces the same 32 bits, so the generated art is byte-identical to
what the console has been drawing.

## Where the code is

| Piece | File |
|---|---|
| PNG decoder | `core/texture/png.cpp` — 8-bit, non-interlaced, colour types 0/2/3/4/6 |
| Zip reader | `core/texture/zip_archive.cpp` — central directory only |
| Zip writer | `core/texture/zip_builder.cpp` — verbatim entries, no deflate |
| Pack listing | `core/texture/pack_list.cpp` — and `kA112Files[]` |
| Atlas assembly and scaling | `core/texture/atlas_image.cpp` |
| Dev Art | `core/texture/dev_art.cpp` |
| Jar import | `core/texture/jar_import.cpp` |
| `3ds.ini` | `core/settings/settings_file.cpp` |
| Screens | `platform/ctr/menu.cpp` |
| Upload | `platform/ctr/textures.cpp` |

There is no new third-party dependency. A PNG's `IDAT` stream is zlib-wrapped and a zip's entries are
raw deflate, and `core/util/compress.hpp` already offered both framings for chunk files and Map Chunk
payloads — so miniz and lodepng, which this document used to name, are not needed.

Two host harnesses run the whole thing away from a console, under ASan/UBSan:

```sh
./build-host/3dalpha --extract-jar <jar> <packs-dir>   # the importer, over a real jar
./build-host/3dalpha --pack <zip|dir|devart>           # assemble an atlas, write atlas.pam
```

## Sounds

Sounds are **not** in the alpha jar. a1.1.2 downloaded them at runtime into `.minecraft/resources/`
from a Mojang server that no longer exists. The game therefore reads a user-supplied `resources/`
folder in the original layout, unchanged, so a folder copied from any alpha- or beta-era install
works as it is:

```
sdmc:/3dalpha/resources/sound/<category>/<name>.ogg
sdmc:/3dalpha/resources/newsound/...
sdmc:/3dalpha/resources/streaming/...
sdmc:/3dalpha/resources/music/...
sdmc:/3dalpha/resources/newmusic/...
```

Those five names are the whole of it — `Minecraft.installResource` splits the key at the first `/`
and recognises exactly those, so a folder that also carries `pack.mcmeta`, `icons/`, `pe/` or
`sound3/` is fine and the rest is ignored. `music/` and `newmusic/` feed the same pool, and that
pool is what the background-music timer draws from. The walk is directory listings only — nothing is
read or decoded until it is needed — and it is capped at six levels and 4,096 entries, because a
card can have anything on it.

If the folder is absent the game runs silently; audio is not a hard dependency. (Audio also needs a
dumped DSP firmware — see below.)

**Music, plus one sound effect.** `random/click.ogg` — under `sound/` or `newsound/`, whichever
the folder has — is decoded at boot and is what the
menus click with; everything else in the sound pool is indexed and counted but not played, because
nothing in the port can emit it yet — there is no block placement, no player body and no entities.
`streaming/*.mus` is Mojang's own container — an Ogg Vorbis file behind a one-byte cipher keyed
on its own file name — and a jukebox plays one straight out of the folder; see
[audio-a1.1.2.md](audio-a1.1.2.md). A card with no `random/click.ogg` in either simply has silent
menus. See
[audio-a1.1.2.md](audio-a1.1.2.md).

## SD card layout

```
sdmc:/3dalpha/
  3ds.ini                  3DS-specific options -- built, and the only file here the game writes
  packs/<name>.zip         texture packs, pre-1.5 jar layout
  packs/<name>/            the same tree lying loose, also accepted
  packs/*.jar              optional: import sources for Extract from a jar
  saves/<world>/           worlds, in the real Alpha level format

  options.txt              not built -- a1.1.2's own options file, for when there
                           are gameplay options worth writing to it
  resources/               user-supplied sounds, in a1.1.2's own layout -- read, never written
  cache/music/             not built, and not currently planned -- see audio-a1.1.2.md
  cache/<pack>.3dtex       not built -- see below
  cache/<world>.idx        not built, and not planned -- the chunk index is built lazily in
                           RAM instead, one leaf directory at a time, with no file to
                           invalidate. See core/world/chunk_cache.hpp
```

`3ds.ini` holds `render_distance`, `texture_pack`, `autosave_seconds`, `chunk_cache_mb`, `audio`,
`music_volume` and `sound_volume`, is
`key=value` with `#` comments, and is
**rewritten from the keys the running build knows** — so a key a later version adds is dropped by an
older one. An absent file is the ordinary first-boot state, and it is written through
`writeFileAtomic`, so a console switched off mid-save keeps the settings it had.

It is deliberately separate from `options.txt`. That file is a1.1.2's own, with its own format, and
keeping them apart means a 3DS setting never ends up somewhere a PC copy of the game would read it.

## What the import pipeline turned out to be

**This section used to describe a design; it now describes what was built, and three of the six
steps went away.**

1. ~~Open the zip with **miniz**; decode PNGs with **lodepng**.~~ Neither is needed. A PNG's `IDAT`
   stream is zlib-wrapped and a zip's entries are raw deflate, and `core/util/compress.hpp` had
   already offered both framings since M1 for chunk files and Map Chunk payloads. `zip_archive.cpp`
   and `png.cpp` are built on it, so the whole feature added no third-party dependency.
2. **Assemble the block atlas** — `atlas_image.cpp`. Missing tiles do not fall back per-tile: a pack
   supplies a whole `terrain.png` or it is not a pack, and the fallback is per-*pack* to Dev Art.
   Partial packs work: a pack that carries only `terrain.png` still plays, and the two files the
   menu reads have fallbacks of their own — see [What a pack actually changes
   today](#what-a-pack-actually-changes-today).
3. ~~Choose a GPU format per image; ETC1A4 by default.~~ Not done. The atlas is RGBA8 at 256 KB,
   which is what it has always been, and the VRAM pressure ETC1 was for does not exist while the
   atlas is capped at 256×256. Worth revisiting if the cap is ever lifted.
4. **Morton-swizzle** — ~~via `C3D_SyncDisplayTransfer`~~, **on the CPU**. The transfer engine was
   doing the tiling, the vertical flip and a format conversion in one step, into VRAM, and it lost
   the destination's last row: a gray line across the top of every tile in atlas row 0, and nothing
   else in the world touched. It is `core/texture/tiled.hpp` now, folded into the byte-order pass
   `Atlas::init` already made over every texel, and the upload is reduced to moving bytes that are
   already final. See docs/status.md §1, ninth launch.
5. ~~Write `cache/<pack>.3dtex`.~~ Not built. It was sized against a pipeline with per-image format
   selection in it; what is left is one PNG decode and one pass over 65,536 texels, which happens
   when the player picks a pack rather than at boot. **Worth building when there is a measurement
   from hardware saying that pass is slow enough to notice** — not before, and note the swizzle is
   now on the CPU, so the measurement is more interesting than it was.
6. ~~Invalidate the cache on mtime change.~~ Follows 5.

## HD packs and the VRAM budget

A 64× pack means a 1024×1024 source: 4 MB at RGBA8, against 6 MB of VRAM that render targets already
take 0.8–1.5 MB of.

**The atlas is capped at 256×256 rather than clamped by a `texture_quality` option**, and anything
larger is box-filtered down at load. That is the same downscale this section always described, with
the option removed: making the atlas edge a runtime value would put every VRAM number in
[3ds-performance.md](3ds-performance.md) back in question, and the option would have had exactly one
useful setting on each model. A pack that cannot be made to fit is refused with a message that names
the limit — the rule this section set, and it is kept.

Downscaling also *helps* frame rate: a smaller atlas has a much better texture-cache hit rate on a
fill-bound GPU.

## Tinting

**a1.1.2 does not tint anything.** This was an open question and the jar answered it: `Block`'s
colour multiplier returns 0xFFFFFF, no subclass overrides it, the jar contains no
`misc/grasscolor.png` or `misc/foliagecolor.png`, and `terrain.png`'s grass and leaf tiles are
already green rather than greyscale masks. The derivation is in
[status.md](status.md#answered).

So for this version there is no tint byte, no colour map in the asset list, and no TEV stage to
spend on it. A pack's grass tiles are whatever colour the pack made them.

This changes in later versions, which is why the vertex keeps three colour bytes rather than folding
face shade into one: Beta-era biome colouring turns the same `colorMultiplier` path on, and porting
then means filling a field that already exists rather than changing the vertex format. See
[porting-to-other-versions.md](porting-to-other-versions.md).

## Fonts

**The menu draws with the pack's `default.png` when the pack carries one**, and falls back to the
3DS system font when it does not. `C2D_FontLoad` was never the path: it takes a converted BCFNT, so
it would only ever serve a bundled font, and there is no bundled font. What the menu uses instead is
`core/texture/font.hpp` for the rules and `platform/ctr/gui_art.hpp` for the drawing — one quad per
glyph, the pen advancing by the glyph's own width.

Every rule in it was read out of `kd.class`, the original's font renderer, rather than remembered:

* The sheet is a 16×16 grid of 8-pixel cells, 128×128 in the original.
* A glyph's advance is the last column of its cell with anything in it, plus two. The space is
  forced to 4 rather than measured.
* **"Anything in it" is the blue channel, not alpha.** The original reads `getRGB` and tests
  `pixel & 255`, which on an ARGB int is blue. A pack whose glyphs are pure red measures as
  entirely empty and draws one pixel wide per character. That is reproduced, not corrected.
* Text is not indexed by character code: each character is looked up in a fixed string running from
  the space to `»`, and the position plus 32 is the glyph. A character outside it draws nothing and
  advances nothing — including a backtick, because the original's string repeats an apostrophe where
  one would go.
* `§` and a hex digit switch colour, both consumed by drawing and by measuring alike; an
  unrecognised digit means white. The sixteen colours are built the way the original's constructor
  builds them, gold's extra 85 of red included.

The one deviation is HD packs. The original hardcodes 8 and 128.0f and would read a 256×256
`default.png` as a garbled grid of its own top-left quarter; here the sheet is scaled to 128×128
first, exactly as `terrain.png` is scaled to the atlas, and every rule above applies to the scaled
copy. Widths are derived at load — a 128×128 scan is 16k texels once per pack change, which is not
worth a cache format.

## Audio and DSP firmware

3DS homebrew audio through `ndsp` requires a dumped DSP firmware at `sdmc:/3ds/dspfirm.cdc`.
Nintendo's copyright prevents shipping it. Users dump it from their own console — Luma3DS Rosalina
menu → "Miscellaneous options" → "Dump DSP firmware".

The game **detects its absence and continues silently**, with a one-line explanation in Options →
Sound. Audio is never a startup dependency.

The explanation distinguishes *why*, and that is the point of having one: a player who has already
dumped their firmware and is told to dump it again will conclude the game is broken. `ndspInit`
failing with the file present means something else on the console is holding the DSP, and the screen
says so instead.

## Licensing rules

| Content | Rule |
|---|---|
| Mojang textures, sounds, fonts | **Never bundled.** User-supplied only, via jar/pack import. |
| Bundled fallback pack | There is none. The built-in pack is **generated** by our own code from our own colours, so there is nothing to license or attribute. A CC-licensed art pack remains an option and would need an entry in `src/core/util/about.cpp` — see [licences.md](licences.md). |
| A pack imported from a jar | Written to the player's own card from the player's own file, never redistributed and never checked in. See [Choosing and importing a pack](#choosing-and-importing-a-pack-from-the-console). |
| craftus_reloaded code | MIT — reusable with attribution. Credited on the Options → Info row; see [licences.md](licences.md). |
| ViaLegacy | GPLv3 — **documentation only**. Protocol IDs and wire sizes are facts; its code is not copied. |
| Data recovered from a client jar | Facts only — ids, hardness, light levels. Recovered by a maintainer, checked in, shipped compiled. Never code, never assets, never redistributed. See below. |
| Third-party libs | zlib, and — optionally, for audio — Xiph's Tremor (`libvorbisidec`) with libogg on the console and `libvorbisfile` on the host. A build without a decoder produces a silent binary, not no binary. **miniz and lodepng were not needed** — see [What the import pipeline turned out to be](#what-the-import-pipeline-turned-out-to-be). Notices are in [licences.md](licences.md) and, since 2026-09-18, in the binary itself — `src/core/util/about.cpp`, read on the Options → Info row. The one entry still open there is libctru and citro3d, whose licence text devkitPro does not install. |

There is no `romfs/licenses.txt`; nothing ships inside the title. The notices are a `const char[]`
in `.rodata`, paged onto the bottom screen by the **Options → Info** row, which also gives the
client version, the Minecraft version it implements, and the statement that this is not Mojang's.
See [licences.md](licences.md#where-the-notice-lives-in-a-shipped-binary--decided).

### Provenance of the generated tables

**This section is about how the repository was built, not about anything a player or the build
does.** `data/a1.1.2/blocks.json` is checked in and `tools/configure.py` compiles it into the
binary; neither `make` nor the game ever looks for a jar.

That JSON was produced once by a maintainer. `tools/extract_blocks.py` disassembles a client jar
the maintainer already owns, with `javap`, and recovers the block table: ids, texture indices,
hardness, blast resistance, emitted light, light opacity, render type, material grouping and
material solidity. `--verify` re-checks the
checked-in file against a jar at any time, which is how the table stays trustworthy after somebody
edits it by hand.

Why derive it rather than write it: a hand-written table looks right and is wrong in small places,
and each of those becomes a rendering or physics bug that is tedious to trace back.

The mesher's **directional face shading** came from the same place and by hand rather than through
the extractor: `RenderBlocks` loads 0.5, 1.0, 0.8 and 0.6 into four locals and multiplies each
face's colour by one of them — bottom, top, the two Z faces, the two X faces. The values live in
`src/core/mesh/vertex.hpp` with that derivation recorded next to them.

**Per-face textures are interpreted, not read.** Every texture in the table now comes out of
`tools/javap.py`, a small JVM interpreter that *runs* the block class instead of pattern matching
it. Pattern matching could not answer either question that matters here: a block that sets its own
texture in its own constructor (grass, logs, chests, doors) puts nothing in the static initialiser
to match, and the dozen classes that give each side a different tile do it with a branch on the face
index. It also got some of them actively wrong — where a constructor is `(int id, boolean flag)`,
"the second int argument" is the flag, which is how the furnace came to be recorded with texture 0.

Running `Block.<clinit>` builds all seventy blocks in order, so a block that asks another block for
its texture — the stairs take their model's, the crafting table asks the planks — sees the real
value. Then each block's three `getBlockTexture` overloads are evaluated per face, most specific
first:

| Overload | Answers for |
|---|---|
| `getBlockTexture(world, x, y, z, face)` | grass under snow, which way a chest or furnace faces |
| `getBlockTexture(face, metadata)` | wheat's growth stage, wet farmland, doors, rails |
| `getBlockTexture(face)` | logs, slabs, the crafting table, ores, cactus, the jukebox |

The world is supplied as a *probe* that raises when touched, so a block that answers without
consulting it gives an answer good everywhere, and one that consults it is **reported** as
world-dependent rather than answered with a plausible guess. Metadata is tried across all sixteen
values for the same reason. `blocks.json` records the no-metadata, no-neighbours answer, and the
extractor names the blocks whose faces depend on either — currently grass, chests, furnaces and
stairs for the world, and wheat, farmland, doors, rails and redstone for metadata. Nothing reads
metadata yet, so those are the known gap.

Two things fell out of this that are worth writing down:

* Alpha's `BlockFurnace` answers its top and bottom faces with `Block.stone.blockID`, not
  `blockIndexInTexture`. It draws correctly only because stone's id and its texture index are both
  1. The table records 1, which is what the game draws.
* `BlockDoor` returns a **negative** tile index to mean "mirror this face horizontally".
  `blocks.json` keeps the sign, because it is what the jar says; `configure.py` stores the magnitude
  and stops the build if a block that renders as a cube ever wants one, since the 12-byte vertex has
  no flip bit.

The line this stays on: what comes out is **factual data of the same kind any wiki table carries** —
that stone is id 1 with hardness 1.5 and that a torch emits light 14. No Mojang code is copied into
this repository, no asset is extracted, and no jar is redistributed. This is the same position the
plan takes on decompiling with OrnitheMC to verify the protocol.

What the jar cannot supply is **names**. `setBlockName` arrived after the alpha era, and a1.1.2's
`Block` class holds no strings at all. Every name in `blocks.json` is ours, and it is the one column
`--verify` cannot check.
