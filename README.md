# 3DAlpha

A from-scratch, faithful reimplementation of **Minecraft Java Alpha v1.1.2** (released 2010-09-18,
protocol version 2, server 0.2.1) for the **Nintendo 3DS**.

Not a port of the Java game — a reimplementation whose data layout, mesh format and memory budget
are chosen for the PICA200 and a 64 MB ARM11 from the first line of code. Compatibility with the
original is defined at the **format and protocol** level, not at the source level.

## Goals

- **Wire-compatible** with real a1.1.2 servers (protocol 2, offline/unauthenticated login).
- **Format-compatible** with real a1.1.2 worlds (Alpha level format) — read *and* write, round-trip safe.
- **Compatible with era texture packs** (pre-1.5 `terrain.png` layout).
- **Fast, efficient, configurable** — every option removes real work when turned off.
- **Stereoscopic 3D**.
- **One binary per game version.** Each supported Minecraft version compiles separately, with its
  own 3DS title, icon and title ID, so several install side by side and no build carries another
  version's code. `make VERSION=a1.1.2`. See [docs/build-versions.md](docs/build-versions.md).

## Status

M0 is complete and validated on a New 3DS: `make` produces a booting `.3dsx` with a hardware probe on
the bottom screen and a textured cube on the top, built exactly the way chunk meshes will be —
12-byte vertices, one shared index buffer, 4 vertices per quad, TEV lighting, stereo 3D. The heap
policy in `src/platform/ctr/heap.cpp` measures 40 MB newlib / 82 MB linear against libctru's default
91/32, which is a 2.6× larger mesh budget.

M0b settled the renderer's one open design question: day/night is a lightmap texture, not light
baked into vertex colour, so dusk rewrites 256 texels instead of invalidating every mesh. Measured
free on hardware over 93 A/B rounds. The cube vertex format is 12 bytes — with an 8-byte-per-quad
geometry-shader alternative built alongside it, because the M2 profile said the cost is on the
vertex side and that claim needs testing rather than believing. See `docs/3ds-performance.md §2`.

M1 is complete on the host target: NBT reader/writer, gzip/zlib wrappers, palette-compressed world
storage, both Alpha NBT codecs (chunk columns and `level.dat`), the storage layer that turns them
into a world on disk — base36 paths, `session.lock`, atomic writes, directory scan — and the
generated block registry. 83 tests pass under ASan/UBSan.

The block table **ships with the game**. All 70 a1.1.2 blocks — ids, hardness, blast resistance,
emitted light, opacity, render type, material, and a texture for each of the six faces — live in
`data/a1.1.2/blocks.json`, which `configure.py` compiles into a `constexpr` table in `.rodata`.
Nothing about game data is ever asked of the player: textures are the only thing they can
optionally supply, and even those have a bundled fallback. The table was derived once, by a
maintainer, from an original client jar with `tools/extract_blocks.py` rather than written from
memory — provenance, not a setup step. The per-face textures are *interpreted* out of the jar by a
small JVM interpreter (`tools/javap.py`), because in the original they are a branch on the face
index rather than a constant: grass is dirt underneath and grass on top because the bytecode says
so, not because someone typed it in. See [docs/assets.md](docs/assets.md).

**A real a1.1.2 world round-trips.** 660 chunks produced by the actual game were opened, scanned,
loaded and written back, then compared with `tools/nbtdiff.py` — an NBT reader written from the
specification rather than from our own codec, so the check is independent. **661/661 files
semantically identical, zero failures.** (0/661 byte-identical, which is correct: the original
stores compounds in a `HashMap`, so byte order is hash order for anyone.) The run found two genuine
bugs — a `Dimension` tag a1.1.2 never had, and the element type of an empty list — both fixed and
pinned by tests.

Those same 660 chunks measure a mean of **18,013 bytes per column in memory against 81,920 raw —
4.55×**, putting render distance 8 at ~5.1 MB of block data against the ~8 MB budgeted. 87 % of
nibble planes and 37 % of sections come out uniform and cost nothing.

M2 is written end to end. The section mesher, the visibility graph, the visibility walk, the VBO
pool, the column streamer and one frame's worth of orchestration all live in `src/core/` and are
host-tested; the citro3d layer on top of them is atlas, lightmap, fog LUT, stereo and the debug
overlay. 399 tests pass.

The split earns its keep: `./build-host/3dalpha --fly <world>` runs everything the console does
between reading the SD card and issuing a draw call — streaming, walking, meshing, uploading,
eviction — over a real world under sanitizers. At render distance 8 it settles at **11.15 MB
resident against the 11.15 MB the offline pool measurement predicted**, with nothing ever refused,
including while walking. What is left for hardware is the half that genuinely needs a GPU, and the
frame rate, which is the M2 gate.

Meshing all 660 columns of that same world gives **99.4 KB of vertex data per column** — 3.9x what
the block data costs, and 22.8 MB at render distance 8 against a ~12 MB VBO budget. 73 % of it sits
below y=64, cave walls nobody ever sees.

The obvious fix — let the visibility search decide what gets meshed, not just what gets drawn —
was measured and **does not work**: it saves 15 % at distance 8, because standing under open sky
nearly everything is reachable. What does work is a bounded VBO pool with eviction, because the set
that has to be resident is what a real frustum admits: **3.0–8.0 MB at distance 8**, measured every
45° around the compass. The visibility walk's real job is ordering that pool and cutting the
per-frame draw list. Both measurements, and the wrong turn, are in
[docs/3ds-performance.md §4](docs/3ds-performance.md).

**The game starts at a main menu.** Title screen, the world list — every world on the card, with
`+ Create New World` above them and delete behind a confirmation — a create screen that asks for a
name and a seed through the system keyboard, and an options screen for render distance,
autosave interval and texture packs. Blank seed rolls one; a typed seed follows the rule Minecraft
itself adopted later, so a seed swapped with someone on a PC makes the same **land** — height,
caves, ores, the Far Lands are a pure function of the seed and the chunk, checked byte-for-byte
against a real JVM. Where the trees end up has always depended on the order chunks were visited in,
in a1.1.2 as much as here. It is drawn with citro2d and the 3DS system font, and nothing in the menu itself is
textured — we ship no Mojang assets, so the backdrop is shaded quads and the buttons are rectangles.
a1.1.2's own screen is five fixed slots and never asks for a seed — both deviations are deliberate
and are written down in [docs/status.md](docs/status.md).

**START pauses, and saves.** Resume, Options, Exit World, over a world that stops dead while the
menu is up — nothing streamed, nothing generated, and the sun stands still. Opening the menu hands
everything unwritten to the I/O thread, which has the whole pause to itself, so the card is caught
up before you resume and nothing ever waited for it. Options there is literally the title screen's
own, so render distance, autosave and texture pack can be changed in a world and apply the moment
you go back to it. Exiting saves, the way closing a world any other way does.

**Nothing on the render thread touches the SD card.** Chunk reads, writes, existence checks and
directory listings all happen on an I/O thread behind a write-back cache, which doubles as a buffer
zone: a column that leaves the view is kept in RAM rather than freed, and columns just beyond it are
read ahead, so turning round or crossing a chunk boundary costs no card access at all. The original
does the same thing — Alpha's chunk provider holds a 1024-slot table and writes a chunk when it is
evicted — with a byte cap and LRU instead of a fixed direct-mapped grid.

**Chunks are generated nearest-to-the-player first**, which is what a1.1.2 does — it keeps no
generation queue at all, and the renderer that asks for chunks sorts its list by distance to the
player before rebuilding them. Outrun the generator and the ground under your feet is made before
the ground you have already left; nothing is skipped, it just waits its turn. a1.1.2 never has to
choose, because it generates inline and freezes instead — which is a limitation of doing it inline,
not something worth reproducing.

**Nothing is written outside a save**, which is the original's shape too: the autosave interval, the
pause menu, or leaving the world. A save writes the dirty columns and `level.dat` — your position,
your rotation and the world clock, so a world reopens where you left it and at the hour you left it.
The interval defaults to 45 seconds and is adjustable; a1.1.2 has no timed autosave of its own,
which the jar settles rather than folklore.

**Options → Texture Pack** lists every pack on the card, with the generated "Dev Art" pinned first,
and applies the one you pick. **Extract from a jar** turns your own `minecraft.jar` into a pack —
the PNGs are copied out verbatim, without being decompressed, into a zip in the pre-1.5 layout that
also works on a PC — and then offers to delete the jar, but only after re-reading the pack it wrote
and decoding it. HD packs load and are box-filtered down to the 256×256 atlas. The choice survives a
reboot in `sdmc:/3dalpha/3ds.ini`. Only `terrain.png` is drawn so far: a pack's GUI, font and mob
textures are kept and counted, and nothing reads them yet.

```sh
make                 # -> build/a1.1.2/3DAlpha-a1.1.2.3dsx
make run             # send it to a console over Wi-Fi via 3dslink
make test            # host unit tests under sanitizers
make host && ./build-host/3dalpha --mesh <world>   # mesh a real world, print the numbers
./build-host/3dalpha --fly <world> [dist] [frames] # run the console's render loop, no GPU
./build-host/3dalpha --mesh <world> quads          # the same, in the 8-byte quad format
```

| Milestone | Contents | State |
|---|---|---|
| M0 | Toolchain, version-driven build, 3DSX and CIA packaging | **done** — validated on hardware; CI builds both per version on every push |
| M1 | NBT, Alpha level format r/w, palette world storage, block registry | **done** — verified against a real 660-chunk world |
| M2 | Renderer: mesher, visibility, fog, stereo 3D, debug overlay | **in progress** — the whole pipeline is written and runs end to end on the host against a real world; it has not yet drawn a pixel on hardware |
| M3 | Singleplayer gameplay | not started — **except the main menu and the pause menu**: title, world list, create-with-seed, delete, options; START pauses to Resume / Options / Exit World. The game starts from it |
| M4 | a1.1.2 world generation, seed-exact | **done** — terrain, caves, the Far Lands, the whole population pass and lighting match a real a1.1.2 world byte for byte under a real JVM's own output, and generation runs on a worker thread (core 2 on a New 3DS) |
| M5 | Multiplayer (protocol 2) | not started |
| M6 | Audio, mobs, texture-pack browser, packaging | **texture-pack browser done**; **background music done, built for both targets, not yet heard on hardware** — a1.1.2's own music timer over an ndsp voice fed by a Tremor decode thread, with an Options → Sound screen and silent degradation when the DSP firmware, the resources folder or the decoder is absent. Sound effects wait on M3: nothing in the port can emit one yet. Mobs and packaging not started |

## Documentation

Two of these are **generated** — `make index` rewrites them, and `python3 tools/gen_index.py --check`
fails when they are stale. They exist so a section can be found and read on its own: `status.md` is
over 200 KB, and almost every question it answers lives in one of its sections.

| Doc | What's in it |
|---|---|
| [CLAUDE.md](CLAUDE.md) | **Start here.** Commands, layout, review-blocking rules, environment facts, and where everything else is |
| [docs/doc-index.md](docs/doc-index.md) | *Generated.* Every heading in `docs/`, with the line range it occupies |
| [docs/code-map.md](docs/code-map.md) | *Generated.* Every module, one sentence each, from the file's own header comment |
| [docs/status.md](docs/status.md) | The authoritative handoff: milestone state, measured numbers, next steps, open questions. Read a section, not the file |
| [docs/architecture.md](docs/architecture.md) | Module map, threading model, memory budget |
| [docs/worldgen-a1.1.2.md](docs/worldgen-a1.1.2.md) | The original's generator, recovered from the jar: classes, seed derivation, hazards |
| [docs/build-versions.md](docs/build-versions.md) | One binary per MC version: slots, codegen, title IDs |
| [docs/toolchain-setup.md](docs/toolchain-setup.md) | devkitPro install, building, running, emulator |
| [docs/world-format.md](docs/world-format.md) | Alpha level format + our in-memory representation |
| [docs/save-data.md](docs/save-data.md) | Where worlds live on SD, size limits, backup/transfer |
| [docs/packed-worlds.md](docs/packed-worlds.md) | Per-world packed storage: container format, lossless conversion |
| [docs/protocol-a1.1.2.md](docs/protocol-a1.1.2.md) | Full protocol-2 packet table and encodings |
| [docs/3ds-performance.md](docs/3ds-performance.md) | The 3DS optimisation playbook |
| [docs/assets.md](docs/assets.md) | Texture packs, asset importer, licensing rules |
| [docs/porting-to-other-versions.md](docs/porting-to-other-versions.md) | Adding a new game version |
| [CONTRIBUTING.md](CONTRIBUTING.md) | The anti-hardcoding rules, as a review checklist |

## Assets and legality

3DAlpha ships **no Mojang content**. It generates its own placeholder art so it is playable out of
the box with nothing to dump, copy or configure.

**Textures and sounds are the only things a player may supply, and both are optional.** Drop a
`minecraft.jar` or a texture-pack zip in `sdmc:/3dalpha/packs` for authentic visuals and pick it
from Options → Texture Pack; drop an original `resources/` folder in `sdmc:/3dalpha/` for sound,
which a1.1.2 downloaded at runtime and never shipped. Without either, the game is complete and
playable — placeholder textures, no audio.

Music also needs a DSP firmware, which Nintendo's copyright means no homebrew can ship: dump your
own console's with Luma3DS's Rosalina menu → Miscellaneous options → Dump DSP firmware. Without it
the game runs silently and Options → Sound says why. With both, background music starts on a1.1.2's
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
