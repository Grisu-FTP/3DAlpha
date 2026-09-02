# CLAUDE.md

A from-scratch reimplementation of **Minecraft Java Alpha v1.1.2** for the **Nintendo 3DS**.
C++17, `-fno-exceptions -fno-rtti`, one binary per game version. Compatibility with the original is
defined at the **format and protocol** level, never at the source level.

**Read this file first, and read it instead of `docs/status.md`.** `status.md` is the authoritative
handoff document and it is ~53,000 tokens; opening it whole is almost never the right move. Use
`docs/doc-index.md` to find the section, then `sed -n 'A,Bp' docs/status.md` to read it.

## Finding things without reading everything

| I need | Go to |
|---|---|
| Which file does X? | `docs/code-map.md` — every module, one sentence each, generated |
| Which doc section covers X? | `docs/doc-index.md` — every heading in `docs/`, with line ranges |
| Why is X the way it is? | The header comment at the top of the file. Every file has one and it argues, not just labels |
| Where are we / what's next? | `docs/status.md` §*Next steps, in order* — **the section, not the file** |
| Is this number real? | `docs/status.md` §*Measured numbers (do not re-derive)* |
| What does a block do when the world ticks it? | `docs/tick-a1.1.2.md` — the clock, the update list, the per-block tables, and every deviation |
| When does music play, and why is it silent? | `docs/audio-a1.1.2.md` — the music counter transcribed, the pools, the volumes, and what is not ported yet |

Both index files are generated: `make index` (or `python3 tools/gen_index.py`). Re-run after adding
or renaming a file, or editing a heading in `docs/`. `python3 tools/gen_index.py --check` fails if
they are stale.

The project's convention is that **every source file opens with a comment explaining what it is and
why it is that way**. That comment is the cheapest thing to read and usually the answer. It is also
what feeds `docs/code-map.md`, so keeping its first sentence a real summary keeps the map useful.

## Commands

```sh
make host                 # build for Linux (SDL2) — this is the fast loop
./build-host/3dalpha_tests   # run the suite directly; prints "N/N passed"
make test                 # same, via ctest
make                      # build a1.1.2 for 3DS  (needs devkitPro at /opt/devkitpro)
make VERSION=<v>          # build another version manifest from versions/
make run                  # 3dslink the .3dsx to a console over Wi-Fi
make index                # regenerate docs/code-map.md and docs/doc-index.md
```

The host build is sanitised by default (`-DSANITIZE=ON`: ASan, UBSan, `float-cast-overflow`) and the
sanitizers instrument `3dalpha_core`, not just the tests. **690 tests pass** as of the last run.

ThreadSanitizer is a separate build, because TSan and ASan cannot be combined. Re-run it after
anything touching `WorldStreamer`'s worker, `ChunkCache`, or the audio decode thread — it has caught
two real races in the first two:

```sh
cmake -S . -B build-tsan -DSANITIZE=OFF -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread && cmake --build build-tsan
```

The host harness is how most behaviour gets exercised without a console. `./build-host/3dalpha`
with no arguments prints every mode; the ones that come up most:

```sh
./build-host/3dalpha --version                     # build configuration
./build-host/3dalpha --fly <world-dir> 8 2000 gen  # generate, mesh, stream and save a world
./build-host/3dalpha --generate [seed] [radius]    # worldgen only
./build-host/3dalpha --mesh <world-dir>            # mesh a world, report the numbers
./build-host/3dalpha --map <world-dir>             # draw the bottom screen's map
./build-host/3dalpha --world-info <world-dir>      # format, seed, on-disk cost
```

## Layout

```
src/core/       platform-independent game logic — builds and is tested on the host
src/impl/       version slots: worldgen, storage, strings, entitydata
src/platform/   ctr/ (3DS: GPU, screens, menus, HUD) and host/ (Linux/SDL2 harness)
data/<ver>/     blocks.json, packets.json — compiled into .rodata by tools/configure.py
versions/       one manifest per game version; adding one adds a CI build with no other edit
tests/          host test suite; *_vectors.hpp are oracles taken from a real JVM
tools/          extract_blocks.py, javap.py, genref.java, configure.py, gen_index.py, nbtdiff.py
shaders/        picasso .v.pica / .g.pica
docs/           see docs/doc-index.md
```

`src/core/` may not include `<3ds.h>`, `<citro3d.h>` or any devkitPro header, and may not contain
`#ifdef _3DS`. That split is what makes the test suite possible; see `docs/architecture.md`.

## Rules that are review-blocking

Full list in `CONTRIBUTING.md`. The ones that get violated by accident:

- **No `#ifdef`/`#if` on game version, anywhere.** Behavioural deltas are `if constexpr (mcver::kHasX)`;
  whole differing subsystems become slot implementations under `src/impl/<slot>/<impl>/`.
- No block, item or packet ID literal outside the generated registry/`PacketTable`. No world height,
  section size or inventory dimension as a `#define` — they come from generated `mcver::`.
- No exceptions and no RTTI: errors are return values, never `throw`, `dynamic_cast` or `typeid`.
- No allocation, no filesystem access and no decompression in the per-frame path or on core 0.
- Nothing blocks the main thread on a worker; results are drained with a try-lock and carry a
  revision stamp so stale ones are discarded.
- World writes preserve unknown NBT tags verbatim. Chunk paths are tested with negative coordinates.
- Renderer code references **render types**, never block IDs. Tick code references **tick
  behaviours**, never block IDs, for the same reason.
- Renderer or mesher changes report before/after numbers from **hardware**, not from Azahar.

## Environment facts

| Thing | Where |
|---|---|
| Shell | **fish** — does not word-split variables in `for` loops |
| Real a1.1.2 client jar | `~/.local/share/PrismLauncher/libraries/com/mojang/minecraft/a1.1.2_01/minecraft-a1.1.2_01-client.jar` — in `libraries/`, **not** the instance directory |
| Real a1.1.2 world (660 chunks) | `~/.local/share/PrismLauncher/instances/a1.1.2_01/minecraft/saves/World1` |
| devkitPro | `/opt/devkitpro`; `make` exports `DEVKITPRO`/`DEVKITARM` itself |
| 3DSX main-thread stack | **32 KB**, and nothing in the binary can enlarge it. A CIA can set `StackSize` in the RSF; a 3DSX cannot |

**Never open the real world directly.** `storage.open()` writes `session.lock` and `close()` rewrites
`level.dat`, so pointing any tool at the original modifies it. Copy it first, every time.

**Nothing about game data is ever asked of the player.** Blocks, packets and the version manifest
ship compiled in. Textures and sounds are the only user-supplied things, and even those have a
bundled fallback ("Dev Art").

`build/` and `build-*/` are gitignored but present and large (1,200+ files). `rg` and `grep` skip
them via `.gitignore`; `find` does not, so scope it to `src tests docs tools`. For status:

```sh
git status --short -- src tests docs tools README.md CLAUDE.md CMakeLists.txt
```

## Working habits for this project

- **Faithful to the game, not to its limits.** a1.1.2's freezes, its fixed tables and its
  single-threadedness are lag and era, not design. Match the *output* byte-for-byte; do not
  reproduce the stalls.
- **Derive, then measure.** Facts about a1.1.2 come out of the client jar (`tools/javap.py`,
  `tools/genref.java` under a real JVM), not from memory or a wiki. Anything inferred gets measured
  before it is called true, and dead ends stay documented so they are not re-explored.
- **Don't gate work on hardware runs.** Instrument, build, and look the console's characteristics up
  rather than waiting for a console to be in hand.
- When a milestone moves or a number is measured, update `docs/status.md` — it is the handoff — and
  run `make index` if files or headings changed.
