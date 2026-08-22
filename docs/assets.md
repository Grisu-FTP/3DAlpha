# Assets and texture packs

3DAlpha ships **no Mojang content**. It bundles a free CC BY-SA fallback pack so the game is
playable with no setup, and imports textures from a user-supplied `minecraft.jar` or texture-pack
zip for authenticity.

## What the player has to supply: nothing

This is a design rule, not just a current state. **Textures and sounds are the only things a player
can supply, and both are optional.** Install the game, launch it, play. Blocks, items, recipes,
physics, lighting and world generation are part of the program — the player is never asked to dump,
extract or convert game data, and no feature is gated behind having a jar.

| Asset | Bundled? | If the player supplies nothing |
|---|---|---|
| Block textures, GUI, font | CC BY-SA fallback pack in RomFS | Everything renders, in the fallback pack's style |
| Sounds | No — a1.1.2 never shipped them | Game runs silently |
| DSP firmware (`dspfirm.cdc`) | Cannot be — Nintendo copyright | Audio disabled, one line in the options screen |
| Block/item/recipe data, worldgen | **Compiled into the binary** | Not applicable — always present |

The last row is the one that is easy to get wrong. Deriving a table from an original jar is
something a *maintainer* does once, and the result is checked in; it never becomes a step in the
player's way. See [Provenance of the generated tables](#provenance-of-the-generated-tables).

## What "an a1.1.2 texture pack" actually is

a1.1.2 predates the in-game texture-pack selector — that arrived in a1.2.2 (2010-11-10). Before it,
changing textures meant overwriting files inside `minecraft.jar`. So "a1.1.2 texture pack" in
practice means **the pre-1.5 jar layout**, which is what every alpha- and beta-era pack uses:

```
terrain.png            256x256 : 16x16 grid of 16px block tiles (also breaking animation)
gui/items.png          256x256 : item icons
gui/gui.png                     : hotbar, inventory, widgets
gui/icons.png                   : crosshair, hearts, bubbles
char.png                        : player skin
clouds.png                      : cloud layer
default.png                     : bitmap font (ASCII grid)
misc/*.png                      : water, shadow, dial, particles, grass/foliage colour maps
mob/*.png                       : mob skins
terrain/sun.png, terrain/moon.png
item/*.png                      : arrows, boat, cart, sign
```

Packs are plain zips containing that tree at the root. HD packs use the same layout with larger
power-of-two images (32×, 64× per tile).

## Sounds

Sounds are **not** in the alpha jar. a1.1.2 downloaded them at runtime into `.minecraft/resources/`
from a Mojang server that no longer exists. The importer therefore accepts a user-supplied
`resources/` folder with the original layout:

```
sdmc:/3dalpha/resources/sound/<category>/<name>.ogg
sdmc:/3dalpha/resources/newsound/...
sdmc:/3dalpha/resources/music/...
```

If the folder is absent the game runs silently; audio is not a hard dependency. (Audio also needs a
dumped DSP firmware — see below.)

## SD card layout

```
sdmc:/3dalpha/
  options.txt              alpha-compatible options file
  3ds.ini                  3DS-specific options
  packs/*.zip              user texture packs
  packs/minecraft.jar      optional: import source for authentic a1.1.2 textures
  resources/               optional: user-supplied sounds, original layout
  cache/<pack>.3dtex       converted, GPU-ready texture blobs
  cache/<world>.idx        chunk index per world
  saves/<world>/           worlds, in the real Alpha level format
```

## Import and conversion pipeline

Conversion is expensive on a 268 MHz ARM11, so it happens **once per pack**, not once per boot.

1. Open the zip with **miniz**; decode PNGs with **lodepng**.
2. Assemble the block atlas. Missing tiles fall back to the bundled pack, so partial packs work.
3. Choose a GPU format per image — see the table in
   [3ds-performance.md §9](3ds-performance.md). ETC1A4 by default; RGBA5551 or RGBA4444 when a
   pack's gradients suffer under ETC1.
4. Morton-swizzle (via `C3D_SyncDisplayTransfer` where possible, CPU fallback under 64×64).
5. Write `cache/<pack>.3dtex` — a small header plus already-tiled texture data that can be
   `memcpy`'d straight into a `C3D_Tex`.
6. Invalidate the cache on pack mtime/size change, or on a version bump of the converter.

The bundled fallback pack ships in RomFS **already converted** — no conversion work at first boot.

## HD packs and the VRAM budget

A 64× pack means a 1024×1024 atlas: 4 MB at RGBA8, 1 MB at ETC1A4. VRAM is 6 MB total and render
targets already take 0.8–1.5 MB.

The `texture_quality` option clamps the atlas to a maximum edge length. Packs above the cap are
downscaled during conversion (box filter, once, cached) rather than rejected. If a pack cannot be
made to fit at all, refuse it with a message that names the limit — never fail silently or crash.

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

**The main menu draws with the 3DS system font today**, through citro2d's `C2D_TextParse`. That is
not a preference, it is what is available: we ship no Mojang assets, the CC BY-SA fallback pack is
not built, and there is no PNG decoder or RomFS in the tree, so there is no `default.png` for the
menu to use. `C2D_FontLoad` takes a converted BCFNT, so a pack's font replaces the system one
without touching the menu's logic. Nothing else in the menu is textured either -- see
[status.md](status.md) §0b.

`default.png` is a 16×16 grid of glyphs with per-glyph widths derived by scanning columns, exactly
as the original does. Derive the widths at conversion time and store them in the `.3dtex` cache
rather than rescanning at boot.

## Audio and DSP firmware

3DS homebrew audio through `ndsp` requires a dumped DSP firmware at `sdmc:/3ds/dspfirm.cdc`.
Nintendo's copyright prevents shipping it. Users dump it from their own console — Luma3DS Rosalina
menu → "Miscellaneous options" → "Dump DSP firmware".

The game must **detect its absence and continue silently**, with a one-line explanation in the
options screen. Audio is never a startup dependency.

## Licensing rules

| Content | Rule |
|---|---|
| Mojang textures, sounds, fonts | **Never bundled.** User-supplied only, via jar/pack import. |
| Bundled fallback pack | Must be CC-licensed and attributed in `romfs/licenses.txt`. |
| craftus_reloaded code | MIT — reusable with attribution in `romfs/licenses.txt`. |
| ViaLegacy | GPLv3 — **documentation only**. Protocol IDs and wire sizes are facts; its code is not copied. |
| Data recovered from a client jar | Facts only — ids, hardness, light levels. Recovered by a maintainer, checked in, shipped compiled. Never code, never assets, never redistributed. See below. |
| Third-party libs (miniz, lodepng, libdeflate) | Licences reproduced in `romfs/licenses.txt`. |

`romfs/licenses.txt` is shipped in the build and viewable from the in-game about screen.

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
