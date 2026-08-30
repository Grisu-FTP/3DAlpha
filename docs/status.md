# Project status and handoff

The authoritative "where we are" document. Everything here is either a decision that has been made,
a number that has been measured, or a task that is next. Estimates and guesses are labelled as such.

Update this file when a milestone moves.

**It is no longer the first thing to read.** `CLAUDE.md` at the repo root is the entry point — it
carries the commands, the layout, the review-blocking rules and the environment facts in about a
fortieth of the length. This file is where you come once you know which question you have, and the
way to read it is one section at a time: `docs/doc-index.md` lists every heading here with the line
range it occupies, so `sed -n 'A,Bp' docs/status.md` beats opening the whole file.

---

## Milestones

**What "ready" means, since the milestone names do not say it.** Nothing here is being shipped, so
"first shippable" was never the right frame and has been dropped. Ready means **a faithful 1:1 port
of Alpha 1.1.2**: it opens real Minecraft worlds, saves them, and generates them the same way the
original does, on a 3DS. That makes M4 part of the definition of done rather than a later
nice-to-have, and it is why worldgen is being built before gameplay — of the two, only worldgen has a
hard oracle to check itself against.

| Milestone | State |
|---|---|
| **M0** Toolchain, version-driven build, 3DSX packaging | **done** — validated on a New 3DS. `make cia` is wired but inert until `makerom` is on PATH |
| **M0b** Day/night design decision | **done** — lightmap texture, measured free on hardware |
| **M1** NBT, Alpha level format r/w, palette storage, block registry | **done** — verified against a real 660-chunk world. **Plus a second on-disk format**, `Packed`: sector-allocated region containers, 9× smaller than the folder layout on a 16 KB-cluster card and 280× fewer file operations, converted losslessly in either direction and byte-exact on a real 1,119-chunk world. New worlds are created in it. See §0j and [packed-worlds.md](packed-worlds.md) |
| **M2** Renderer | **in progress; the gate failed and the answer to it is built but unrun** — the whole pipeline exists and runs end to end on hardware. Six launches that ran: a stack overflow, a VRAM write, a wrong daylight curve, fog/depth/frame-time, black torches, and the profile below. **Two more did not launch at all, and neither was a bug in the build** — `loader` refused the file on the SD card both times, which looks exactly like a crash; see §1. **The 12-byte/4-vertex path costs 0.208 µs per quad and misses the M2 gate by 3.2× at distance 10, and by an estimated 2.1× at the distance 8 the New 3DS gate has been lowered to.** The geometry-shader path §2 always pointed at now exists, is measured on the host, and needs a seventh launch to say whether it closes the gap |
| M3 Singleplayer gameplay | not started as *gameplay*, **but its foundation is now built: the world ticks.** a1.1.2's 20 Hz clock (`ir.class`, accumulator, partial ticks, the ten-tick cap that drops rather than defers), its scheduled-update list with the original's ordering and both of its limits, its 80-samples-a-chunk random tick, and fifteen block behaviours -- grass, leaves, saplings, crops, farmland, flowers, mushrooms, sugar cane, cactus, ice, both snows, torches, sand and gravel. Blocks are dispatched by a **tick behaviour**, never by id, the way the renderer dispatches on render type. Redstone, fire and the fluids are named and not done; see §0o and [tick-a1.1.2.md](tick-a1.1.2.md). Also **the main menu and the pause menu, which are built** — title, world list, create-a-world with a typed seed, delete, and an options screen for render distance. The game now starts from it rather than opening whatever `readdir` returned first; see §0b. **START pauses instead of exiting**: Resume, World Settings, Options, Exit World, over a world that stays open and stops dead while the menu is up. **World Settings is the world's own screen, as against Options, which is the console's** -- Gamemode, Format, Size, Copy, Delete, reached from the pause menu and from `X` on the world list, and cut to Gamemode alone when a world is open behind it. Gamemode is real: it lives in `<world>/3dalpha.ini`, a file of ours that a real Alpha client never reads, because a1.1.2 has no gamemode key for `level.dat` and a per-world value has no business in `3ds.ini` either. **Spectator is the only implemented mode and it is the honest one** -- there is no player body yet, so movement is free flight with no collision; Survival and Creative are drawn disabled. **Worlds have a storage format now**, Folder or Packed, converted losslessly from that screen; new worlds are packed. See §0j. Built and linked; not yet seen on hardware. It is the same `Menu` object, so Options and Texture Pack in a world are the ones the title screen uses and both apply live; see §0d. **It is transparent**: the world is redrawn behind it every frame and dimmed with a1.1.2's own gradient, because the menu now draws into the renderer's frame rather than into a target of its own. **Opening it costs a frame** -- the two card listings that used to run on every `Menu::init` are asked for by the screens that show them instead. **Opening it also saves**, which is more than the original does -- a1.1.2 only writes everything out on Save and quit to title -- and only when a column is dirty, so pausing twice costs one save. Leaving a world counts the drain out as a percentage. Options has an autosave row beside render distance and texture pack. **The bottom screen is now a tabbed HUD, and the debug pages are behind it rather than beside it.** The player's half is three pages switched by touching tabs along the top -- **Map**, **Items** and **Look** -- drawn as panels, slots and bevels in a1.1.2's own GUI colours over the pack's tiled dirt, while the three debug pages stay shared, unchanged and behind `SELECT + Y`. It is still libctru's text console underneath: the furniture is written straight into the RGB565 framebuffer and the console's glyphs are printed on top of it, on backgrounds set per row with `\x1b[48;2;R;G;Bm`, which is what stopped text punching black rectangles through the panels. **Map** is 208 by 200 blocks at one pixel per block, centred on the player, drawn from the columns the streamer already holds -- **in every gamemode now, not only Spectator's** -- and it shows **coordinates and nothing else**: the chunk, the map tile, the chunk count and the redraw time were the maintainer's questions and have moved to the Info page, and the debug grids to the settings page. **There is no strip of button hints along the bottom**, because it was the same three lines on every page spending a twelfth of the screen to repeat itself; the debug pages keep theirs, which is where `SELECT + Y` is worth naming. Those 24 rows and 24 columns off the coordinate panel are what made the window a quarter bigger than the 192 by 176 it started at -- an estimated 790 us a redraw against 700, measured on the host as 16.9 us against 14.2. The marker is an **arrowhead rotated to a real yaw** rather than a diamond with a whole-block tick, which is what fixes both of the old one's faults at once -- it pointed eight ways and it was 41 % longer on a diagonal than on a straight. **Items** is the inventory frame, nine across with a hotbar, drawn empty until M3 fills it, and it is not offered in Spectator. **Look** is a pad that hands the drag to the camera, with a compass ribbon over it -- which exists because the bottom screen is both the UI and the only pointing device an old 3DS has, and dragging on a map used to turn the view. **Run on hardware, where a redraw read 5,000 us**; a per-chunk patch cache took that to a copy, and the number is on the Info page. **The HUD itself has not been seen on hardware.** See [map.md](map.md) |
| **M4** a1.1.2 worldgen, seed-exact | **done, wired, and on a worker thread.** Terrain, caves, **the Far Lands**, lighting, the whole population pass, **and `ft`, the chunk provider above them all** match a real a1.1.2 World byte for byte, reflected under a real JVM by `tools/genref.java`. `ChunkGenerator` turns "there is no chunk here" into a finished, populated, lit column, and `WorldStreamer` now asks it for one and writes what comes back — so the game makes world where there is none, which is what an Alpha world does. **Generation runs on its own thread**, below the render thread, so making ground costs latency rather than frame rate — and the world it produces is byte-identical to the one generating inline produces, which is a test rather than a hope. `--fly <empty-dir> 8 2000 gen` creates a world, generates it, meshes it and saves it under sanitizers. It found a real bug in `WorldGenBigTree` that no per-generator test could. See [worldgen-a1.1.2.md](worldgen-a1.1.2.md). **Run on hardware now, and the cost is exactly what was predicted**: generation is slow and a walking player outruns it and never sees it catch up. The cause was not the generator but the thread it was on — `std::thread` had put it on core 0 at the bottom priority, where it ran on scraps. It is on **core 2** on a New 3DS now; see §0 |
| M5 Multiplayer (protocol 2) | not started |
| M6 Audio, mobs, texture-pack browser, packaging | **the texture-pack browser is done and run on hardware, and background music is built but unheard**, both ahead of the rest of M6; mobs and packaging not started. **Music is a1.1.2's `of.c()` transcribed exactly** -- the counter seeded at `nextInt(12000)` and reset to `nextInt(24000)+24000`, and, the part that is easy to lose, *not decremented while a track is playing*, so the period is the track's own length plus 20-40 minutes. It runs on the same `elapsedTicks()` the world does, beside `stepTicks`. Underneath it: `audio::Backend` (the `IAudio` docs/architecture.md always named), an ndsp voice on channel 0 fed by a ring of eight 1024-frame wave buffers in linear memory, and a third `WorkerRole` -- `Audio` -- decoding Ogg Vorbis through Tremor. **On an Old 3DS that decode is on core 0**, because a 3DSX has no other core; it sits below the main thread and the 186 ms ring is what makes that safe, which is a claim only hardware can settle -- the overlay counts underruns and decode microseconds for exactly that reason. Options -> Sound carries the volumes and the one line that says why a console is silent, distinguishing a missing DSP firmware from a DSP something else is holding. **Sound effects are deliberately absent**: nothing in the port can emit one yet, so the machinery arrives with its first caller rather than as dead code. See [audio-a1.1.2.md](audio-a1.1.2.md). Options -> Texture Pack lists the packs on the card and applies one; Extract from a jar turns a player's own `minecraft.jar` into a pack and offers to delete the jar afterwards; the generated art is now "Dev Art", one pack among them. **Three of a pack's files have consumers now**: `terrain.png` is the block atlas, and `default.png` and `dirt.png` are the menu -- the font every label is drawn with and the backdrop behind them, both a1.1.2's own rules read out of the jar, both optional and both with a fallback that needs no file. A pack's gui, mob and item textures are still carried and counted and unread. **The menu art is built and not yet seen on hardware.** See §0c and [assets.md](assets.md) |

**644 tests pass** under ASan/UBSan/float-cast-overflow, at `-O3`, and the 3DS target links clean.
**They also pass under ThreadSanitizer, which reports no races** — a separate build, because TSan and
ASan cannot be combined: `cmake -S . -B build-tsan -DSANITIZE=OFF -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread`. It is worth re-running after anything that touches
`WorldStreamer`'s worker or `ChunkCache`: it found the first version reading the generator's counters
from the main thread while the worker wrote them, and then found the same shape again in
`ChunkCache::stats()`, which handed out a reference to counters the I/O thread writes. Both return
locked copies now.

**The sanitizers now instrument `3dalpha_core`, which they did not before.** They had been attached
to the test executable alone, so they covered `tests/*.cpp` and nothing else — the NBT parser, the
mesher and the world generator were all built unsanitised while the suite looked fully covered.
Moving them onto the core library as `PUBLIC` found a zero-length `memcpy` from an empty vector's
null `data()` in `MeshBuilder::copyTo` on the first run. `float-cast-overflow` is named explicitly
because **GCC leaves it out of the `-fsanitize=undefined` group**.

---

## Environment facts worth not rediscovering

| Thing | Where |
|---|---|
| Repo | `/home/grisu/Documents/GitHub/3DAlpha` — **a git repository**, whose `build/` and `build-host/` are committed, so filter status with `git status --short -- src tests docs README.md CMakeLists.txt` |
| Real a1.1.2 client jar | `~/.local/share/PrismLauncher/libraries/com/mojang/minecraft/a1.1.2_01/minecraft-a1.1.2_01-client.jar` — in `libraries/`, **not** in the instance directory |
| Real a1.1.2 world (660 chunks) | `~/.local/share/PrismLauncher/instances/a1.1.2_01/minecraft/saves/World1` |
| Shell | **fish** — does not word-split variables in `for` loops |
| 3DSX main-thread stack | **32 KB**, and no symbol in the binary can enlarge it. Measured from a crash dump, not assumed: entry `sp` was `0x08008010` and everything below `0x08000000` faults. A CIA can set `StackSize` in the RSF; a 3DSX cannot |

**The real world is only ever touched through a copy.** `storage.open()` writes `session.lock` and
`close()` rewrites `level.dat`, so pointing a tool at the original modifies it. Copy to the
scratchpad first. Verify afterwards by checking that no file in the original is newer than the
session started.

```sh
make                 # -> build/a1.1.2/3DAlpha-a1.1.2.3dsx
make test            # host unit tests, ASan/UBSan
make host            # host binary only
tools/check3dsx.py <file.3dsx>                      # would Luma's loader accept this image?
tools/lumadump.py <dump.dmp> [--elf <elf>]          # read a Luma exception dump
./build-host/3dalpha --mesh <world-dir> [quads]      # mesh a world, print every M2 number
./build-host/3dalpha --generate [seed] [radius] [snow] [cache-columns] [raster]
                                                    # make a fresh world, print every M4 number
./build-host/3dalpha --fly <world-dir> [dist] [frames] [switch-to] [quads|flip|gen|gensync]
./build-host/3dalpha --pack <zip|dir|devart>        # assemble a pack's atlas, write atlas.pam
./build-host/3dalpha --extract-jar <jar> <packs-dir> # the console's jar importer, sanitised
./build-host/3dalpha --map <world-dir> [pack] [grid] # draw the spectator screen's map of a
                                                    # whole world, write map.pam, report cost
```

A trailing **`quads`** on either harness puts the cube range in the geometry-shader format — one
8-byte vertex per quad instead of four 12-byte ones. **`flip`** (`--fly` only) starts in the 12-byte
format and changes over halfway, which is what the settings page does and the path worth sanitizing.
**`gen`** (`--fly` only) is the console's configuration rather than the harness's: a missing chunk is
generated and written back instead of counted absent, the world is created if the directory has
none, and the chunk cache is threaded with a two-ring read-ahead band. **`gensync`** is the same
world made the old way — every read and write on the calling thread, nothing read ahead — and it is
there for one purpose: *generate a seed both ways and diff the trees.* They must be identical, and
that is the check that the cache changed what chunk I/O costs rather than what it says. Every other
`--fly` invocation leaves the cache unthreaded so the documented numbers keep meaning what they did. `--fly <empty-dir> 8 2000 gen` is the whole recipe for exercising creation, generation,
streaming, meshing and saving end to end under sanitizers. **It writes to the directory it is given.**
It also sleeps a millisecond a frame, because generation is on another thread now and a host with
nothing to draw would otherwise finish its frame budget before the worker had made anything; expect
a sanitized run to fill far less than an `-O3` one in the same frame count.

Two environment variables put the console's pacing on the host, and `--fly … gen` is where they
belong: **`MC_IO_LATENCY_US`** is what one storage operation costs (~4000 is a card; 0, the default,
is the host's page cache) and **`MC_FLY_FRAME_MS`** is how long a frame takes (33 is the console's
refresh rate; 1 is the default). Together they are what made §0k reproducible without hardware.
`MC_FLY_AUTOSAVE` overrides the autosave interval.

`--fly` is the one to reach for when something is wrong with the renderer. It runs everything the
console does between reading the SD card and issuing a draw call — streaming, the walk, meshing,
uploading, eviction — over a real world, under sanitizers, and prints counters instead of pixels.
The console and the host run the same `core/render/` code; only who draws the result differs.

---

## What exists in the tree

```
core/nbt/         reader, writer, generic tree, preserved-tag passthrough
core/world/       Section (palette), NibbleArray, ChunkColumn, LevelData, storage contract,
                  ChunkCache -- the write-back chunk cache, the leaf-directory existence
                  index and the I/O thread; the only thing that touches the storage slot
core/block/       BlockDef, RenderType, registry; the table is generated
core/item/        ItemStack
core/io/          FileSystem seam + the POSIX implementation both targets use
core/texture/     PNG decoder, zip reader/writer, pack listing, atlas assembly, Dev Art, jar import,
                  the PICA tiling map the CPU writes textures through
core/settings/    3ds.ini -- render distance, texture pack, autosave interval, cache size
core/map/         the bottom screen's map, with no screen in it: chunk sampling, the
                  palette built from the texture pack, the sample store and the shading
core/mesh/        three vertex formats, MeshScratch, mesher, fluid, torch, visibility masks
core/render/      SectionField + buildVisibleSet (the visibility walk); VboPool;
                  ChunkRenderer (one frame, no GPU); WorldStreamer (columns in and out,
                  and the chunk generator when the world has none)
core/util/        types, span, nibble, compress, math (Vec3/Mat4/Plane), frustum, the
                  worker-thread seam (which core a background thread gets, per role),
                  the free-heap seam (how much room is left, for the caches that can use it),
                  coord and seed text parsing (what a player types, parsed where it can be tested)
core/world/       ...and the light engine: sky and block light as one fixed point;
                  world_list -- the saves folder as a list, without opening anything
impl/storage/alpha_chunkfiles/   chunk paths, chunk NBT, level.dat, storage slot
impl/worldgen/alpha_nobiome/     noise, terrain, caves, the nine population generators,
                  the pass driver, and ChunkGenerator -- `ft`, which decides what gets
                  generated and when it gets populated
platform/ctr/     heap policy, VBO allocator, atlas/lightmap/fog, citro3d renderer,
                  citro2d main menu, the bottom screen (one per gamemode, plus the three
                  debug pages) and the spectator map's framebuffer half, game loop, M0 probe
platform/host/    harness: --version, --mesh, --fly, --generate, --map
tools/            configure.py, extract_blocks.py, javap.py, nbtdiff.py, genref.java
```

The split that matters: **everything deciding *what* to draw is in `core/render/` and host-tested.**
`platform/ctr/renderer.cpp` is left with the part that genuinely needs a GPU — bind a buffer, set a
matrix, draw — and `platform/host/main.cpp --fly` drives the same core code with no GPU at all.

---

## Decisions that are settled

These were argued or measured; do not relitigate without new evidence.

- **C++17**, `-fno-exceptions -fno-rtti -fno-threadsafe-statics`.
- **Nothing on the render thread touches the SD card.** `core/world/chunk_cache.hpp` owns the
  storage slot and is the only thing in the process that reaches it. Reads, writes, existence
  checks and directory listings happen on an I/O thread; the main thread's half of the API answers
  "not yet" and retries next frame rather than blocking. The card is not the limiter and a faster
  one changes nothing — a chunk file is 2,917 bytes at the median, so the cost is four to six IPC
  round trips per operation, not bandwidth. **Internal storage is not a second tier either**: a
  title's save data and extdata live on the SD card, CTRNAND is not writable from a 3DSX, and both
  go through the same sysmodule. See [save-data.md](save-data.md).
- **The chunk cache changes what a read costs, never what it says.** Every read returns
  byte-identical content whether it comes from the table or the card, and `hasChunk` answers true
  from the moment a save is accepted rather than from the moment bytes land. That is not a nicety:
  cell classification feeds the generation queue and population order *is* the world, so an
  existence answer that moved in time would produce a different world. Held in place by the third
  arm of `a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up`, which fills the same
  seed three ways — inline, on a worker, and on a worker with the cache threaded and reading ahead —
  and compares the trees chunk file for chunk file.
- **a1.1.2 has no timed autosave, so the interval is ours.** From the jar: `ft.saveChunks(saveAll,
  progress)` writes at most two dirty chunks per call when `saveAll` is false, and its only periodic
  caller is the "Saving level.." screen reached from Save and quit to title; otherwise a chunk is
  written when it falls out of the provider's 1024-slot table, synchronously, on the main thread.
  Ours defaults to 45 seconds and covers dirty columns, `level.dat` (position, rotation and the
  world clock), the `session.lock` refresh and — from M3 — player edits.
- **Nothing is written outside a save**, and this was argued and then reversed. The first version
  queued a generated column for writing the moment it was made, on the grounds that a power-off
  would otherwise lose finished world *and lose it in a way regenerating cannot reproduce*:
  population passes spill across chunk borders, so a column that comes back regenerated against
  neighbours already marked `terrainPopulated` is missing whatever their passes had put into it.

  That reasoning is sound and the conclusion was still wrong, because **the original has the same
  hazard in a worse form**. `ft`'s table is indexed by `(x & 31, z & 31)`, so a chunk is only
  written when something 32 chunks away collides with its slot and can otherwise sit unwritten for
  an entire session; task-killing Alpha loses it identically. Writing eagerly made us safer than
  a1.1.2 rather than correct where it was wrong, and it put a deflate and six file operations on
  the card every time a column was finished. What bounds the exposure is the autosave interval,
  which is what the interval is for. The one exception is memory: a dirty column cannot be evicted
  because it is the only copy of that world, so over `dirtyCapBytes` the thread that dirtied it
  writes one itself.
- **A background thread's core is a platform decision, and it now carries a role.**
  `mc::setWorkerThreadOps` in `core/util/worker.hpp` takes `WorkerRole::Generation` or
  `WorkerRole::Io`, because the two want opposite things: generation is tens of milliseconds of pure
  CPU and wants core 2 on a New 3DS, while I/O is almost entirely blocked in FS IPC and wants core 0
  one priority step below the main thread, where it runs in the VBlank slack and cannot delay a
  frame. It used to be a pair of statics on `WorldStreamer`; it moved out when the cache gained a
  thread of its own.
- **`opaque` and `opaqueCube` are two different columns and both are needed.** `opaque` is what
  `isOpaqueCube()` answers at runtime and drives face culling; `opaqueCube` is
  `Block.opaqueCubeLookup`, filled in the Block constructor, and is what world generation reads.
  They differ for leaves (the live answer is `!fancyGraphics`, and a1.1.2 defaults to fancy) and for
  the double slab. Do not collapse them -- headless they agree, so no oracle will catch it.
- **Light is computed as a fixed point, not as a replay of the original's queue.** a1.1.2 relights
  through a queue of bounding boxes that re-scans; the rule it applies has exactly one solution, so
  a bucketed BFS that settles each cell once lands on the same numbers and is affordable on a
  268 MHz ARM11. Verified against a real World drained to a standstill. The one divergence is block
  light *inside* fully-opaque blocks — cave lava the original never schedules — which no cell can
  receive from and the mesher never samples. See [worldgen-a1.1.2.md](worldgen-a1.1.2.md).
- **The Far Lands generate; the jitter does not.** The Far Lands are terrain — a `d2i` overflow at
  block 12,550,824 that the generator drives into by design — so a 1:1 port reproduces them, and
  four fixtures compare them byte-for-byte against the jar. The one-block camera stutter that Java
  also shows out there is *not* terrain: it is float precision in the original's renderer, it
  changes nothing that reaches a save file, and it is fixed by holding the camera in double (as
  `Entity.posX` already is) and rendering relative to the camera's chunk. Faithful where the world
  is concerned, fixed where only the screen is. See
  [architecture.md](architecture.md), "Rendering far from the origin".
- **One binary per Minecraft version.** Slots bound statically through `mcver::`, no vtables, no
  runtime version branching, no JSON parsed at runtime.
- **Cube vertex is 12 bytes, 4 vertices per quad**, one global immutable index buffer. Final.
- **Non-cube geometry gets its own 16-byte vertex**, position in 1/1024 of a block. The 12-byte
  format stores position as one byte per axis at one unit per block and cannot express anything
  that is not a cube; widening it for everyone would add ~10 MB to a real world's geometry and put
  render distance 8 over the old 3DS's 12 MB. Non-cube blocks are **2.7 % of a measured world**, so
  they carry the cost alone.
- **A section's mesh is one allocation holding three ranges**, drawn as three passes: cubes, opaque
  detail, then translucent detail sorted back to front. `mesh::MeshRanges` is the layout and the
  pool carries it. The split is about *draw order*, not ownership -- the passes are what change the
  shader, the attribute layout and the blend state, and each of those changes a fixed number of
  times per eye rather than once per section. All three share the one index buffer.
- **Day/night is a lightmap texture**, not light baked into vertex colour. Measured free (M0b).
- **Heap policy 40 MB newlib / 82 MB linear**, replacing libctru's 91/32. Measured on hardware.
- **POSIX file descriptors, one implementation for both targets.** Settled by disassembling libctru
  2.7.0: `archive_read` calls `FSFILE_Read` directly and `archive_dirnext` batches 32 entries per
  `FSDIR_Read`. The devoptab is a thin wrapper; raw `FSFILE` would buy nothing.
- **Tile entities are a per-chunk side table, not per-block objects.** Java gives every chest, sign,
  furnace and spawner a heap object and walks all of them every tick, making a virtual call even for
  the ones that do nothing. In a1.1.2 that is most of them: `ic.b()` is the tick the World calls, and
  only `ke` (furnace, smelting) and `bd` (mob spawner) override it. **Chest and sign have no tick
  behaviour at all** -- the chest lid animation people associate with the cost arrived later.

  So contents live in a per-chunk table keyed by packed position, and **only furnaces and spawners
  are visited by a tick list**. Chests and signs are pure data. Rendering needs nothing either:
  chest and furnace are both render type `cube` with per-face textures, and nothing in the jar
  renders the chest tile entity specially, so a chest is drawn by the same `faces[]` plus
  neighbour-lookup path grass already uses. Signs are the exception -- not a cube, so they need a
  mesh emitter -- but they are still not tickable.

  **This is a runtime representation only. Saves stay byte-for-byte 1:1**: the same `TileEntities`
  NBT list goes back out. Today it round-trips as an opaque blob, so compatibility is already there
  and the work is about making the contents *functional*.
- **Faithful including quirks.** 3DS conveniences are opt-in, default off.
- **The player supplies nothing.** Textures and sounds are the only optional user-supplied assets.
  All game data ships compiled into the binary. See CONTRIBUTING.md.

### Reversed, and why (keep this — the reasoning matters)

M2 measurement contradicted an inference recorded in `docs/3ds-performance.md §4`: that because 73 %
of geometry is underground, the visibility search should decide what gets meshed. It saves only 15 %
at render distance 8, because standing under open sky nearly everything *is* reachable. The bound is
a **budgeted VBO pool with eviction**; the walk's job is priority order and cutting the per-frame
draw list. The dead end is left in the doc deliberately.

---

## Measured numbers (do not re-derive)

> **Audio costs about 99 KB of binary and nothing else measured yet.** Linking Tremor takes `.text`
> from 762,772 to 809,852 and `.rodata` from 68,916 to 120,540 -- a1.1.2 Release build, `-O3`.
> The `.3dsx` goes 861,252 -> 960,224 bytes. **Every other audio number in this document is a
> prediction, not a measurement**: decode cost per buffer, underruns and whether the Old 3DS thread
> policy holds are all waiting on a console, and the debug Info page exists to answer them in one
> launch. See §0p.

> **The reference world grew between M1 and the sixth launch, and the totals below are the 660-column
> world.** It has been played in Java since: it is now **1,118 columns**, and a re-run gives
> 2,132,350 quads and 107.29 MB of geometry against the 1,370,144 and 64.05 MB recorded here.
> **Every per-column and per-section figure held** — 98.3 KB a column against 99.4, 30.0 µs a
> section at `-O3` against 28.5 — and re-sweeping the size classes over 1.7× the geometry still
> picks **ratio 1.15 at 7.6 % waste**, which is the strongest evidence yet that the ratio was not
> overfitted. Only the totals moved. Anything below quoted as a total is a fact about the old world;
> anything quoted per column or per section is still current.
>
> It also **has 24 torches now**, which several passages below say it does not — see the sixth
> launch. Their metadata is 2, 3 and 5 only; the −X and +Z wall mounts remain unexercised.
>
> **It has grown again** and is now **1,119 columns**, 2,135,496 quads, 107.45 MB. Anything pinned to
> a total moves with it: `--fly` at distance 10 over 1,200 frames now settles at **frame 273**, not
> the 277 recorded further down. That was checked rather than assumed — the settle frame moved and
> the column count moved with it, in the same run. Per-column and per-section figures are unaffected.

**M0, New 3DS hardware.** Heap/linear 40/82 MB. VRAM free after two stereo top targets 5,019 of
6,144 KB. Fill rate ~210 M fragments/s (4.777 ns each, two textures bound). Fixed cost ~562 µs per
frame. SD cluster size **16 KB**, not the assumed 32.

**M1, the real 660-chunk world.** 661/661 files semantically identical after a load/save round trip,
0 byte-identical (correct — Java stores compounds in a `HashMap`). Mean **18,013 bytes per column in
memory** against 81,920 raw = **4.55×**. 37 % of sections and 87 % of nibble planes come out
uniform. Exactly one Palette8 section and zero Direct16 in the entire world. Median gzipped chunk
file 2,917 bytes.

**M4, a fresh world.** `--generate`, dev host at `-O3` with no sanitizers. **A generated column is
milliseconds, not microseconds**: 5.9 ms nearest-first and **3.8 ms raster** at radius 11, which is
roughly 25x the cost of meshing the same column. The order is worth 1.6x on its own, because each
request sweeps a 6x6 and a spiral re-sweeps what a raster keeps -- 3.20 terrain columns generated per
delivered column against 1.59. Peak resident in the generator is **two rings of frontier**, 98 / 162
/ 210 columns at radius 4 / 8 / 11, so 3.1 to 6.6 MB of raw block arrays; `cacheColumnsFor()` is
fitted to those three points and `evictedLive` is the counter that says it was fitted wrongly. A
delivered column palette-compresses to **15.2 KB**, against the 18.0 KB mean of the real played
world -- a fresh chunk has no player-made structures in it. Full table and the derivation in
[worldgen-a1.1.2.md](worldgen-a1.1.2.md).

**M2, the same world.** 1,282,900 cube quads and 87,244 in the 16-byte detail format -- 31,626 of
them opaque and 55,618 translucent -- over 660 columns = **99.4 KB of vertex data per column**. Worst section **3,281 quads** against the 12,288
the index buffer is sized for. 36 % of sections are uniform air and rejected before meshing; another
517 of the 3,370 meshed produced nothing. **28.5 µs per section** on the dev host at `-O3`, split
10.0 µs filling the scratch and 19.1 µs emitting faces. Metadata is about 1.3 µs of that fill and
was added for the shapes that bend to it.

**Every byte figure in this block is the 12-byte format's.** In the geometry-shader format the same
world's geometry is 25.96 MB and 23.8 KB a column, and a section meshes in 28.7 µs rather than 31.6
— identical quad counts, so it is a substitution and not a change. The quads, the height
distribution, the render-type census and the atlas histogram below are properties of the world and
do not move. See `docs/3ds-performance.md §2`.

Fluid is 8.9 % of those bytes and 2.7 µs of that emit — **measured against the same world with the
fluid emitter switched off**, not estimated, because "2.7 % of blocks" says nothing about cost when
each of those blocks samples sixteen cells for its corner heights. Cube quad count is unchanged to
the last quad, which is the check that fluid was added rather than substituted.

Take these as best-of-five: the first `--mesh` after a rebuild reads 40 % high because it is also
paging 660 chunk files in for the first time. Chasing that as a regression wastes an afternoon.

That split is why it is no longer 30.4 µs. The scratch fill was 14.2 µs — **46 %** of the time to
mesh a section, and it cost that whether the section produced 3,281 quads or none, because it is
5,832 lookups either way. It was reading every cell through `ChunkColumn::block`, paying a bounds
check, a division by the section height and a switch on the section's encoding 5,832 times over.

Both `Section` and `MeshScratch` index Y-fastest, so a sixteen-block run of Y is contiguous in both;
`Section::readBlocks` and `readPackedLight` now move one such run per call with the encoding switch
hoisted out. That is 4,096 of the 5,832 cells no longer touching `ChunkColumn` at all. The fill fell
**38 %** and the whole mesh **18 %** -- to 8.8 and 24.8 µs, before metadata was added -- with every
output number over the 660-column world unchanged.
`tests/scratch_test.cpp` keeps the old per-cell version as a reference implementation and checks the
two agree across all four section encodings, every section height, partial neighbourhoods and both
ends of the world.

Height distribution — 73 % of all quads sit below y=64:

```
y   0- 15  31.1 %      y  64- 79  19.2 %
y  16- 31  15.9 %      y  80- 95   7.1 %
y  32- 47  11.9 %      y  96-111   0.3 %
y  48- 63  14.4 %      y 112-127   0.0 %
```

**What is not a cube**, from the same run — the number the second vertex format was sized against:

| render type | blocks | share of all solid blocks |
|---|---|---|
| cube | 11,417,426 | 97.263 % |
| fluid | 320,790 | 2.733 % |
| cross | 474 | 0.004 % |
| cactus | 6 | 0.000 % |

One lightly-built world, so a heavily-played one would have far more torches and the rails this has
none of, but
fluid dominating is a property of natural terrain rather than of this save. Cross geometry costs
1,896 quads in the 16-byte format, 0.15 % of all quads and 0.11 MB across the whole world; fluid
costs **85,348 quads and 5.21 MB**. That is 0.27 quads per fluid block — most of that 2.73 % is
buried in a lake and emits nothing.

Atlas coverage, from the same run: the world's geometry touches **22 of the 256 tiles**, and the top
eight carry 98 % of it.

| Tile | Quads | Share | |
|---|---|---|---|
| 1 | 672,797 | 52.4 % | stone |
| 17 | 178,389 | 13.9 % | bedrock — the world's floor emits its downward faces |
| 0 | 110,962 | 8.6 % | grass top |
| 2 | 99,231 | 7.7 % | dirt |
| 52 | 98,502 | 7.7 % | leaves |
| 3 | 56,655 | 4.4 % | grass side |

Tile 0 is the per-face table doing its job. It is grass's top, which is reachable only through
`faces`; before this, those 110,962 quads — **8.6 % of the world**, all of it the ground the player
walks on — drew the grass *side* texture. (Grass's bottom moved too, from tile 3 to tile 2, but
those faces are almost all culled against the dirt underneath.)

Reachable vs drawn, from the player's actual position:

| Distance | Everything in range | Walk reaches | Drawn, real 70° frustum |
|---|---|---|---|
| 6 | 12.53 MB | 7.86 MB | 1.35–4.28 MB |
| 8 | 22.76 MB | 19.49 MB | **2.95–8.01 MB** |
| 10 | 34.76 MB | 28.60 MB | — |
| 12 | 52.86 MB | 44.00 MB | — |

**The VBO pool, same world, three full turns on the spot with a real frustum.** Size classes are
geometric at **ratio 1.15**, 42 of them, smallest 2 KB — chosen by sweeping the ratio over all 2,853
real section meshes *and* re-running the pool at its ceiling for each. Powers of two waste 46 % and
fit only 8.12 MB of geometry in a 12 MB pool; 1.15 wastes 7.7 % and fits **11.15 MB**. The expected
cost of finer classes — a freed block matching a new mesh less often — turned out to be six points
of recycling rate and three and a half extra allocator calls per frame. Re-swept once fluid existed
and **the answer did not move**, which is worth knowing: the ratio was not overfitted to an all-cube
world.

| Configuration | Peak resident | Held | Evictions / 48 frames | Refused |
|---|---|---|---|---|
| o3DS, distance 6, 12 MB | 7.69 MB | 8.29 MB | **0** | 0 |
| o3DS, distance 8, 12 MB | 11.15 MB | 12.00 MB (ceiling) | 2,311 | 0 |
| n3DS, distance 8, 32 MB | 18.49 MB | 19.92 MB | **0** | 0 |
| n3DS, distance 10, 32 MB | 27.63 MB | 29.82 MB | **0** | 0 |

**Both M2 gate configurations turn on the spot without evicting anything**, and n3DS distance 8 --
the New 3DS gate since the frame-rate floor was lowered -- does it at 19.92 MB of a 32 MB pool.
Distance 8 on an old 3DS is the one that runs at the ceiling and churns instead: 59 re-meshes per frame while turning, 75 % of them
served from a recycled block with no allocator call, and never a refused upload. At the measured
28.5 µs per section that is an *estimated* ~1.7 ms of worker-thread meshing per frame.

**The tightest configuration is now n3DS distance 10**, which holds 29.82 MB of its 32 MB pool
against 26.95 MB before fluid. Nothing is refused and nothing is evicted, but that is 2.18 MB of
headroom, and it is the configuration the *next* render type to gain an emitter has to be measured
against. Distance 8 on an old 3DS is no longer the interesting number: it was already at its
ceiling, so fluid cost it 104 more evictions and not one byte of peak.

Caveat on all M2 figures: one world, one player position, on the surface. An underground spawn would
shift them.

**The whole pipeline, `--fly` over the same world.** Streaming, walking, meshing, uploading and
evicting, at 2 columns and 4 sections per frame, through three phases: stand still until the world
settles, spin on the spot, then walk in a straight line until the field's centre has moved ten
chunks. Frame counts are frames, so divide by 60 for seconds.

`./build-host/3dalpha --fly <world> <distance> 1200`. The frame count is load-bearing and was not
recorded the first time: `fly()` derives its phase boundaries from it, so a shorter run starts
turning before the world has settled and reports a different settle frame. 1,200 is the count that
makes the walk exactly the ten chunks the phase is described as.

| Configuration | Settled | Peak, no fluid | Peak, with fluid | Refused |
|---|---|---|---|---|
| o3DS, distance 6, 12 MB | frame **121** | 8.14 MB | 8.52 MB | 0 |
| distance 8, 12 MB | frame **201** | 11.13 MB | 11.15 MB | 0 |
| n3DS, distance 10, 32 MB | frame **278** | 23.81 MB | 26.11 MB | 0 |

Re-run on the grown 1,118-column world, and both columns re-measured in one session so the two
formats are comparable. The settle frames moved by a frame or two because the world did, not because
anything in the streamer changed; the peaks are the same measurement as above, in the "with fluid"
sense:

| Configuration | Settled | Peak, 4 × 12-byte | Peak, geoshader | Evictions |
|---|---|---|---|---|
| o3DS, distance 6, 12 MB | frame **123** | 8.24 MB | **1.66 MB** | 0 → 0 |
| distance 8, 12 MB | frame **191** | 11.15 MB (ceiling) | **3.33 MB** | **1,014 → 0** |
| n3DS, distance 10, 32 MB | frame **277** | 26.13 MB | **5.94 MB** | 0 → 0 |

**Settling did not move between the two formats at all** — same frame, same uploads, same columns —
which is the third independent confirmation that it is bound by the streamer's two columns a frame.
What did move is meshing work at distance 8: 1,781 section meshes over the run become 928, because
the 853 extra were re-meshes of sections the full pool had evicted.

Three things this confirms rather than assumes. The peak at distance 8 is **11.15 MB against the
11.15 MB the offline pool measurement predicted** — the real streaming path, with a real per-frame
budget and a real frustum, lands on the same ceiling. Nothing was ever refused in any configuration,
including while walking, which is the phase that moves the field's centre and hands whole columns'
worth of slots back. And **the settle frames did not move when fluid was added**: settling is bound
by the streamer's two columns a frame, not by how much geometry a column turns into.

"Settled" is nothing left to load and nothing left to mesh: **2 seconds at distance 6, 4.6 at
distance 10**, on a host with no SD card latency. The console will be slower, and this is what the
loading screen has to cover.

---

## Facts recovered from the client jar

Obfuscated names are version-specific; these are for a1.1.2_01 only.

| What | Where |
|---|---|
| `Block` | `ly.class` |
| `getRenderType` | `ly.f()` — identified by behaviour (the `()I` whose overrides spread across small integers), not by name |
| `RenderBlocks` | `bc.class` — 15 methods taking `(Block,int,int,int)` |
| `renderStandardBlockWithColorMultiplier` | `bc.b(ly,int,int,int,float,float,float)` |
| Face shade | 0.5 bottom, 1.0 top, 0.8 both Z faces, 0.6 both X faces |
| Face numbering | 0 −Y, 1 +Y, 2 −Z, 3 +Z, 4 −X, 5 +X — opposites differ in the low bit, which the walk relies on |
| `blockID` / `blockIndexInTexture` | `ly.bc` / `ly.bb` — found by running the (id, texture, material) constructor with two marker values and seeing where they land |
| `getBlockTexture` | three overloads: `ly.a(I)I`, `ly.a(II)I` (face, metadata), `ly.a(Lnm;IIII)I` (world, x, y, z, face) |
| Blocks recovered | 70, ids 1–85 with a clean gap at 21–34 and 36 (the Beta-era additions) |
| Terrain fog | `iq.class` — `glFogi(GL_FOG_MODE, GL_LINEAR)`, start `renderDistance * 0.25`, end `renderDistance`. The `-1` pass (sky and clouds) uses 0 to `renderDistance * 0.8` |
| Render-type dispatch | `bc.a(ly,int,int,int)` switches on `getRenderType` — 0 cube `k`, 1 cross `h`, 2 torch `b`, 3 fire `d`, 4 fluid `j`, 5 redstone `e`, 6 crops `i`, 7 door `o`, 8 ladder `g`, 9 rail `f`, 10 stairs `n`, 11 fence `m`, 12 lever `c`, 13 cactus `l` |
| Non-cube geometry constants | cross ±0.45 from centre; ladder 0.05; torch 0.4 tilt, 0.2 rise; fluid heights in ninths averaged over four corners, less a 0.01 lip; fence 0.375/0.4375/0.5625/0.625/0.75/0.9375; crops and rail 1/16 |
| Cross shading | unshaded — `bc.h` calls `setColorOpaque_F` once with the block's own brightness and never consults the per-face table |
| Fluid renderer | `bc.j(ly,int,int,int)`, transcribed whole into `core/mesh/fluid.cpp` |
| Torch renderer | `bc.b(ly,int,int,int)` picks the mount and `bc.a(ly,DDDDD)` — renderTorchAtAngle — builds it. Both transcribed into `core/mesh/torch.cpp` |
| Torch full-bright | `bc.b` reads the cell's brightness and then throws it away: `if (ly.t[blockID] > 0) brightness = 1.0f`. `ly.t` is the light-emission table, already the block table's `light` column |
| Fluid surface height | `jp.b(I)F` is `if (level >= 8) level = 0; return (level + 1) / 9.0f` — **ninths, not eighths**. A source block's surface sits at 1 − 1/9 = **8/9** of a block |
| Fluid corner heights | `bc.a(int,int,int,gb)`, transcribed below |
| Fluid face culling | `jp.c(nm,IIII)`: no face against its own material, none against ice, **top face always**, otherwise `!isOpaqueCube` |
| Fluid flow vector | `jp.e(nm,III)` and `jp.a(nm,IIILgb;)D` — `atan2(v.z, v.x) − π/2`, or −1000 when the horizontal components are both zero |
| Fluid side texture | `u` spans the tile, `v` anchored to the tile's **top** and starting at `(1 − h) × 16`, so the texture is cropped by the fluid's height rather than squashed into it |
| Fluid light | `jp.c(nm,III)F` overrides `getBlockBrightness` with **the brighter of a cell and the one above it**, and every face in the fluid renderer reads through it. So the underside of a lake is lit by the lake, not by the shadow it sits in |
| Render pass | `getRenderBlockPass` is `ly.g()I`. **Water and ice only** — lava shares BlockFluid with water and is told apart solely by its material, which is why this is the one column the extractor cannot read as a constant |
| `World` | `cn.class`. **`new World(File, String)` seeds itself with `new Random().nextLong()`** and there is no prompt anywhere in the game — which is why the create screen's seed box is ours rather than a1.1.2's |
| `SnowCovered` | rolled in `cn`'s constructor, in the branch taken when there is no level.dat: `this.snowCovered = this.rand.nextInt(4) == 0`, where `rand` is the World's **unseeded** `new Random()`. A one-in-four coin flip at creation, **not a function of the seed**, persisted and never rolled again. The generator reads it and puts ice at sea level − 1 across every ocean |
| `Material.isSolid` vs `blocksMovement` | `gb.a()` and `gb.c()`. **Identical for every one of a1.1.2's four material classes** — base overrides neither, air/liquid/no-collision override both to false — so the `solid` column answers both questions |

**The fluid corner-height algorithm**, from `bc.a(int,int,int,gb)`. Each of a top face's four corners
samples the four cells meeting at it:

```java
float corner(int x, int y, int z, Material self) {
    int count = 0; float total = 0;
    for (int i = 0; i < 4; i++) {
        int px = x - (i & 1), pz = z - ((i >> 1) & 1);
        if (material(px, y + 1, pz) == self) return 1.0f;   // fluid above: full height
        Material m = material(px, y, pz);
        if (m == self) {
            int level = metadata(px, y, pz);
            if (level >= 8 || level == 0) { total += heightPercent(level) * 10; count += 10; }
            total += heightPercent(level); count += 1;
        } else if (!m.isSolid()) {
            total += 1.0f; count += 1;                       // empty: pulls the corner down
        }
        // a solid neighbour contributes to neither sum
    }
    return 1.0f - total / count;
}
```

The ×10 weighting on sources and falling columns is what keeps a lake's surface flat instead of
sagging toward its edges.

Two of the three predicates it needs are already answerable from the block table: metadata now rides
in `MeshScratch`, and **"the same fluid" is `render == Fluid` with an equal `texture`** — water's 8
and 9 both carry 205, lava's 10 and 11 both carry 237. `faces[]` already distinguishes the still
tile on top and bottom from the flowing tile on the sides, exactly as `getBlockTexture(face)` does.

The third, **`Material.isSolid()`**, is now a column too. `blocks.json` carries `material` and
`solid` for all 70 blocks, `--verify` checks both against the jar, and `configure.py` compiles the
material names into a dense index. `BlockDef` did not grow: both fields fit in padding it already
had, so it is still 40 bytes.

Everything about it is derived rather than written down. The Material class comes from `Block`'s own
constructor signature `(IILgb;)V`; the field it lands in is found by running that constructor with a
marker, the same trick that names `blockID` and `blockIndexInTexture`; the 22 singletons are matched
to their classes by reading `Material.<clinit>`'s `new`/`putstatic` pairs; and solidity is resolved
per class, walking up to the base when a subclass does not override. Only *which* boolean method is
`isSolid` had to be pinned by hand in `MEMBER_MAP` -- booleans carry no spread to recognise them by
-- and it was named by its one caller, the fluid corner-height helper.

**Solidity lines up with nothing else in the table**, which is why it had to be derived rather than
inferred:

| block | render | solid | opaque | fullCube |
|---|---|---|---|---|
| glass, leaves | cube | **yes** | no | yes |
| wooden stairs, doors | stairs / door | **yes** | no | no |
| stone button | **cube** | **no** | no | no |
| snow layer | **cube** | **no** | no | no |

Any rule guessed from the other columns -- "solid means opaque", "solid means a full cube", "solid
means it renders as a cube" -- gets at least four of a1.1.2's blocks wrong. Pinned by
`tests/block_registry_test.cpp`.

Materials also give fluids a better "same kind" test than the texture-identity one noted above:
flowing and still water share material `f`, lava shares `g`, and air's index 0 is one no
constructed block takes. That is what `core/mesh/fluid.cpp` compares.

**Ice is the one material the engine has to know by name.** `jp.c` refuses a face against
`Material.ice` and there is no property behind it: ice's material is a bare `new Material()`, exactly
like stone's, so nothing in the table separates them. `configure.py` therefore emits
`mcver::kIceMaterial`, resolved from whichever block the version calls "ice", and 0 — a material no
constructed block takes — when there is none, which compiles the comparison away entirely.

**The flowing top face reads outside its own tile, and that is not a transcription error.** The
still tile is sampled by a square centred on the tile's middle, which covers it exactly. The
*flowing* tile is sampled by a square of the same size centred on the tile's bottom-right **corner**
and rotated by the flow angle, so it straddles a 2×2. a1.1.2's terrain.png is built for it: 206, 207,
222 and 223 are all water, and 238, 239, 254 and 255 are all lava, each a solid block of one fluid
around the flowing tile. Checked against the jar's image rather than assumed. The consequence for us
is that the generated atlas has to agree — `core/texture/dev_art.cpp` derives each fluid's group
of five tiles from the block table and gives them one colour, or a river would be four colours.

**A torch is not the cuboid it looks like.** `bc.a(ly,DDDDD)` emits **five quads**: four that span
the *whole block* in width and height, and one small cap. The stick is carved out of those four by
the texture's own transparency, so this render type lives or dies on the alpha test in the opaque
detail pass — the same one the crossed squares need. There is no bottom face; the sixth quad was
never written.

The lean of a wall torch is a **shear, not a rotation**. Each side quad's *bottom* edge is displaced
by (dx, dz) and its top edge is not, so a point at height t sits at `dx * (1 - t)`. The cap is
placed independently at `dx * (1 - 0.625)`, which is that same line evaluated at the stick's top —
the two agreeing is the check that the shear was transcribed the right way up, and
`tests/torch_test.cpp` pins it by interpolating one against the other.

`bc.b` picks the mount from metadata, and the pair of numbers reads backwards until you see what the
tilt moves: the base offset and the tilt point the **same** way, and their sum is exactly half a
block, so the bottom of the stick lands on the block face it hangs from.

| metadata | base offset | tilt | stick base ends at |
|---|---|---|---|
| 1 | x − 0.1, y + 0.2 | dx = −0.4 | the −X face |
| 2 | x + 0.1, y + 0.2 | dx = +0.4 | the +X face |
| 3 | z − 0.1, y + 0.2 | dz = −0.4 | the −Z face |
| 4 | z + 0.1, y + 0.2 | dz = +0.4 | the +Z face |
| anything else | none | none | standing, centred |

There is no case for 0 or 5 — both fall through to the untilted call, and so does anything else a
corrupt world might hold. The stick is 2/16 wide and its top is at **0.625** of a block; the cap
samples texels **7..9 across and 6..8 down** of the tile, which is where the flame sits.

Those texel numbers are not free parameters: the side quads map the whole tile across the whole
block, so texel column t is at t/16 of a block and texel row t is at 1 − t/16 of its height. The
stick's top at 0.625 **is** row 6. That is why `core/texture/dev_art.cpp` can carve a correct
placeholder torch from the geometry alone — and why it must. A solid placeholder tile draws a torch
as a block-sized slab, which reads as a broken emitter and is not one. The three torch tiles (80,
99, 115) are used by no other block in a1.1.2, checked, so carving them is safe.

**Light is the part that would have been wrong by a plausible amount.** A lit torch stores block
light 14, so reading its cell — which is what every other render type does — would draw every torch
in the game at 0.93 of full brightness rather than 1.0. Close enough to look fine and wrong
everywhere. The unlit redstone torch emits 0, so the override does not apply to it and it *does*
read its cell, which is what makes it look dead rather than dimmed.

| Blocks with per-face textures | 24 of the 70 |
| Faces that depend on the world | grass, chest, furnace, lit furnace, both stairs |
| Faces that depend on metadata | redstone wire, wheat, farmland, both doors, rail, both redstone torches |

`tools/extract_blocks.py --verify data/a1.1.2/blocks.json` re-checks the table against a jar at any
time. Textures come out of `tools/javap.py`, a small JVM interpreter that **runs** the block class;
everything else is still pattern-matched from the static initialiser and only recovers **constant**
returns, so `BlockStep.isOpaqueCube` (which branches on the block's own id) is corrected by hand,
excluded from `--verify`, and pinned by a test.

**`getRenderBlockPass` is the exception that had to be taught.** It is not constant in two different
ways, and both are followed rather than guessed: `BlockFluid` returns `material == water ? 1 : 0`,
and `BlockStairs` forwards the question to the block it is made of. The extractor recognises exactly
those two shapes and reports "unknown" for anything else, so a version that does something new says
so instead of silently emitting a 0 -- and a 0 here means a whole render pass quietly missing. The two passes cross-check each other on which
blocks exist and what class each one is.

Quirks the interpreter surfaced, both kept because faithful means faithful:

- `BlockFurnace` answers its top and bottom with `Block.stone.blockID`, not with a texture index. It
  looks right only because stone's id and texture are both 1.
- `BlockDoor` returns a **negative** tile index meaning "mirror this face". `blocks.json` keeps the
  sign; `configure.py` stores the magnitude and fails the build if a *cube* ever wants one, since
  the 12-byte vertex has no flip bit.

---

## Next steps, in order

### 0. The chunk worker — built, and the world it makes is the same world

**Generation runs on its own thread.** A generated column is 3.7 ms on this host and therefore tens
of milliseconds on a 268 MHz ARM11 — several whole frames each — so on the main thread it did not
slow the game down, it stopped it. Off the main thread the frame rate is untouched and a column
simply takes longer to appear.

`std::thread`, `std::mutex` and `std::condition_variable` all compile and link for the 3DS, so this
needed no `#ifdef _3DS` in core. The console lowers the worker one priority step below the main
thread, which is what makes it free: the game loop spends most of every frame blocked on VBlank, and
a strictly lower-priority thread turns that idle time into chunks. `Stats::workerRunning` says
whether it started; if it did not, generation falls back into the frame, which stutters but is not
broken.

**Two locks, never nested.** `queueLock_` guards the job slot and the finished-column queue and is
held for a pointer swap at a time; `storageLock_` guards the storage slot, which both threads reach.
Verified with ThreadSanitizer — which found one race the first time, the main thread reading the
generator's counters while the worker wrote them, and now reports none.

#### The world must not depend on the clock, and making that true was most of the work

Population order **is** the world in a1.1.2: two chunks whose passes reach the same ground come out
differently depending on which ran first. So a worker that changed the *order* of requests would
change the world, and a slower machine would produce a different one. Measured, before it was fixed:
the same path produced **36 columns of world threaded against 48 inline**, disagreeing from the
second column onwards.

Four things were needed, and each was found by a test rather than by reasoning. **The first of them
was later revised on new evidence from the jar; see §0g.**

- **A queue, not a fresh choice.** Picking "the nearest column still missing" each time the worker
  went idle made the choice depend on how far behind the generator was. The scan appends to a queue
  in spiral order and the worker consumes it one at a time.
  **Superseded twice — read §0g, then §0h.** Consuming it *strictly oldest-first* stranded a player
  who outran the generator behind ground they had already left, and the jar shows a1.1.2 generates
  nearest-to-the-player: it has no queue at all, and the renderer that asks for chunks sorts by
  distance (§0g). Once the order was "nearest to the camera", the queue was a second copy of what
  the grid already said, and it is gone (§0h): what is owed is read off the grid every frame and the
  worker is fed from an eight-entry slate rewritten from it. "Nothing is ever dropped" went with it,
  deliberately — see §0h.
- **Classification separated from loading.** Asking "does the world have this chunk" has a different
  answer before and after a sweep writes it, and the answer decides whether a column is generated.
  Every cell is now asked about once, and the grid is **three rings wider than the load radius** —
  exactly a sweep's reach — so every column the generator can write has a cell of its own to be
  asked about first. Nothing is generated until the whole grid is classified.
  **Narrowed in §0h**, on the same reach argument: nothing is generated until the *7×7 around that
  column* is classified, which is what lets a cell wait for its directory listing instead of the
  render thread waiting for a `stat`.
- **Queue membership, not a flag on the cell.** The grid wraps, so a cell is recycled by whatever
  column lands on it next; a flag on it is lost when the camera moves far enough, and a column that
  came back was queued a second time. **Moot as of §0h**: the grid *is* the membership now, and a
  recycled cell taking its coordinate out of the reckoning is the wanted behaviour rather than the
  bug it was.
- **`jobDone_` in the post condition.** The worker could finish between the drain and the post in
  the same frame, leaving the job neither pending nor active — the head was then handed out twice,
  two completions arrived, two entries came off the queue, and the second was a column that was
  never generated. A hole in the world that only appeared when the timing lined up.
  **The flag no longer exists**: the worker takes its own next job, so nothing hands one out and
  there is no window to guard. Kept here because the *shape* of the bug is worth remembering — two
  threads agreeing on whose turn it is, through flags read a frame apart. See §0g.

`a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` walks a fixed path, once on
the worker and once inline, and compares every chunk file.

**Read §0g before relying on this.** The queue now takes the column nearest the camera rather than
the oldest, so "same route, same world, on any machine" holds *while the generator keeps up* and not
once it is behind — which is why the test settles at each waypoint and why its name says so. That is
not a1.1.2's guarantee being given up: a1.1.2 keeps no queue and never falls behind, so it never has
to make the choice at all.

**What is path-dependent is the path**, and that *is* a1.1.2's own property rather than ours: the
original populates a chunk when its quadrant happens to be resident, so two clients with the same
seed and different routes have always produced different worlds. What is a pure function of the seed
is the land — height, caves, ores, the Far Lands — and that is checked against a real JVM.

#### What is not closed

Columns the generator has started and not finished are dropped at `close()` and at a render-distance
change. Next session regenerates them, and regenerating is not reloading — their neighbours' passes
run a second time into columns that were already saved. The original does not have this because `ft`
saves **every** chunk it evicts, populated or not, with `TerrainPopulated` riding along in the chunk
NBT. The fix is the same: persist the unfinished frontier with its flag. It needs a decision about
what light to store for a column that is not final yet, which is why it is written down here rather
than guessed at.

~~**Core 2 on a New 3DS.**~~ **Done, and the reasoning that deferred it was wrong in a way worth
recording.** "The worker takes whatever core the pthread shim gives it, which is the main thread's;
priority is what makes that work rather than affinity, and it is enough" — it was not enough, and the
shim is worse than that sentence assumed. devkitARM's
`__SYSCALL(thread_create)` is:

```c
Thread t = threadCreate((ThreadFunc)func, arg, stack_size, 0x3F, 0, false);
```

Core **and** priority are literals. Every `std::thread` on this toolchain lands on core 0 at `0x3F`,
the bottom of the range — so the console's start hook, which read its own priority and lowered it by
one, was reading `0x3F` and setting `0x3F`: **a no-op that had been getting credit for the design.**
The intent held by accident, and the cost was invisible because nothing reported which core anything
was on.

The ARM11 schedules core 0 strictly by priority, so the worker only ever ran in what was left of a
frame after the main thread blocked. On a console holding 30 fps that is a sliver, and it is the
whole reason generation feels stopped rather than slow: a player walking at 4.3 blocks a second
outruns it, and because the queue is strictly first-in — which it must be, since the order **is** the
world — the ground under their feet goes in behind the whole horizon.

**A New 3DS has core 2 free, and Luma already grants it.** `hbldrPatchExHeaderInfo` puts
`0xFF002109` in the synthesised 3DSX exheader, and bit 13 of that is "Access core2"; nothing has to
be installed or configured. The clock is already right — `osSetSpeedupEnable(true)` has had the
MPCore at 804 MHz with L2 on since the third launch. What was missing was only the ability to say
which core, and `std::thread` cannot say it.

`WorldStreamer::setWorkerThreadOps(spawn, join)` is that seam, and it **replaced** the start hook
rather than joining it: a hook that runs *on* the worker can never fix this, because by then the
thread exists on core 0 and a 3DS thread cannot move. `src/platform/ctr/main.cpp` supplies
`threadCreate(entry, arg, 64 KB, mainPriority, 2, false)` on a New 3DS, falling back to core 0 at
`0x3F` on an old one or if core 2 is refused. **Unmeasured on hardware** — what the worker gets is a
whole 804 MHz core instead of a fraction of a shared one, and the debug page now says which, but the
number that matters is columns per second on a console and no one has read it yet.

**What the debug page says now**, because "owed" alone could not tell slow from stalled: `owed` is
columns in range that do not exist yet, `ask` is cells whose directory listing has not arrived so
the streamer does not yet know whether the world already has them, and `GATE` means the nearest owed
column is one of the ones waiting. Owed high with `ask` at zero is a worker that cannot keep up;
`ask` high with `GATE` is the streamer waiting on the card, which is ordinary for a moment after a
crossing and a fault if it stays. `--fly … gen` prints the same. (Before §0h these were `queue` and
`refused`, which counted a queue that no longer exists.)

**Measured on the host, at distance 4 and 24 blocks a second** (`--fly <empty> 4 3000 gen`): the
world settles at frame 773 while the camera is standing still, and once it starts moving the streamer
never catches up again — 109 of 121 columns owed at frame 3000, with the queue tracking it exactly,
nothing refused and nothing gated. That is the harness deliberately outrunning the streamer, so it is
not itself a bug report; it is the shape of the failure, and it is the shape a console reproduces at
walking pace because its worker is so much slower.

### 0b. The main menu — the game starts from it now

Before this, `runGame` opened `worlds[0]` — whatever `readdir` handed back first — and a console with
no world on the card got a paragraph of `printf` explaining that worldgen was not written yet. Both
are gone. `main` brings the GPU up once and then alternates: the menu picks or makes a world,
`runGame` plays it, Exit World on the pause menu comes back out to the menu (§0d), and Quit on the
title screen is the only thing that ends the process.

**`C3D_Init` moved out of `runGame` and into the shell**, which is the change that made a menu
possible at all: it used to run *after* the no-world check, so everything before a world was chosen
had only the bottom-screen text console to talk through. The menu's citro2d context and its
top-screen render target are built and given back around each visit, so a 400×240 colour buffer and
its depth buffer are not sitting in VRAM while the atlas and the VBO pool are measured against what
is left.

**The screens.** Title (Singleplayer, Multiplayer greyed until M5, Options, Quit); world list, with
`+ Create New World` pinned above every world on the card, X to delete behind a confirmation; create,
which asks for a name and then a seed through the system keyboard; options, which is render distance
alone — 2 to `kPlayMaxDistanceOld3DS`/`New3DS`, the play limits that had been sitting in `overlay.hpp`
with nothing reading them.

**Three deliberate deviations, none of them accidents:**

- **The list is every world on the card.** a1.1.2's own screen is five fixed slots — `World1`..
  `World5`, `- empty -`, `Delete world...`, `Cancel`, in `jq.class` — and a card that holds hundreds
  of saves has no use for five.
- **The seed can be typed**, and blank rolls one. a1.1.2 never asks; `new World(File, String)` seeds
  itself with `new Random().nextLong()`. Being able to type one is the player-facing proof that the
  generator is seed-exact, which is worth a screen the original does not have. The rule for what a
  typed seed *means* is the one Minecraft itself adopted later — a decimal integer that fits an i64
  is that seed, anything else is `String.hashCode()` sign-extended — and `core/util/seed_text.cpp`
  implements it against reference values printed by a real JVM, including the UTF-8 → UTF-16 decode
  that makes a seed with an emoji in it hash the same on a console as on a PC.
- **The backdrop and the font are the pack's; the widgets are still rectangles.** We ship no Mojang
  assets, so what the menu draws with is whatever the player's pack carries. The backdrop is
  `dirt.png` tiled at 32 pixels and multiplied by 0x404040, both read out of `GuiScreen`
  (`bh.class`), baked into the texels because citro2d's image tint interpolates towards a colour
  rather than multiplying by it. The font is `default.png` with a1.1.2's own glyph widths — the
  column scan that reads the *blue* channel, the character string that starts at the space, the `§`
  colour codes, all of it out of `kd.class`; see [assets.md](assets.md#fonts). Neither is required:
  a pack with no font leaves the 3DS system font in place, and a pack with no `dirt.png` — Dev Art
  included — gets a backdrop out of the dirt tile of whatever `terrain.png` is live. `widgets.png`
  still has no consumer, so buttons are drawn rectangles.
  **Built, verified off-console, not yet seen on hardware.** What could be checked here was:
  a host mock linked against the real `lib3dalpha_core.a` built the font and the backdrop from a
  real a1.1.2 jar and rendered the title, world-list, World Settings and pause screens to PNGs at
  400×240 — the layout holds, nothing overflows, and the derived widths are Minecraft's own
  (`i` 2, `l` 3, `I` 4, space 4, everything else 6). Orientation was checked against `tex3ds`
  itself: run on a test image, its output stores the source's *top* row first in memory with the
  subtexture's `top` at v = 1, which is exactly what `GuiTexture::init` writes and what the glyph
  cells assume. What is left for a console is the `GPU_REPEAT` wrap under the single backdrop quad,
  whether citro2d's tint at blend 1.0 leaves a glyph's alpha alone, and whether 8-pixel text is
  comfortable on the panel rather than merely correct on it.

**A listing never opens a world, and that is a rule rather than an optimisation.** `Storage::open()`
writes `session.lock` and `close()` rewrites `level.dat`, so a menu built on them would re-stamp
`LastPlayed` on every world the player merely scrolled past. `Storage::peekLevel` reads level.dat and
stops; `world::listWorlds` is core, host-tested, and sorted newest-first with the name as tiebreak so
the order never depends on what `readdir` felt like returning. A test asserts the level.dat bytes are
identical before and after a listing.

**Deleting is the only destructive thing in the project**, so it refuses anything without a level.dat
in it — the caller bug that hands it the saves folder removes nothing — and it is behind a
confirmation screen. `io::FileSystem` grew `removeDirectory` for it; the recursion lives in core
where a test can run it, bounded to eight levels against a directory tree that loops back on itself.

**Creating a world rolls `SnowCovered` here**, because a1.1.2 rolls it at world creation from the
World's *unseeded* Random — see the jar table above. `Storage::create` cannot do it itself: core has
no clock, and the harnesses want it off and deterministic.

**What a new world does before it is playable.** It is empty, and the streamer fills it at the speed
of one worker thread on an ARM11, so "Create New World" used to hand over an empty sky. `runGame` now
runs its own frame loop first — the same loop, streaming the same way, not a second generation path —
until there is geometry on the screen, or nine columns are resident (a section cannot be meshed until
all eight of its column's neighbours are there), or the player presses START, or thirty seconds pass.
The console shows the streamer's own `owed`/`queue` counts while it waits, so a slow console and a
stalled one do not look the same.

**Run on hardware, and the first launch crashed on the first frame of the first world.** The menu
itself came up and worked -- the crash was at `renderer.drawFrame`, and it was not the renderer's.

**citro3d remembers the last shader program bound and dereferences that pointer on the next bind,
before it looks at the new program at all.** `C2D_Fini` frees citro2d's program, citro3d is never
told, and the game's first `bindPipeline` read `oldProg->vertexShader->dvle` out of freed heap and
data-aborted on `0x1008`. The fault lands on whoever binds next, never on the code that freed --
and it is symmetric, because `Renderer::shutdown` frees its own three pipelines and would have taken
the menu down on the way back, one lap later.

`ctr::parkShaderProgram()` is the fix: one program built from the world shader, never freed, bound
immediately before either side frees anything, so the next bind always dereferences something alive.
Both teardowns call it. The exception, written down where it will be read, is a free immediately
followed by `C3D_Fini` -- the M0 probe -- which throws the context and its pointer away together.

The dump is `crashlogs/004-loading-a-world-from-the-menu/`, **the first in this project with its ELF
archived before the fix was built**, so the call chain came out of `addr2line` in a minute instead of
being reconstructed. The fix itself is unrun: it builds, links and passes `check3dsx.py`, and the
3DSX is 528,536 bytes against 474,292 before citro2d came in. (588,548 as of §0c, which added the
PNG decoder, the zip reader and writer, the pack listing and the jar importer.)

### 0c. Texture packs — the browser, the jar importer, and Dev Art as a pack

**Options → Texture Pack** lists Dev Art pinned first and then every pack on the card, applies one,
and remembers it. **Extract from a jar** turns a player's own `minecraft.jar` into a real pack file
and then offers to delete the jar. The generated placeholder atlas is now one selectable pack among
the others rather than the only thing there is.

The whole of it added **no third-party dependency**. `docs/assets.md` had named miniz and lodepng
since before anything was built; neither is needed, because a PNG's `IDAT` stream is zlib-wrapped and
a zip's entries are raw deflate, and `core/util/compress.hpp` has offered both framings since M1 for
chunk files and Map Chunk payloads.

**Every number here was read out of a real client jar before anything was written**, and three of
them contradicted what `assets.md` said:

| Fact | Value |
|---|---|
| PNG entries | **58**, and they are the whole texture tree |
| Every PNG | bit depth **8**, **non-interlaced**; 54 are RGBA, 4 are palette |
| `terrain.png` | 256×256, 8-bit RGBA |
| Compression | mixed — **41 stored, 497 deflated** |
| General-purpose flag | **bit 3 on 497 of 538 entries** — zeroed CRC and sizes in the local header, a trailing data descriptor, and the truth only in the central directory |
| `misc/` | three files, not the loose-ends drawer the doc described |
| Root, not `misc/` | water, waterterrain, dirt, grass, rock, snow, rain, particles, fluff, shadow |
| Absent | `grasscolor.png`, `foliagecolor.png` — a **fourth** independent confirmation of the no-tinting finding |

The flag-bit-3 row is the load-bearing one and it shaped both halves. `ZipArchive` treats the central
directory as the only source of truth and opens a local header for exactly one purpose: to learn how
many bytes of name and extra field to skip before the data. `ZipBuilder` writes local headers with
bit 3 **cleared** and the real sizes filled in — copying the flag across without also copying the
descriptor produces an archive lenient readers accept and strict ones reject.

**The import is a filter, not a conversion.** A jar already *is* the pre-1.5 pack layout, so entries
are copied verbatim: compressed bytes, method, CRC and sizes straight out of the source's directory.
A 900 KB jar becomes a pack with **no inflate and no deflate call at all**. The rule is "every `.png`
not under `META-INF/`" rather than an allow-list — simpler, exactly the 58 files above, and it keeps
working on a jar from a version nobody has measured.

**Deleting the jar is gated on verification, and that is the point of the design.** `importJar`
re-opens the pack it just wrote off the card, decodes its `terrain.png` and builds the atlas from it;
only a pack that survives that round trip is reported as a success, and the confirm screen is reached
on no other basis. It is the only file this project deletes that the player did not create in it, and
the screen leads with **Keep**.

**A pack is loaded on the screen that chose it**, not in the renderer. `MenuChoice` carries the
assembled 256 KB atlas and `Renderer::Config` borrows it, so a pack that will not decode is refused
in front of the player with a reason and the pack they had stays live. The alternative was an
untextured world and no explanation.

**The atlas stays 256×256** whatever the pack's tile size, and anything larger is box-filtered down
on load. Making the edge a runtime value would put every VRAM number in
[3ds-performance.md](3ds-performance.md) back in question for a `texture_quality` option that would
have had one useful setting per model. The box filter averages **premultiplied by alpha** — a cutout
edge sits beside texels whose alpha is 0 and whose RGB is black, and a plain mean gives every HD pack
dark halos. Smaller packs are replicated, not interpolated.

**Moving Dev Art into core found a real defect that had been shipping.** The generator lived in
`platform/ctr/textures.cpp`, where nothing is sanitised; in `core/texture/dev_art.cpp` it is, and
UBSan reported a signed integer overflow on the first tile it reached — the per-texel jitter
multiplies three `int`s that overflow for almost every input. The multiply is unsigned now, which
wraps by definition and produces the same 32 bits, so **the generated art is byte-identical** to what
the console has been drawing. It is the second time the sanitizers have found something by having
code moved under them rather than by anyone looking for it.

**Two host harnesses run all of it without a console**, which is how the importer was exercised
against the real jar under ASan/UBSan before it touched a card:

```sh
./build-host/3dalpha --extract-jar <jar> <packs-dir>   # 58 png copied, 58 of 58 known names
./build-host/3dalpha --pack <zip|dir|devart>           # assemble an atlas, write atlas.pam
```

Both were run. The produced zip is accepted by `unzip -t` and by Python's `zipfile.testzip()`, all 58
entries hash identical to the jar's, and the decoded `terrain.png` is **byte-identical to a reference
decode by Pillow**. The jar's mtime was unchanged afterwards.

**45 new tests**, none of them against a checked-in fixture: PNGs and zips are built in memory by
`tests/texture_support.hpp`, so a decoder test states the exact bytes it is about to decode. All five
scanline filters are exercised deliberately rather than left to an encoder's choice, and there is a
test for an entry whose local header lies.

`io::FileSystem` grew **`fileSize`** for the jar picker: listing the jars on a card must not mean
loading them all into memory to find out how big they are. The read ceilings are set by the console's
heap rather than by what a zip could hold — the 3DS build has no exceptions, so a failed allocation
is an abort and anything that might not fit has to be refused before it is asked for.

**Run on hardware, and it found one thing.** A pack extracted from a real client jar loads, applies
and draws. The defect it exposed was not in any of this — it was in the atlas *upload*, which had
been wrong since before texture packs existed and which flat placeholder art cannot show. See the
ninth launch in §1.

### 0d. The pause menu — START stops the world instead of leaving it

START used to break the game loop outright, which meant the only way out of a world was also the
only thing the button could ever do. It now hands the frame loop to `Menu::runPause`: **Resume,
Options, Exit World**, over a world that is still open behind it.

**It is the same `Menu` object the shell already owns, and that is the whole design.** A pause menu
wants the Options screen and the Texture Pack list that already exist, and it wants them to be the
*same* ones — the alternative is a second copy of both plus a rule for reconciling what a player
changed mid-world with what the title screen is still holding. Sharing the object makes that
reconciliation nothing at all: there is one render distance, one pack list and one atlas, and both
entry points read and write them. `Screen::Options` and `Screen::TexturePacks` differ under a pause
in exactly two places, both driven by one `inGame_` flag: where B goes back to, and what the console
says. `runPause` remembers which screen the main menu was standing on and puts it back on the way
out, so Exit World returns to the world list rather than to the pause menu.

**Both rows apply to the world the player is standing in.** Render distance goes through the same
pool-then-streamer rebuild the debug page uses. Texture pack goes through a new
`Renderer::setAtlas`, which is a `C3D_Tex` swap and nothing else — a pack changes what a tile looks
like, not where it is, so every UV already in the VBO pool still points at the right tile and
nothing has to be re-meshed. The old texture is released *before* the new one is asked for, because
`Atlas::init` prefers VRAM and holding 256 KB of the old one would silently demote the new one to
linear on a console that is nearly full.

**The world genuinely stops, and it is still on screen while it does.** `runPause` owns the frame
loop, so nothing is streamed, nothing is meshed, no column is generated and the sun does not move.
The generation worker finishes at most the one column it had in flight and then blocks on its
condition variable, which is what it does when idle anyway. What the player sees behind the menu is
that stopped world — the same frame redrawn — under the original's own dim.

#### The pause menu draws into the game's frame, not into one of its own

**The first version drew a wall of dirt over the world and a scrim on top of it to say the world was
still there.** The reasoning was that the world lives in the renderer's colour buffers and the menu
had a 2D target of its own, and that putting citro2d and citro3d in one frame was the seam
`crashlogs/004` came out of. The first half was true and the conclusion was the wrong way round: it
is not interleaving that crashed, it was `C2D_Fini` freeing a shader program citro3d still pointed
at, which `parkShaderProgram` already answers.

So the ownership is inverted. `Renderer::drawFrame` takes an optional overlay callback and calls it
once per eye with the frame still open and the world already on that eye; `Menu::runPause` takes a
`PauseBackdrop` — a pointer to whoever owns the frame — and its `drawFrame` delegates to it instead
of opening one. What is left is a1.1.2's own arrangement: `GuiScreen.drawScreen` draws the dirt only
when `mc.theWorld` is null and fills the screen with a gradient from `0xC0101010` to `0xD0101010`
over the world when it is not, and both numbers are now what this draws.

Four things had to be right for it, and three of them are state nobody sets on purpose:

* **Per eye, not once.** citro2d's vertex buffer is reset at `C3D_FrameEnd`, so 2D drawn on one eye
  and not the other is a menu half the player can see, and 2D drawn on both spends the object budget
  twice. The pause menu asks for 2048 objects where the main menu asks for 1024, and falls back to
  1024 rather than refusing to open.
* **The frame-level GPU state is re-applied per eye.** `Renderer::applyWorldState` is the old head of
  `drawFrame` — texture binds, alpha test, the three combiner stages, cull/depth/blend — hoisted so
  that an overlay between two eyes cannot leave the second one drawing through citro2d's settings.
* **The depth test is turned off for the 2D pass.** citro2d draws with `GEQUAL` against depths of its
  own between 0 and 0.5; the buffer under it now holds the world's, written by a pass whose test is
  `GREATER`. A menu that respected that would be a menu with terrain through it.
* **citro2d sets neither `C3D_AlphaBlend` nor `C3D_AlphaTest`, anywhere.** Checked in
  `libcitro2d.a`, not assumed: it inherits what `C3D_Init` left — src-alpha blending, no alpha test —
  and that holds only until something else has drawn. The something else here is a renderer that
  turns blending off for its opaque pass and the alpha test on for its cutouts, so `Menu::prepare2D`
  states both. Without it the scrim is solid, which is the whole of the transparency, and a pack
  font's edges get punched out by an alpha test meant for torches.

**The menu no longer takes the top screen at all**, which is the other half of what this bought: no
render target of its own (a 400×240 colour buffer plus depth that used to be allocated while the
world's two eyes were live), no `gfxSet3D(false)`, and so nothing to give back on the way out.
`Renderer::reclaimScreen` has no caller any more and is kept for the next thing that wants the screen
to itself — the finding below is still true, and was expensive to find.

**The live render distance is passed in rather than read back.** The debug page can put a world at
distance 20, past anything this screen will offer; clamping on entry would mean that merely *opening*
the pause menu undid it. What the Options row refuses is a step **up** past the play maximum, so a
value that arrives above it can be read and lowered and nothing else.

#### The finding: citro3d holds one target per screen output, and a menu takes it

**`C3D_RenderTargetSetOutput` does not append to a list — it evicts.** `linkedTarget` is a
three-entry array (top-left, top-right, bottom), and pointing a new target at an output clears the
old one's `linked` flag and overwrites the slot. Deleting that new target then stores **NULL** into
the slot rather than restoring what it displaced. Read out of `libcitro3d.a`'s `renderqueue.o`, not
inferred:

```
C3D_RenderTargetSetOutput:  ldr r2, [r7, r8, lsl #2]   @ linkedTarget[id]
                            strb r3, [r2, #27]         @ old->linked = false
                            str  r4, [r7, r8, lsl #2]  @ linkedTarget[id] = target
C3D_RenderTargetDelete:     str  r3, [r7, r6, lsl #2]  @ linkedTarget[id] = 0
```

So `C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT)` unlinks the renderer's left eye, and `Menu::shutdown`
leaves the top screen with no target attached at all. `C3D_FrameEnd` walks the same three slots and
transfers the ones marked `used`, so the world would go on being drawn, correctly and completely,
and never reach the screen again — the top screen would hold the last frame from before the pause,
forever.

**This never bit the main menu**, which is why it was there to be found: the Renderer is constructed
and destroyed around every visit to the title screen, and `Renderer::init` re-links both eyes. A
pause menu is the first thing in the project that outlives a Renderer. `Renderer::reclaimScreen()`
re-links both eyes and resets the cached `gfxSet3D` state — the second thing the menu takes, since
`drawFrame` only calls `gfxSet3D` when the slider crosses zero and the menu sets it to false on the
way in. Resuming with the slider up would otherwise leave the second eye switched off until the
player happened to move the slider.

One more thing the menu leaves behind: **citro2d turns alpha blending on and `drawFrame` never turned
it off.** `drawEye` sets it around the translucent pass and restores `ONE, ZERO` afterwards, so
within a run of frames it was already right — but the *first* frame after anything else has drawn
inherited src-alpha blending for its opaque pass. Benign, because opaque texels have alpha 255 and
cutouts are alpha-tested away, and it used to happen once per session. With a pause menu it happens
every resume, so `drawFrame` now states it with the rest of its per-frame state.

#### Opening it costs a frame, not two seconds

**`Menu::init` used to read the card, and the pause menu paid for it.** Two listings ran on every
visit to either menu, and neither is on screen when a pause menu opens:

* `world::listWorlds` opens and gunzips a `level.dat` per world on the card, through
  `Storage::peekLevel`.
* `texture::listPacks` **reads every pack zip in the packs folder in its entirety** — `describeZip`
  takes the whole file to walk its central directory and count which of the 58 names it carries. A
  jar-derived pack is around a megabyte, and there is one per pack.

Both are now asked for by the screen that shows them: `Menu::run` reads the world list on its way in
(and `recoverConversions` with it, which is where an interrupted conversion belongs anyway), and the
pack list is already read when `Screen::TexturePacks` opens. What `init` still does is build the
atlas if there is not one yet — `ensureAtlas`, which needs the saved pack's *name* and not a listing
— and upload the pack's font and backdrop, which are decoded once per pack and kept.

So the pause path reads two small files (`3dalpha.ini` and the world's format) and allocates
citro2d's vertex buffer, and that is all.

**And it saves only what is owed.** START used to call `saveNow` unconditionally; it now asks
`WorldStreamer::dirtyColumns()` first, taken live from the cache rather than from the once-a-frame
copy in `stats()`. Pausing twice in a row, or pausing and leaving, costs one save. What is given up
is the `level.dat` refresh that rode along with it, and that is covered twice over: the autosave
timer writes it on its own interval and `close()` writes it on the way out.

#### "Saving level.." says how far along it is

`close()` takes an optional progress callback and reports as the write queue drains — columns
written against columns owed — so the still screen on the way out of a world has a number on it. The
drain is done here rather than inside `ChunkCache::close`'s blocking wait, because a call that
returns when it is finished can say nothing while it runs. Two details are load-bearing: the cache
recounts its columns in `pump()` and nowhere else, so polling `stats()` without it reports the same
number forever; and unthreaded — the host harnesses — `pump()` is also what runs the writes, so the
same loop drains and terminates there too. **Nothing owed prints "already saved"**, which is the
visible half of the rule above: a world the pause menu has just written owes nothing, and now says
so instead of sitting on a still screen.

**Superseded by 0n**, which keeps all of the above and puts the number on the *top* screen as a bar
over the frozen world, rather than on the bottom one as text under it.

**Unrun.** It builds, links, and `check3dsx.py` accepts the image; nothing here has been on hardware.

#### Deviations from `ie.class`

a1.1.2's own version of this screen is `GuiIngameMenu` — `ie.class`, title **"Game menu"**, three
buttons laid out top to bottom as **Back to game** (`h/4+24`), **Save and quit to title** (`h/4+48`),
**Options...** (`h/4+96`). The title is kept. Two things are not:

- **The order is Resume, Options, Exit World.** The destructive row goes at the far end of the list
  from where the cursor rests. A d-pad makes "one row down from the start" a place a thumb lands by
  accident, and on the original's order that row closes the world.
- **"Exit World" rather than "Save and quit to title."** The saving is not optional and never has
  been — `WorldStreamer::close` rewrites level.dat however the player left the world — so a label
  offering it as a choice would describe a decision nobody is being given. The console line under the
  screen says it happens, and the world's teardown prints the original's own `Saving level..` while
  it does.

### 0e. Chunk I/O off the render thread — the cache, the I/O thread, and the autosave interval

The symptom was a hitch whenever chunks loaded or unloaded, and the first guess was the SD card.
The card is not the limiter. **Three separate pieces of SD work ran on the render thread, and a
fourth blocked it through a lock**:

| Where | What it was | What it cost |
|---|---|---|
| `classifyCell` | one `stat` per newly exposed cell, unbudgeted, over the whole grid | 625 on the first frame at distance 8, and ~25–49 *every time the camera crossed a chunk boundary* |
| `loadColumn` | `open`+`fstat`+`read`+`close`, then a gzip inflate to ~46 KB, then `decodeChunk` | 1–2 a frame, inside the frame |
| both | took `storageLock_` | which the generation worker held while it deflated and wrote a chunk file — `open`, `write`, `fsync`, `close`, `unlink`, `rename` |
| `dropCell` | freed the column outright | and re-read it from the card if the player turned round |

The stat storm is the one that lines up exactly with the symptom: a whole row of cells reclassified
in one frame, one IPC round trip each, at every boundary.

**A faster card fixes none of it.** A chunk file is 2,917 bytes at the median, so even at a
pessimistic 5 MB/s the transfer is under a millisecond — the cost is four to six IPC round trips to
the FS sysmodule plus FAT metadata, per operation. Which is what §7 of
[3ds-performance.md](3ds-performance.md) already said: *the lever is the number of operations.*

`core/world/chunk_cache.hpp` is now the only thing in the process that touches the storage slot, and
it is three things that are the same table looked at three ways:

- **A write-back cache.** A finished column is stored and written afterwards, so nothing that
  dirties a column waits for a card. Coalescing falls out of it.
- **A read-through cache with a retention ring and a read-ahead band.** `dropCell` gives the column
  back instead of freeing it, and cells between the load radius and the classification ring are read
  ahead on the I/O thread. Crossing a boundary, or turning round, then costs no SD operation at all.
  This is **the original's own idea**: `ft` holds `new ga[1024]`, a 32×32 direct-mapped table
  indexed by `(x & 31) + (z & 31) * 32` that saves the previous occupant of a slot. 1024 columns is
  18.4 MB at our measured mean, which an Old 3DS heap does not have — hence a byte cap and LRU.
- **An existence index, lazily built.** The layout puts a chunk in `<x & 63>/<z & 63>/`, so one leaf
  directory holds only chunks spaced 64 apart and **one listing settles up to a thousand `hasChunk`
  answers for the session**. The streamer lists the ring one chunk beyond its grid whenever the
  centre moves, so the listing is there before the cell that needs it. This replaces the
  "walk 4,096 directories at open and cache to a file" design in
  [world-format.md](world-format.md), and is strictly cheaper: incremental, self-limiting to where
  the player goes, no cache file, nothing to invalidate.

**Columns are handed out as clones**, and that is what makes it simple: the cache keeps a shared
immutable column, so there is never a moment when an entry exists but its contents have been handed
to someone else, and no lock is held while 46 KB of NBT is built and deflated on the I/O thread. An
18 KB clone against an SD read plus an inflate is not a close call. An entry whose column the grid
takes and that the card already agrees with is dropped, so a resident column is not stored twice.

**The I/O thread is core 0 at one priority step below the main thread**, on both consoles. It is
almost always blocked in FS IPC, so a core of its own would be wasted — and core 2 is the generation
worker's, which genuinely needs all of it. At a lower priority on core 0 it is preempted the instant
the main thread is ready and runs in exactly the VBlank slack, so SD reads overlap with the GPU.

#### When anything is actually written — and one reversal

**Nothing reaches the card outside a save.** The saves are: the autosave interval, opening the pause
menu, and leaving the world. That is deliberately the shape a1.1.2 has, and it is a reversal — the
first version queued a generated column for writing the moment it was made. The argument for that
was real: a power-off loses finished world, and it is lost in a way regenerating cannot reproduce,
because population passes spill across chunk borders and a column regenerated against neighbours
already marked `terrainPopulated` comes back missing whatever their passes had put into it.

The conclusion was still wrong. **The original has that hazard in a worse form** — `ft`'s table is
indexed by `(x & 31, z & 31)`, so a chunk is only written when something 32 chunks away collides
with its slot, and can otherwise sit unwritten for an entire session. Eager writing made us safer
than a1.1.2 rather than correct where it was wrong, and it cost a deflate and six file operations
every time a column was finished. The autosave interval is what bounds the exposure; that is what
the interval is for.

The one thing that writes outside a save is memory pressure. A dirty column cannot be evicted — it
is the only copy of that part of the world — so over `dirtyCapBytes` (4 MB, ~220 columns) whoever
dirtied it performs a write itself. That is back-pressure paid by the generation worker, which is
the thread that outran the card. It is never the main thread, and an M3 edit path reaching it should
give up frame budget instead.

**What a save writes**, beyond the dirty columns: `level.dat` and the `session.lock` refresh,
neither of which had any trigger but `close()` — `refreshLock()` was written and never called,
despite [world-format.md](world-format.md) saying to run it on a timer. And `level.dat` now carries
**the player's position, rotation and the world clock**. Before this it was rewritten only to change
`LastPlayed`, so a world always reopened at the position it was first entered at and at the time it
was created: the fields were read at open and never written back. The clock is stored absolutely,
because `Time` is a running tick count and the day is `Time % 24000` — saving the remainder would
put the world back on day zero every time. From M3 the timer is also where player edits plug in, and
that is where it genuinely pays: coalescing many edits to one column into one deflate is something
eager writing cannot do.

#### How it is held honest

`a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` grew a third arm. The same seed and
the same camera path, filled three ways — generation inline, generation on a worker, and the
console's own configuration with the cache threaded and reading ahead — compared chunk file for
chunk file. Its cache cap is set small enough that eviction happens during the run, because an entry
evicted and re-read is where a stale answer would show. `tests/chunk_cache_test.cpp` covers the
pieces underneath with a counting `FileSystem`, because "served from memory" and "read again"
produce the same column and differ only in whether a file was opened.

**ThreadSanitizer found one race on the first run**, and it was the same shape as the one it found
in the generation worker: `ChunkCache::stats()` handed out a reference to counters the I/O thread
writes, so every locked write raced an unlocked read. It returns a locked copy now. "It is only a
counter" remains not a reason.

#### What to read on the console

A **Storage** page joins Info and Settings on SELECT+Y. The first row is the answer: `main` is
main-thread microseconds inside a storage call and **is expected to read 0.0**.

**Read the `now` column, not the total.** The cache's counters are cumulative, and the first version
of this page showed only the totals — which on hardware read `main 4000 ms` over a session that felt
perfectly smooth. Four seconds accumulated, not four seconds in a frame. Opening a world classifies
the whole grid in one pass before any directory listing has landed, so it stats a few hundred chunks
at once, and that cost then sits in the total for the rest of the session looking like a live fault.
The page now shows the delta over one sample block beside each total: the delta is the diagnosis,
the total is the history. A cumulative counter cannot answer "is it happening *now*", which is the
only question this page exists to answer.

**As of §0h that row reads 0.0 always, not merely usually.** The one reason it could be non-zero was
the `stat` a cell fell back to when its group had not been listed yet — the thing "sprinting into
unwalked ground" used to do — and that fallback is gone from the render thread. Any number above
zero on this row is now a fault to chase rather than a busy moment. After that, `hit` counts columns served
with no SD operation and `pre` how many of those the read-ahead band earned rather than retention: a
low `pre` with plenty of `hit` means the band is memory spent for nothing and can go to zero.

### 0f. Revisited chunks not drawing — a stale `published` flag

Reported from hardware after §0e: walking away from chunks and back left some columns not drawing at
all and others apparently cut off at sea level, and **changing the render distance put it right**.
That last detail is the whole diagnosis. `setMeshDistance` rebuilds the field, resets every cell's
`published` flag and republishes the grid, so whatever was wrong lived in the streamer's record of
what the renderer had — not in the columns, the cache or the meshes.

**Three defects**, all of them in how the streamer and the renderer agree on who holds what,
and none of them in the chunk cache. Any one alone reproduces part of the symptom.

**`published` was a claim about the renderer that the renderer could invalidate on its own.** The
`SectionField` wraps modulo the render distance, so a column one ring outside it shares a cell with
the column on the opposite side. When that one is published, `publishColumn` releases the outgoing
occupant's meshes and takes the cell — and nothing tells `WorldStreamer`, whose `Cell::published`
still says yes. Walk out past the render distance and back and `publishIfReady` returns early on a
flag that is now a lie: the field holds somebody else, the walk finds the cell occupied by a
different column, `isLoaded` says no, and the column is never drawn. The streamer's grid is three
rings wider than what it loads, so the band where this happens is several chunks of ordinary walking
wide.

The flag is now advisory: it is believed only when `ChunkRenderer::hasColumn` agrees, which is one
lookup.

**Nothing republished when the camera moved.** `publishIfReady` was only ever called where something
*arrived* — the nine cells around a freshly read column, or the whole grid when a generated one was
adopted. Walking back over ground that is already in memory and already on the card does neither:
nothing loads, nothing generates, so nothing publishes. That is why the symptom needed *revisiting*
rather than merely arriving, and why it was quiet in a world still being generated — there, the
adopt sweep was hiding it by republishing the grid constantly. `update()` now sweeps the spiral once
per chunk-boundary crossing, after classification so a newly exposed neighbour has a state for
`neighboursReady` to read.

**A readopted column inherited meshes that were not its own.** The renderer keeps a column's meshes
when the *same* column is published again — that is what a neighbour arriving should do — and it
decides "same" by comparing coordinates, which is all it can see. A column that left the render
distance, was dropped from the grid while it was out there, and has since been read back in has the
same coordinates and none of the same meshes: their pool slots went to other sections long ago.
Republished as "the same column" it keeps those slots and draws whatever is in them now — the
likeliest source of the sections that came back *wrong* rather than absent. `Cell` now carries
`freshlyAdopted`, and such a column is dropped from the field before it is published, so the
renderer treats it as an arrival.

Tracing that one also explained why the failing test *hung* instead of reporting a difference: when
the field holds a column the streamer no longer has, `meshSection` returns without marking anything,
so the walk queues those sections again every frame for ever. On a console that is the mesh budget
being spent on sections that can never finish — its own reason for chunks not appearing. A fresh
adopt clearing the field entry bounds it again.

**None of the three is new** — all predate the chunk cache, and the retention ring only made
revisiting ordinary enough to notice. Worth saying plainly, because the obvious suspect was the
cache and it was not the cache.

`revisiting_a_column_meshes_it_to_exactly_what_it_was` holds it: it walks out just past the render
distance and back one chunk at a time, then compares every section's mesh byte count against what it
was the first time — including the sections that legitimately mesh to nothing, since a section
wrongly marked `kEmptyMesh` is one that never comes back. **The distance is the test**: an earlier
version teleported 64 chunks away, which drops every cell from the grid and forces a clean reload,
and it passed against the broken code.

It was checked both ways rather than only against the fix, which is the only thing that makes a
regression test worth having: with the three changes reverted the same test does not finish in 300
seconds, and with them it passes in a few.

`tests/framework.cpp` grew a substring filter for that — `./build-host/3dalpha_tests revisit` runs
one case. Proving a fix by running a five-minute suite is how a fix stops being proved.

### 0g. Generation order — nearest-first, and why the FIFO queue was ours rather than Alpha's

Reported from hardware after §0f: flying around leaves a **border of chunks that never fill**, a few
appear if you wait a long time, and it comes right much later. Higher render distances let you cover
more ground before it starts.

Measured on the host, sprinting at distance 8 for 6,000 frames:

| | |
|---|---|
| columns generated | 646 |
| columns still queued | **702** |
| ...of those, already out of range | **341** |
| columns owed inside the load radius | **361** — the whole 19×19 |
| refused by the queue cap | 0 |
| classification gated | no |

Neither the cap nor the gate. The queue was **strictly FIFO**, so a player who outran the generator
was queued behind every column they had already passed: the ground under their feet sat behind 341
columns of ground they had left. That is the border; that is why waiting yields "a few"; that is why
it comes right once the backlog drains.

`pumpGeneration` now takes the queued column **nearest the camera**. Nothing is dropped — a column
that has gone out of range is still generated, just after the ones the player can see — so the set
of columns the world ends up with is still a function of the camera path alone.

> **Superseded by §0h.** The queue is gone: what is owed is read off the grid every frame, so a
> column the camera has left is not merely served last, it is not owed at all until the player comes
> back. "Nothing is dropped" no longer holds and was ours rather than a1.1.2's — the game generates
> inline for the chunk being asked for, and a chunk that never comes into range is never asked for.
> The rest of this section — why nearest-first, and the jar evidence for it — stands unchanged.

#### Why that is a correction rather than a deviation

The FIFO rule was a settled decision, so it needed evidence rather than an argument. Two things,
and the second is the one that actually settles it.

**a1.1.2 generates nearest-to-the-player.** `ft.b` (`provideChunk`) loads the chunk and, failing
that, calls the generator **inline on the game thread**; class `e` (`RenderGlobal`) runs
`Arrays.sort(worldRenderers, new RenderSorter(player))` before rebuilding them — comparator `fb`,
ordering on `WorldRenderer.a(Entity)` ascending. Nearest first.

**a1.1.2 has no queue at all**, and that is the load-bearing fact. It re-derives what it wants every
frame from where the player is *now*; there is no memory of requests made from an old position. So
the natural asynchronous form of a1.1.2 is not a FIFO queue — it is "nearest to the player,
re-evaluated continuously", which is what this now does. **The FIFO queue was our invention**, and
inventing it is what built the border.

The freeze a1.1.2 shows when you move faster than it can generate is a technical limitation of
generating inline, in the same category as lag — not a design decision to be faithful to. An
a1.1.2 that did not freeze, moving at the speed our free-fly camera moves, would behave the way this
does now.

#### What that costs, stated plainly

**Threaded no longer equals inline once a backlog exists.** With a backlog, which column is nearest
depends on where the camera got to, so a slower console makes a different world than a faster one
from the same seed and the same path.

That property was never a1.1.2's either — the hypothetical unfrozen a1.1.2 above loses it too. It
was a property of *our own asynchrony*, bought with the queue, and paid for with the border. Worth
recording that the old arrangement did not fully have it anyway: the queue **cap** refuses a column
when the queue is full and lets it be re-enqueued later, at a position that depends on how fast the
worker drained, so the guarantee already had a hole in it exactly when the queue was under pressure.

**What still holds, and should not be overstated in either direction:**

- **Terrain is seed-exact and path-independent.** Height, caves, ores, the Far Lands are a pure
  function of the seed and the chunk coordinate, verified byte-for-byte against a real JVM in
  `generate_test`. A seed typed on a PC still makes the same land.
- **Population has always been path-dependent**, in a1.1.2 as much as here: walk a different route
  and the trees fall differently. What is new is that it is now also *speed*-dependent when the
  generator is behind.
- **Nothing is lost.** Every column that is owed is still generated, just in a different order.

`a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` is the old three-way test with
its premise corrected: it settles at each waypoint, so the generator is never behind, and all three
arms must still agree chunk file for chunk file. That is the regime the game is in whenever a player
is moving at a speed a person moves at, and it still catches what the test was written for — a
column handed out twice, a sweep reaching ground the synchronous path never would.

`the_ground_under_a_stopped_camera_is_made_before_the_ground_it_left` is the new one. It outruns the
generator on purpose, stops, and counts **columns generated** — not frames or seconds, which would
measure the host — before the area around the camera is complete. Checked both ways: **1 second with
nearest-first, and over four minutes without finishing under oldest-first.**

#### Considered and rejected: clamping the camera at the frontier

Holding the camera where the world is not made yet would keep the generator from ever falling
behind, which would keep FIFO and nearest-first identical and preserve everything above. It was
rejected: a1.1.2's freeze is a limitation rather than an intent, and reproducing it as a movement
restriction would be porting the limitation instead of the game. It is also worth noting for
whoever revisits this that at a1.1.2's actual movement speeds the generator keeps up anyway — the
border is reachable because the M2 free-fly camera does 12 blocks a second, and 40 sprinting, which
is several times what a player on foot does. M3's player body may make the whole question quiet.

#### And the throughput cap it was hiding: one column per rendered frame

Found while measuring the above and fixed after it. `pumpGeneration` ran once per `update()` and
posted at most one job, so **generation was capped at one column per rendered frame** — thirty a
second at thirty frames a second — however fast the worker actually was. On a New 3DS the worker has
core 2 to itself and can beat that, and every column it could have made and did not is a frame the
frontier stays empty.

The worker takes its own next job now. `pumpGeneration` no longer dispatches; it publishes the
camera position under `queueLock_` and wakes the thread. The queue moved under that lock with it,
since both threads reach it, and finished coordinates come back through a list rather than a single
`inFlight_` slot — more than one column can finish between two frames, which is the point.
`jobPending_` and `jobDone_` are gone; the handshake they guarded no longer exists.

**The rule is unchanged: one column at a time, nearest to the camera.** The realised sequence does
change, in that finishing three columns between two frames means the camera moved less between the
picks. That is the same speed-dependence nearest-first already accepted rather than a new kind of
it, and `a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` still passes — at a
settled centre the order converges to distance order whatever the rate, which is why that test
settles at each waypoint.

One thing the change had to add: `waitForWorkerIdle` now **pauses** the queue before waiting.
Waiting for "not busy" without stopping the worker handing itself more work is a wait that never
ends on a full queue, and `setMeshDistance` calls it to grow the generator's cache — moving a table
the worker walks. `resumeGeneration()` lets it go again.

Measured on the same 6,000-frame sprint at distance 8: **619 columns generated against 522–546**
before, 43 resident columns against 16, and the settle point moved from frame 1632 to 1487. The host
worker was never the bottleneck, so that is the floor of what this buys; the console, where a frame
is 33 ms and a column is tens of milliseconds, is where the cap actually bit.

### 0h. The freeze while moving — classification off the render thread, and the queue that went with it

Reported from hardware, and the report named its own cause: **moving a lot makes the game stop for a
second or two, and the Storage page's `main` figure climbs every time it happens.** That row counts
one thing only — microseconds the main thread spent inside a storage call — so the question was not
*what* but *why so many, and why so slow*.

#### The mechanism

Three things compose into the stall, and none of them is a bug on its own.

1. **A chunk-boundary crossing exposes a whole row of cells at once.** The grid is
   `loadRadius + 3` rings, so at render distance 8 a row is 25 cells and a diagonal crossing exposes
   49. Each has to be classified before anything can be loaded into it or generated for it.
2. **A cell whose directory group has not been listed yet fell back to a `stat`** — one IPC round
   trip to the FS sysmodule, taken on the render thread. That was the documented and accepted cost,
   on the argument that classification cannot be deferred (see below) and that `warmGroup` keeps the
   listings far enough ahead that it almost never fires.
3. **That `stat` takes the storage lock, and the I/O thread holds it for the length of a chunk
   write.** An autosave flush is two hundred encode-plus-deflate-plus-fsync operations, and group
   listings sat *behind* every one of them in the job order. So during a flush the listings stop
   arriving, every newly exposed cell falls back, and every fallback queues behind a file write.

A row of cells × a whole chunk write each is the second or two. It is worst exactly when the player
is moving fastest over new ground, which is when the flush has the most to write — the three
compound rather than merely add.

#### Why deferring was said to be impossible, and why it is not

`chunk_cache.hpp` said the check "cannot be deferred and it cannot be budgeted", because
WorldStreamer's correctness argument is that a cell is classified *before* any sweep in flight could
have written it — the answer decides whether a column is generated, generation decides population
order, and population order **is** the world in a1.1.2.

That argument is right about the ordering and wrong about what enforces it. The enforcement used to
be "nothing is generated until the whole grid is classified", which needs every answer in the frame
the cell is exposed, which needs the `stat`. But a sweep reaches exactly three rings —
`ChunkGenerator::provide` sweeps terrain over `(cx-3 .. cx+2)` — so the property only ever needed to
hold *locally*: **no column is generated until the 7×7 around it is classified**
(`WorldStreamer::classifiedAround`). A cell further out than three rings cannot be one the sweep
silently fills, so it is free to wait for its listing.

So `ChunkCache::chunkPresence` answers `Present`, `Absent` or **`Unknown`**, never touches storage,
and marks the group it could not answer for as urgent. The cell stays `Empty` and is asked again
next frame. `hasChunk` — blocking — stays for the unthreaded configuration, where there is no I/O
thread to ever change the answer and "ask me later" would be a question nobody answers.

Two supporting changes, both of which turned out to be load bearing:

* **Urgent listings jump the writes.** The I/O thread's order is now reads → listings something is
  waiting on → writes → housekeeping → the speculative ring → read-ahead. A listing is one directory
  read; a write is an encode, a deflate and an fsync. Nothing is waiting on the speculative ring by
  definition, so that half stays behind the writes.
* **Listings are asked for nearest-first.** `warmAndPrefetch` walked its ring row-major, so the far
  north-west corner was listed first and the ground under the player last — which does not matter
  when the answer is only an optimisation and matters entirely when it is what the cell waits for.
  It now walks a distance-sorted spiral, `warmSpiral_`, one ring wider than the grid.

#### The ordering traps, all three found by the test that exists for them

`a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` fills the same world three
ways — generation inline, generation on a worker, and the console's configuration with the chunk
cache threaded — and compares the trees file for file. It caught every mistake in this change, and
each one is the same mistake wearing a different hat: **something was allowed to depend on which
directory listing happened to arrive first.**

1. **Skipping a blocked column.** The first version passed over a column whose neighbourhood was not
   classified and staged the next one out. The threaded-cache arm produced 36 columns against the
   other two arms' 48, missing exactly the west and north edge — the asymmetric extra ring a sweep
   populates. A blocked column now stops the walk instead: nearest-first means nearest-first whatever
   the card is doing, and it is waiting on one listing that has already been asked for urgently.
2. **Skipping a cell that has not been classified at all.** The gate only fired for cells already
   known to be `Ungenerated`; a cell still `Empty` — nobody has been told whether the world has it —
   was silently passed over, and a column further out got staged in front of it. Same fix: it stops
   the walk.
3. **"Settled" not counting the questions.** This one was worth the whole exercise. On the first
   frame of a world nothing is classified, so nothing is owed, so the streamer looked *idle* — and
   every caller that waits for the world to settle believed it. In the failing runs the test's first
   waypoint generated **nothing at all** and the harness walked on. `generationIdle()` now reports
   false while any cell is still waiting to be asked about, which is the honest reading of it and
   fixes the tests, the `--fly` settle point and the console's loading screen in one place.

Traps 2 and 3 only appeared in the `-O3` build; the sanitizer builds are slow enough that every
listing lands before the next frame and the window never opens. **Run the suite under `-O3` as well
as under the sanitizers when touching this** — three consecutive green `-O3` runs is what this
change was signed off against, not one.

#### And the queue is gone

Removing the queue was the reported ask — *"it feels unnecessary without FIFO"* — and it is right,
for a reason worth writing down: **the queue was a second copy of something the grid already
knows.** A cell inside the load radius in state `Ungenerated` *is* a column that is owed, and the
spiral is already sorted nearest-first, so the head of the spiral *is* the next job. Every property
the queue needed was a defence of the copy: a cap so a sprinting player could not grow it without
bound, a refused counter for when the cap bit, a membership set so nothing was queued twice, a stale
count for entries no longer in range.

What replaces it is a **slate**: at most eight coordinates, rewritten from the grid every frame,
which exists only because the worker takes its own next job between frames and cannot read the grid
(it is the main thread's). Nothing to cap, nothing to dedupe, nothing to go stale.

**What it gives up, stated plainly.** The queue never dropped anything: a column the camera had left
was still generated, just later, so the set of columns the world ended up with was a function of the
camera path alone. The slate does drop it — that ground is simply not made until the player comes
back. That is the more faithful of the two, and this is the one place it is worth being explicit
about why: **a1.1.2 has no queue at all.** `ft.b` loads the chunk or calls the generator inline for
the chunk being asked for, and a chunk that never comes into range is never asked for. The property
being given up was ours, not the game's.

#### Measured

Dev host, `--fly <world> 8 <frames> gen`, same seed and same path both sides.

| 1,200 frames | before | after |
|---|---|---|
| main-thread storage calls | 237 | **0** |
| ...costing | 4,429 µs | **0** |
| worst single frame | 237 calls (frame 1, world open) | **0** |

The host is the wrong machine to measure the *cost* on — a host `stat` hits the page cache, where
the console pays an IPC round trip and may queue behind an open file — so the count is the number to
read. All 237 fell in frame 1, which is the world-open classification pass; the console's version of
that same pass is the "opening a world stats a few hundred chunks" note in §0e.

| 6,000-frame sprint, three runs each | before | after |
|---|---|---|
| columns on the queue at the end | 643–698, of which 348–400 out of range | **no queue** |
| columns still owed | 290–304 | 291–292 |
| resident columns | 57–71 | 69–70 |
| settled at frame | 1,278–1,284 | 1,296–1,468 |

**Ranges, because a single run of this is noise** — the host's worker and I/O threads are not paced
by anything. Read it as: how much of the frontier gets filled did not measurably change, and the
several hundred queued coordinates, half of them ground the camera had left, are simply gone.

The settle column is not a like-for-like comparison and is here so that nobody reads the shift as a
regression: `generationIdle()` deliberately got stricter in this change — a world is not settled
while cells are still waiting to be asked about — so "after" is measuring a later moment than
"before" measured. Doing that was the third trap above.

#### The dirty cap follows the free heap

Also asked for, and it belongs with the above because it is the other half of what the I/O thread
does. A dirty column cannot be evicted — it is the only copy of that part of the world — so past the
cap the generation worker stops and writes a chunk file itself. That back-pressure is right when
memory is short and is a stall bought for nothing when megabytes are sitting free, and the fixed
4 MB had to be chosen for the worst case: longest render distance, generator cache at full size,
block data at its peak.

`ChunkCache::dirtyCapLocked` now derives it from `heapFreeBytes()` — half of what is spare, less a
2 MB reserve, clamped between the configured floor (4 MB) and ceiling (16 MB on the console). Half
rather than all, because the other half is the grid still growing and the mesher's staging buffers;
what fails when the heap runs out is an allocation, not a write. The platform seam is
`core/util/memory.hpp` and the console's answer is `__ctru_heap_size - mallinfo().uordblks`; a
platform that cannot say leaves the ceiling at 0 and gets the fixed cap, which is what the tests and
the host harnesses run on. The Storage page prints the cap beside the dirty figure, because a number
that moves is unreadable without its denominator.

#### What holds it

* `flying_over_new_ground_never_takes_the_render_thread_to_the_card` — 24 chunk-boundary crossings
  over unwalked ground with the console's threaded cache, asserting **zero** storage calls and zero
  microseconds on the calling thread. A count rather than a time: a time measures the host.
* `asking_whether_a_chunk_exists_never_opens_a_file_while_a_worker_is_running` — the `Unknown` half,
  and that the answer is the true one once the listing the question queued has landed.
* `without_a_worker_an_existence_check_answers_on_the_calling_thread` — the unthreaded path keeps the
  blocking form, or the tests and `--mesh` would wait for a listing nobody runs.
* `the_dirty_cap_follows_the_free_heap_between_its_floor_and_its_ceiling` — the four rows of the
  policy: no answer from the platform, plenty of heap, more than the ceiling, almost none.
* `a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` — unchanged, and it is what
  caught the ordering trap above.

422 tests were passing before, 426 now; ASan and TSan both clean over the streaming and cache tests.

### 0i. Generation stopping, and "Saving level.." never going away

Reported from hardware after §0h: **generation stops after a while, nothing on the debug page looks
full, and once that happens leaving the world hangs on the "Saving level.." screen.** Three separate
faults, and the reason they arrived together is that two of them are the same fault seen from either
end of a session.

**On "generation stops", be honest about which fix was the one.** There are two candidates and no
way to tell them apart from here, because neither can be reproduced without the console:

* the FIFO backlog §0h removed. A queue of several hundred columns, half of them ground the camera
  had already left, is served entirely before the frontier under the player's feet — which looks
  exactly like generation having stopped, for minutes at a time, with nothing full and nothing
  refused. The measured sprint had 695 queued and 388 of them out of range.
* the unbounded dirty set below, which ends in the allocator failing rather than in anything
  reporting a fault.

Both are real, both are fixed, and the console is what settles which one the player was looking at.
The number to read now is the Storage page's `dirty X KB of Y`: at its cap means the backstop is
working, far under it means it was the backlog.

#### 1. Nothing bounded what was owed to the card

A dirty column cannot be evicted — it is the only copy of that part of the world — so
`Config::dirtyCapBytes` is the only thing standing between a generation worker and the heap. The
enforcement was this, in `save()`:

```
while (overCap) {
    if (dirtyBytes_ <= cap || !takeWriteLocked(&job)) break;
    runJob(job);
}
```

`takeWriteLocked` takes from `writeQueue_`, and **nothing but `flush()` ever put anything on
`writeQueue_`**. So past the cap the loop found an empty queue, broke on its first iteration, and
the dirty set grew without any bound at all until the next autosave collected it. The documented
back-pressure — "over the cap, whoever dirtied the column writes one itself" — had never once run.

Measured, `MC_FLY_AUTOSAVE=600 --fly <world> 8 6000 gen`, which is the console's situation *between*
autosaves:

| | before | after |
|---|---|---|
| peak owed to the card | **11.28 MB, 741 columns** | **4.01 MB, 252 columns** |
| against a cap of | 4.00 MB | 4.00 MB |
| columns still owed at the end | 284 | 307 |

11 MB of unevictable columns is more heap than an Old 3DS has to spare — 21 MB of newlib heap
against ~4 MB of block data, a ~6.5 MB generator cache and the chunk cache's own 2 MB. What a player
sees when the allocator runs dry is generation stopping. And **nothing on screen said so**, because
the dirty figure was printed without the cap it was being measured against; §0h had already started
printing the cap beside it, which is the other half of the answer to "I don't see what would be
full".

`save()` now queues the oldest dirty columns down to half the cap and takes one of them itself, so
the thread that outran the card is the one that pays — which is what the comment above it always
claimed. The 23 extra columns still owed at the end of the run is that cost, and it is the right
trade against exhausting the heap. Note it interacts well with §0h's dynamic cap: on a console with
room to spare the ceiling rises to 16 MB and this rarely bites at all.

#### 2. A blocking flush could wait for ever

`flush(true)` waits on `drained_` for `writeQueue_.empty() && !housekeepingPending_ && no jobs
active`. The Read and List completion paths decremented the job counter **without notifying
`drained_`**. So whenever a listing or a read-ahead was the last job to finish — which is the normal
case at world exit, where hundreds of listings are queued — the predicate became true with nobody
left to say so and the waiter slept for ever. That is the "Saving level.." screen never going away.

It was always possible; §0h made it likely, because listings now run ahead of writes and are
therefore what finishes last. Every job now leaves through one function, `finishJobLocked`, which
notifies whatever the kind.

#### 3. …and it was waiting for the wrong thing anyway

Even without the hang, waiting for *every* job meant world exit waited out every queued directory
listing and every read-ahead — hundreds of SD operations whose answers nobody will ever read — in
front of a player looking at a "Saving level.." screen. A flush owes the dirty columns and
level.dat, so that is what it waits for now (`writesActive_`), and `close()` throws the speculative
queues away before flushing rather than finishing them.

#### 4. A sweep that cannot finish is now visible

Not part of the report, but found while looking for other ways generation can stop: if
`ChunkGenerator::provide` returns false — its cache could not hold the sweep — the column is simply
not made, the cell stays `Ungenerated`, and the nearest-first rule asks for the same column again
next frame, for ever. That is a frontier that never advances again, and it had no counter. It has
one now: `Stats::generationFailures`, `fail` on the Info page's generation row (it displaces `GATE`,
which clears itself as soon as a listing lands, where this does not) and a line of its own in
`--fly … gen`. **It must read zero.**

#### What holds it

* `what_is_owed_to_the_card_stays_under_its_cap` — 200 columns saved into a 128 KB cap, asserting
  the dirty set never passes twice the cap and that it was bounded by *writing* rather than by
  refusing: every column is still in the world afterwards. Fails on the unbounded version at the
  first over-cap column.
* `a_blocking_flush_returns_without_waiting_for_listings_or_read_ahead` — 400 listings and 24
  read-aheads queued, one column owed, and a watchdog on the flush. Fails on the old wait, which is
  a hang: the test is heap-allocated and leaked on failure so the suite reports it rather than
  wedging.
* `closing_a_world_with_listings_and_read_ahead_in_flight_finishes` — the same for `close()`.

429 tests pass, three consecutive `-O3` runs; ASan and TSan clean.

### 0j. Packed worlds, per-world settings, and the world options screen

Three requests that turned out to be one feature with three faces: a per-world settings file, the
`X` on the world list becoming a real screen, and an on-disk format built for this console.

**The size problem, restated with the number that was missing.** The Alpha format stores one gzipped
file per chunk column. A real card measures **16 KB clusters** and a median chunk is 2,917 bytes, so
each chunk costs a whole cluster — that much was known. What was not: **the folder format pays the
slack twice**, because the leaf directory `<x&63>/<z&63>` also costs a cluster, and below a
64×64-chunk span every chunk is alone in its own leaf. The measured 1,119-chunk world has 1,157
directories to 1,122 files. So the cost is `(chunks + leaves) × cluster`, and the saving is far
bigger than `packed-worlds.md` first guessed:

| Cluster | Folder | Packed | |
|---|---|---|---|
| 4 KB | 9,609,216 B | 4,079,616 B | 2.36× |
| **16 KB (measured card)** | **37,339,136 B** | **4,145,152 B** | **9.01×** |
| 32 KB | 74,678,272 B | 4,259,840 B | 17.53× |

Reading every chunk goes from 1,122 file opens and 1,157 directory listings to **4 and 1**. At four
to six IPC round trips per file operation that is ~9,100–13,700 down to ~25. **The wall-clock figure
is still owed**: on a Linux host with a warm cache the same walk differs by 8%, which measures the
host's cheap `open` rather than the console's expensive one.

**Sectors are 1,024 bytes, not McRegion's 4,096**, re-derived rather than borrowed. Against 4,096
the median chunk takes two sectors and wastes 53%, because a1.1.2's chunk sizes (min 1,194, median
2,917, max 5,872) straddle the sector rather than sitting inside it. Against 1,024 it wastes 17%.

**Two formats in one binary, without breaking the slot system.** The version slots bind exactly one
storage implementation per binary, statically, no vtable — and a converter needs both at once. The
resolution is `world::AnyStorage`, a tagged wrapper over `mcver::Storage` and
`format::PackedStorage`, dispatching on an enum. Not a vtable: every storage call is once per chunk,
never per block. **The blast radius was one line** — `ChunkCache` owns its storage, and
`AnyStorage::open()` reads the format off the folder, so no existing test changed.

**The bug that cost the most to find, and the only one that was a real defect.** Widening
`chunkGroupKey` from `u32` to `u64` — a region-pair key needs 34 bits — missed four places where the
key was still carried as a `u32`: two range-for loops, two `.front()` reads, and `groupRep_`'s map
key. The folder backend's key fits in 12 bits so nothing showed. On a packed world, regions (0, 0)
and (100, 0) differ only above bit 32, so the second was marked queued, never listed, and
`--fly … packed` reported *"GATED: the nearest owed columns are waiting on a listing"* and generated
nothing at all. `tests/chunk_cache_test.cpp` now streams exactly that pair; re-narrowing the types
fails it.

**What is deliberately not done**, on the user's instruction that this is a Minecraft save and not a
bank ledger: no per-chunk CRC on the gameplay read path (a payload is a gzip stream and inflate
checks gzip's own), no persisted free map (derived from the directory at open, so it cannot disagree
with it), no Compact action, no `mtime` restore. Crash-safety is the generation counter and the rule
that live data is never overwritten: a torn commit loses the last save, never the region.

**Verification is where the promise is absolute — conversion.** The headline result:

```
cp -r <real World1> /tmp/w-rt
./build-host/3dalpha --convert /tmp/w-rt pack      # 1119 chunks, 1122 files -> 5 files
./build-host/3dalpha --convert /tmp/w-rt unpack
diff -r <original> /tmp/w-rt                       # empty
```

Byte-for-byte on a real 1,119-chunk world, stray files and all. A file counts as a chunk only if its
name parses **and** it sits at the canonical path for those coordinates **and** nothing else has
claimed that slot — a `c.-d.18.dat` in the wrong leaf directory survives as a stray file rather than
being silently relocated.

**One honest deviation, and it is the original's property rather than ours.** A region listing
settles 1,024 chunks at once where a folder listing settles a scattered mod-64 set, so the streamer
reaches the frontier in a different order — and population order is part of an Alpha world; see the
note at the top of `impl/worldgen/alpha_nobiome/chunk_generator.hpp`. Flying the same seed to the
same place in both formats gave **891 identical chunks and 7 that differ by a handful of blocks**,
all where the two runs had populated different neighbours. Each format is deterministic run to run
(two identical runs are byte-identical in both), reopening a packed world regenerates nothing
(179/179 unchanged, 0 lost), and converting an existing world is byte-exact. Only newly generated
frontier can differ, exactly as it does between two a1.1.2 clients that walked different routes.

**Per-world settings.** `<world>/3dalpha.ini`, a plain `key=value` file sharing `3ds.ini`'s parser,
in the world folder in both formats. Not `level.dat` — a key no version of a1.1.2 ever wrote would
travel back to a PC copy of the world — and not `3ds.ini`, where a per-world value would be one
value for every world on the card. A real Alpha client enumerates `level.dat` and the base36 tree and
nothing else, so an unknown sibling file is inert. Absent means defaults and writes nothing, which is
every world that exists today.

**Gamemode is real now, and honest.** Spectator is the default and the only implemented mode,
because it is what the game already does: there is no player body, so movement is free flight with
no collision. Survival and Creative are listed and drawn disabled, in the same spirit as the
Multiplayer row — a button that is missing reads as an oversight, one that is greyed out reads as a
plan. The value is stored as a **word**, not an ordinal, so a file written by a later build is
readable rather than a number that silently means something else.

**The world options screen.** `X` on a world row opened `ConfirmDelete` directly; it now opens the
world's own screen — Gamemode, Format, Size, Copy, Delete, Back — and Delete keeps its confirmation
one press further in. The same screen serves the pause menu, where `inGame_` cuts it to Gamemode and
Back: copying, deleting or converting a world that is open and streaming all mean rewriting files
something else holds. It carries its own `selectedWorldName_`/`selectedWorldPath_` rather than an
index into `worlds_`, because the list is re-read on every refresh and a conversion refreshes it.

**Size is computed when that screen opens, for one world**, never per row: measuring a folder world
is a stat per chunk file across up to 4,096 leaf directories. Both numbers are shown, content and
on-disk, because the gap between them *is* the argument for packing.

**New worlds are created packed**, which reverses what `packed-worlds.md` said. Deliberately the
last step, so every path that reads a packed world was tested before one could be made by accident.

**New harness commands.** `--convert <world> pack|unpack` and `--world-info <world>`; a trailing
`packed` on `--fly` creates a packed world instead of a folder one. Opt-in, so every documented
`--fly` number keeps meaning what it did.

**Two defects a review caught after the fact, both in code this change touched.**

*The autosave was committing the wrong thing.* Housekeeping ends in
`storage_.commit()` -- the call that makes staged packed writes findable -- and it was reaching that
call before the writes it was meant to cover. Two faces of one mistake: threaded, `saveNow` asked for
housekeeping *before* it queued the writes, so the I/O thread could wake to an empty write queue and
commit early; unthreaded, `requestHousekeeping` runs the job on the calling thread and bypassed
`takeJobLocked`'s write-first priority entirely. So an autosave left its own chunks staged until the
next one, and a power cut between two of them cost two intervals rather than one. `saveNow` now
flushes first, and `requestHousekeeping`'s inline path drains the write queue itself.

*`rename` destroyed the target when it failed.* The FAT fallback -- unlink the target, retry --
fired on *any* first-failure, including a source that was not there, so a failed rename could take
the file it was asked to replace with it. `writeFileAtomic` gets away with that shape because it has
just written its temporary and knows it is there; a general-purpose rename knows nothing of the sort.
It now probes for the source before clearing anything. The test in this change already called
`rename` with a missing source and only checked that it returned false; it now checks the target
survived, and fails without the fix.

A third finding -- that seven new headers were missing and nothing builds -- was the review reading a
diff of tracked files only. The new sources are untracked, so they were invisible to it.

505 tests pass under ASan/UBSan at `-O3`; TSan clean. `--fly` under sanitizers streams a packed
world, and `--fly … packed` creates, generates, meshes and saves one. The 3DS target links and
`tools/check3dsx.py` accepts the image. **None of the UI has been seen on hardware.**

### 0k. The wall at the edge of an imported world — a pass that ran and was never recorded

Reported from hardware after §0i: **on loading a world it is invisible until you move a little, and
generation pauses, works sparsely, and stops again.** One fault, and this time it was reproducible
on the host — because two knobs were added that put the console's pacing on a dev machine, which is
what §0i said could not be done. See "Modelling the console's card" below.

**What was wrong.** A column that comes out of the save with `terrainPopulated` set has had its
population pass run already, and `ft` skips it on exactly that basis. `ChunkGenerator` skipped it
too — and skipped the bookkeeping with it. The pass writes into a 2×2, and because we light a
column once it is final rather than relighting lazily for ever, each of those four columns records
which of the four passes that reach it have run (`Entry::popMask`). A pass that ran in an *earlier
session* was never recorded, so the first generated column east of a stored one waited for a pass
that would never run again.

`provide()` then failed `lightable` for that column, for its neighbours, and for everything behind
them, every frame, for the rest of the session. The frontier never advanced — and because
`WorldStreamer::neighboursReady` will not publish a column whose neighbour is still `Ungenerated`,
the unlit seam walked back inward and the world stopped being drawn at all. **That is both halves of
the report**: generation stopping is the sweep failing, and the world being invisible is the
publishing that failure blocks.

Measured, walking east off the reference world at distance 8 with the console's pacing modelled
(`MC_FLY_FRAME_MS=33 MC_IO_LATENCY_US=4000 --fly <world> 8 3000 gen`):

| | before | after |
|---|---|---|
| failed sweeps | **4,897** | **0** |
| columns resident at the end | 208 of 361 | **361 of 361** |
| columns still owed | 153 | **0** |
| sections drawn, last 300 frames | fell to **0** | 143–282 |

Every one of the 4,897 was `lightable`, not the cache — which matters, because §0i's counter
reported all of them as "the generator's cache could not hold one". **A page that names the wrong
cause is worse than one that names none**, so the counter is two counters now
(`Stats::sweepUnlightable`, `Stats::sweepIncomplete`), the Info page's `fail` reads `failL` or
`failC`, and `--fly` prints the split.

**The fix is one line of bookkeeping and it is the faithful answer.** `ensure()`'s four triggers
keep every one of `ft`'s residency checks and drop the `isTerrainPopulated` test, which moves into
`populate()` — where it already was. A pass whose column is already populated does not run and does
mark its 2×2 as done, through `notePopulated()`. What a player sees at that seam — missing trees and
ores along the border of ground generated in another session — is a1.1.2's own seam: it skips the
same pass for the same reason and simply never notices, because nothing in it asks whether a column
has stopped changing.

Held by `generation_continues_past_the_edge_of_a_world_made_elsewhere`: a store that is half a world
(everything at x ≤ 0 stored and populated, everything east of it not), asking for eighteen columns
across the seam. Fails on the first one without the fix. The seed-exact fixtures are untouched — a
world with no stored chunks never reaches the new path — which is what says the fix changes the
border and nothing else.

#### Modelling the console's card, so this class of bug stops needing hardware

§0i had to say "there is no way to tell the two candidates apart from here, because neither can be
reproduced without the console". That is no longer true, and the reason it was is that a dev host
answers a chunk read out of the page cache in tens of microseconds while the console pays four to
six IPC round trips, and runs frames a hundred times faster than 30 Hz. Two knobs close both gaps:

| Knob | What it does |
|---|---|
| `MC_IO_LATENCY_US` | `ChunkCache::Config::opLatencyMicros` — what one storage operation costs, paid where storage is actually touched: a read, a write, a group listing, the `stat` fallback. ~4000 is a card. |
| `MC_FLY_FRAME_MS` | how long a `--fly` frame takes. 33 is the console's refresh rate, and it is what makes the per-frame budgets mean what they mean on hardware — two columns adopted per frame is 60 a second at 30 Hz and 2,000 a second at the 1 ms default. |

Both default to the old behaviour, so every documented `--fly` number stays where it was. `--fly …
gen` also prints per-frame card operations split into reads, listings and writes, which is what made
the shape of a stall legible.

**One thing they measured that is not a bug but is worth recording**: on a *folder* world the leaf
directory holds chunks 64 apart, so a group listing answers for exactly one cell in view. Measured
over 900 frames: **135 listings against 125 reads** — the group index amortises nothing there and
doubles the card operations classification costs. It is already in the numbers `--world-info`
reports (1,157 listings for 1,122 chunks, against packed's 1) and it is one more reason packed is
the format the console makes.

### 0l. A bottom screen per gamemode, and a map on the spectator one

**The Normal page is now the gamemode's, and the three debug pages stay everyone's.** That is the
split the bottom screen has been waiting for since the hotbar was promised on it: what a *maintainer*
looks at -- frame split, pool residency, storage counters, the settings row -- is the same wherever
you are, and what a *player* looks at depends entirely on what they are playing. The page is chosen
by a `switch` over `settings::Gamemode` with **no `default`**, so a mode added later does not compile
until it has been given a screen.

Survival and Creative get the screens their hotbar and block palette will be built on; both say so
and list the controls, which today are free flight in all three modes because there is still no
player body. Spectator gets a map, which is the one with anything in it.

#### The map

192 by 192 blocks at **one pixel per block**, centred on the block the player is standing in, drawn
from the columns `WorldStreamer` already holds -- **it never asks the card for anything**. Beside it,
in the sixteen character columns the map leaves free: the coordinates, the chunk, which of later
versions' 128-block map tiles this ground is on, how much has been seen, and what the last redraw
cost.

Three claims, and each of them is something to check rather than assert:

- **It lines up with the chunks.** Sixteen pixels to a chunk, because the window origin is a whole
  block. `d-pad left/right` cycles the grid overlay off / 128-block / 16-and-128, which is that
  alignment made visible. The d-pad is free to take because it is one page of one gamemode: the
  stereo tuner wants Y held and the debug pages want SELECT.
- **It lines up with the maps of later versions.** `MapData` at `scale = 0` covers the 128 blocks
  starting at `tile * 128 - 64`; that grid is what the red lines are and what the "map tile" readout
  names. Every tile edge is a chunk edge, because 64 is a multiple of 16 -- which is what lets both
  claims hold at once.
- **The player is a marker on it.** A diamond with a tick for facing. `map::drawMarker` takes a
  position, a facing and a colour and assumes nothing about there being one of them, so multiplayer
  adds callers rather than parameters.

**a1.1.2 has no maps at all**, so there was no oracle and the specification is a later version's:
`MapData.updateVisitedBlocks` for the shading, `MapColor`'s 180/220/255 for the brightnesses, the
height map for where the surface scan starts. The one thing that could not be borrowed is the colour
table -- later versions key it off `Material`, and a1.1.2's materials are coarser than the set those
colours were written for, so grass and dirt share one and a material-keyed table paints every meadow
brown. **The colour is the average of the block's top face in the loaded `terrain.png` instead**,
alpha-weighted; the store holds block ids rather than pixels, so changing the pack recolours ground
that was sampled long ago and is no longer loaded. See [map.md](map.md) for the whole derivation and
for the two divergences it is honest about.

#### What it cost -- **and the hardware number that changed the design**

The screen was built to print its own last redraw time in microseconds, because 132 µs on a 3 GHz
host said only that an ARM11 would want a millisecond or three and no amount of host testing narrows
that. **A console read 5,000 µs** -- a third of a frame, on every block the player crosses -- and all
of it was re-deriving pixels that had been derived before.

So the fix that was named and costed in this section got built: a chunk's 16 x 16 patch of pixels is
drawn once and kept beside its sample, and a redraw is a copy. Everything a pixel depends on is
either inside its chunk or knowable from its coordinates -- its own heights, the row of heights
immediately north of it, the palette and the grid style -- so nothing else can invalidate a patch,
and `MapStore` stamps them with what they were drawn with.

| | how often | host, `-O3`, real world |
|---|---|---|
| **sample** a chunk | once per chunk, ever | **1.3 µs** |
| **draw** a chunk's patch | when it is sampled, when its northern neighbour arrives, or when the palette or grid changes | **0.8 µs** |
| **copy** the 192 x 192 window | when the player crosses a block | **17.4 µs**, against the **132 µs** it replaced |

If the same host-to-console ratio holds, that puts the redraw near **700 µs** on the console that
read 5,000. The screen still prints the figure, because that is the number that found this.

Two decisions inside it are worth keeping:

- **`MapChunkSample` is stored x-major** while `ChunkColumn::heightMap` is z-major, which is what
  took the shading pass from 361 µs to 132 µs before any of the above. The walk goes down a column
  because a pixel's brightness is the height step to the block one to the north, and carrying that
  forward costs a register in this order and a screen-width buffer in the other; z-major storage made
  every pixel of that walk a 32-byte stride, a fresh cache line per pixel on a 16 KB L1. The strided
  access moved into `sampleChunk`, which pays it once per chunk ever.
- **A patch is stored x-major with z running backwards**, `patch[x * 16 + (15 - z)]`, which is what
  makes the copy a `memcpy`. The bottom screen is stored in columns from the bottom up, so going
  south steps back through the framebuffer; written in reverse, a chunk column and the sixteen
  halfwords it lands on run the same direction and the copy is 32 bytes moved. `renderMapWindow`
  takes that path when the z stride is exactly -1 and walks pixel by pixel otherwise, which is how a
  host test reading a row-major buffer checks the same picture.

What still costs the old price is a **texture-pack change or a grid toggle**: every patch in the
window is stale at once, 156 of them, about 4.7 ms -- once, at a moment the screen is already
expected to stop and think. Deliberately not budgeted: spreading it over frames would show the old
pack's colours for a while, which is worse than the hitch.

One behaviour changed with the split and is better for it. A chunk whose northern neighbour has never
been sampled now shades its first row *level* rather than against a height of zero. Zero is not a
neutral seed -- every surface is above it -- so the old continuous walk gave the first row after
every gap the bright step, and the northern edge of explored ground was a bright fringe that moved
with the player. Measured on the real world: 608 pixels changed, every one of them directly south of
unexplored ground.

`--map <world-dir> [pack] [grid]` draws the same three pieces over a **whole** world instead of a
192-pixel window and writes `map.pam`, which is how the shading and the palette were checked against
real terrain rather than against a unit test's idea of it. It also prints the surface census -- what
fraction of the world's top block is grass, water, sand -- which is the check that matters when the
colours are derived from a pack rather than declared.

### 1. Run it on a console

**This is the only thing that matters next, and none of it can be done here.** The renderer is
written, builds, links and produces the right numbers on the host, but no part of it has drawn a
pixel.

**The first launch got none of the way in.** `runGame` held `WorldStreamer` — 36,208 bytes, almost
all of it the mesher's 18³ scratch — as a local, giving it a 38,220-byte frame on a 32 KB stack. It
data-aborted on the first instruction that touched the frame, before `findWorlds()` ran. Three
things are worth keeping from it:

- The host never could have caught this. `--fly` and `--mesh` do the same thing on an 8 MB stack,
  and still do, deliberately.
- `-Werror=stack-usage=8192` is now on the **3DS build only**, for that reason. The largest frame
  left is `runGame`'s own 2,028 bytes; the next is `FogLut::build` at 1,548.
- Luma exception dumps are worth reading properly rather than eyeballing. `sp`, `far` and an empty
  stack dump named the fault; `addr2line` named the function, because a 3DSX loads at `0x00100000`
  and the ELF addresses match one-for-one. `tools/lumadump.py <dump> --elf <elf>` does both.
  **Archive the `.elf` next to the `.dmp`** — resolving against a later build answers confidently
  and wrongly, which it did here the moment the fix was compiled.

The dumps are kept in `crashlogs/`, not under `build/`, which a clean rebuild removes.

**The second launch reached the render loop and died uploading a mesh.** `runGame` →
`WorldStreamer::update` → `meshSection` → `VboPool::upload` → `memcpy`, faulting on a *write* to
`0x1F38CA00`. That is VRAM, and **the CPU cannot write VRAM** — so the pool's VRAM tier could never
have worked, and `docs/3ds-performance.md §3` now carries the reversal in full. `budget.vram` is 0
and `GpuVboAllocator::allocate` refuses the tier. The two things worth carrying forward:

- The bug was in a *seam that was one function too small*. `VboAllocator` abstracted `allocate`,
  `release` and `flush` — everything about the memory except putting bytes in it. `Atlas::init`
  had the rule right all along, in the same directory.
- The pool's totals are unaffected: 12 and 32 MB ceilings, peak resident 7.12 and 23.75 MB, all
  comfortably inside the linear heap alone. Only which allocator serves them changed.

**The third launch booted and drew the world.** Three things came back from it, and the first two
are fixed.

*The overworld was dark.* `daylightFromTime` was an invented squared sine that peaked at noon, so
the test world -- saved at `Time=412`, just after dawn -- rendered at **0.31 of full brightness**.
Alpha holds full daylight for the whole first half of the day. The real curve is now derived from
`cn.class` and lives in `core/world/daylight.{hpp,cpp}`, pinned by `tests/daylight_test.cpp`:

| what | where | shape |
|---|---|---|
| `getCelestialAngle` | `cn.c(F)F` | raw day fraction, eased a third of the way toward a cosine |
| `calculateSkylightSubtracted` | `cn.a(F)I` | an **integer 0..11** taken off stored sky light |
| `lightBrightnessTable` | `cn.<clinit>` | `float[16]`, 0.05 ambient to 1.0, **monochrome** |

Two things fell out of it. Alpha does not scale brightness smoothly -- it subtracts an integer, so
dusk steps down eleven times between ticks 12041 and 13670 and the stepping is the original's look,
not an artefact. And the table is monochrome, so the lightmap's warm block-light tint was invention
and is gone: in a1.1.2 torchlight is exactly as warm as sunlight.

*It crashed in 3D, sometimes.* **citro3d does not bounds-check its command buffer.**
`GPUCMD_AddRawCommands` memcpys into `gpuCmdBuf + offset` and advances the offset; nothing compares
it against `gpuCmdBufSize`. The draw list fills it and stereo doubles the cost, so the crash landed
in whatever linear allocation followed, intermittently, depending on where the player looked.
Sizing the buffer bigger only moves which view breaks it, so `drawPass` now checks the real
headroom before each section and calls `C3D_FrameSplit` when it is short -- safe mid-pass, because
GPU registers carry across command lists. `kCommandBufferBytes` is 4x the default on top of that,
to make splits rare rather than to make them unnecessary. **The overlay reports the split count:
anything above 0 is a frame that would have corrupted memory before this.**

*The 3D was flat.* Reported as "it goes in front of the other stuff but then still stays in the
same depth layer", which turns out to describe the arithmetic exactly. From citro3d's own
`Mtx_PerspStereoTilt`, read out of `libcitro3d.a` — the tilt puts the parallax terms in row 1,
which is the 400-pixel axis, so NDC maps to pixels at 200 per unit:

```
disparity_px(d) = 200 * (I/2) * [ 1/(F*t*a) - 1/d ]      t = tan(fovy/2), a = 400/240
```

At the old `I = 0.25` blocks, `F = 8`, full slider:

| distance | 1.6 | 4 | 8 | 16 | 32 | 64 | 160 | ∞ |
|---|---|---|---|---|---|---|---|---|
| disparity px | −12.9 | −3.6 | −0.5 | +1.1 | +1.9 | +2.3 | +2.5 | +2.7 |

**Across 32 to 160 blocks the disparity varies by 0.62 of one pixel.** The far world was a single
flat card 2.5 px behind the screen; only things nearer than the 8-block focal plane separated at
all, which is the "in front of the other stuff" half of the report. For scale, the 3DS convention
for infinity is roughly 1/30 of screen width, about 13 px.

The tunable is now **on-screen disparity in pixels** rather than an interocular distance in blocks,
because pixels are what the eye judges and blocks are not — the same 0.25 blocks is dramatic at a
focal distance of 1 and invisible at 100. The separation is derived from it. Default 10 px, which
is a starting point and not an answer: **hold Y in game and use the d-pad** — left/right for
strength, up/down for the focal plane — with both values on the overlay. Whatever settles, write it
into `kInfinityDisparityPixels` / `kFocalBlocks`. Note the near field is inherently harsh in first
person: at 10 px infinity the ground at 1.6 blocks sits at −48 px, and no choice of the two numbers
avoids that, because the 1/d term always wins near the camera.

*Light was inverted.* Caves and sea floors lit like open sky, open sky lit like a cave. The
lightmap wrote sky light at memory row `sky` and the shader sampled it at `v = (sky+0.5)/16`, which
reads row `15 - sky`.

**`v = 0` samples the last row in memory, not the first.** Three independent confirmations, which is
worth recording because the rule is invisible until something is upside down:

- `Atlas::init` puts tile 0 at source row 0 and flipped it on upload (with
  `GX_TRANSFER_FLIP_VERT(1)` at the time; on the CPU since the ninth launch), so tile 0 lands in the
  *last* rows — and `mesher.cpp` gives tile 0 a `v` of ~0. The atlas is right on hardware, so v=0
  must read the last row.
- `probe.cpp`'s `buildAtlas` does the flip by hand and says why: "texture origin is bottom-left".
  That code is M0, validated on hardware.
- The console, twice: the atlas is right way up and the lightmap was not.

The bug started in the M0 probe, whose *lightmap* omits the flip its own *atlas* two functions above
performs — M0 was measuring fill rate and never asked which way up the light was. The renderer
inherited it. Both are fixed; no M0 number moves, because fill rate does not care what the texels
say. `tiledOffsetFlipped` carries the derivation so the next CPU-written texture does not
rediscover it — in `core/texture/tiled.hpp` since the ninth launch, and it is the atlas's path too
now, not just the lightmap's.

Still unverified: the **block-light axis**. It is `u`, and a horizontal flip would be a different
bug. **The strongest check in this world is lava in the dark**, not a torch: every one of the 24
torches sits at sky light 9–15, where the sky term masks the block term, whereas 461 chunks hold
lava that is exposed to air at **sky light 0 and block light 15**. The nearest to the player's
saved position is **(256, 26, 221)**, about 44 blocks straight down. It must glow; black means `u`
is flipped.

**Torches now make this directly testable**, which is half the reason they were done before the
other non-cube types. A torch draws at block light 15 by construction — the full-bright override,
not the cell — so a torch that renders *dark* in a cave is the `u` axis flipped and nothing else.
An unlit redstone torch next to it is the control: it reads its cell, so it is meant to be dark.

**The fourth launch reported three things, and all three are now fixed or explained.** Fog too
thick, shimmer at the left and right of the screen, and a 17.5 ms frame against a 0.8 ms GPU.

*The fog was far too thick, and worse the further you could see.* The PICA's fog LUT is indexed by
**window depth**, which is 1/d: with near 0.2 and far 176 the entire 40-to-160-block ramp fell
inside LUT entries 0 and 1, so the hardware interpolated a single straight line across it -- 50 %
fogged at 40 blocks where a1.1.2 wants 0 %, 83 % at 80 where it wants 33 %. Raising the render
distance pushes the ramp further into 1/d's flat tail, which is why it read as "too thick for this
render distance". Fog is now a line in `w` in both vertex shaders, carried to the fragment in the
vertex colour's alpha and applied by an `INTERPOLATE` combiner stage; no extra texture fetch, and
it scales with the render distance by construction. Full derivation and the before/after table in
`docs/3ds-performance.md §5`; `FogLut` is deleted.

*The frame is 17.5 ms because the top screen is 59.83 Hz.* `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)`
spins on `gspWaitForAnyEvent` until citro3d's VBlank callback bumps its frame counter -- read out
of `renderqueue.o`, not assumed -- so **16.7 ms is a floor no amount of optimisation moves**, and
17.5 is that floor with about one frame in twenty missed. The comment that SYNCDRAW is what
serialises CPU and GPU was wrong: the `gxCmdQueueWait` immediately after it does that, with or
without the flag, so the pool's VBO memory is safe either way and dropping SYNCDRAW would only
render frames nobody sees.

The overlay now reports the split, because "frame 17.5, GPU 0.8" cannot distinguish the two cases
on its own: **`CPU busy` against `vsync`**, and under it `walk / stream / submit`. If busy is well
under 16.7 the console is simply at its refresh rate; if vsync is near zero the CPU is the thing
missing frames and the three phases say which one. The `Mtx_Multiply` per section per pass per eye
is gone in passing -- the model matrix is a pure translation, so `mvp` is `vp` with one column
replaced, 12 multiplies instead of 64 and exactly equal, about 1,800 times a stereo frame.

*Shimmer.* Two defects found, both by arithmetic rather than by looking:

- **A 16-bit depth buffer cannot describe this world.** Resolution is
  `d^2 * (far-near) / (near*far*65536)`, which is 0.76 blocks at 100 away and 1.95 at 160, so past
  ~110 blocks two surfaces a whole block apart share a depth value and rounding decides which one
  is drawn. It changes as the camera moves and differs between the eyes, which is what makes it
  shimmer rather than sit still. Both eyes are now `GPU_RB_DEPTH24_STENCIL8`: 256x the resolution,
  375 KB of the ~5 MB of free VRAM. `docs/3ds-performance.md §6` carries the reversal.
- **The cull frustum's `* 1.08` was a guess that fails as the 3D is turned up.** Derived from
  `Mtx_PerspStereoTilt` the widening a stereo eye needs is `|iod|/(2*focal)` on the horizontal
  half-extent, which is 1.025 at the default 10 px of disparity and passes 1.08 around 25 px. Past
  that, sections the outer eye can see get culled, so they appear in one eye and not the other --
  at the left and right edges of the screen specifically. Now computed from the live stereo
  parameters. `docs/3ds-performance.md §10`.

Not fixed, because it is not a defect: at 10 px of infinity disparity the two eyes' images of the
far world are offset by 10 px, so a strip that wide at each edge of the screen is seen by one eye
only. Every stereo renderer has this; it shrinks with the disparity, which is on Y + d-pad.

**The fifth launch had torches, and they drew as four black squares each.** Predicted, in those
words, by the torch entry in the check list below — which is the only reason it took one look
rather than an evening.

**The alpha test was only ever on in wireframe.** `C3D_AlphaTest(wireframe_, ...)`, on the
reasoning recorded in the comment that "in the ordinary case the test is off entirely so nothing
else has to care what a texture's alpha means". Nothing did care, until a render type arrived whose
entire shape is carved out of transparency: a torch is four *full-block* quads, so with the test off
every transparent texel — `rgba(0,0,0,0)` — was written as opaque black. Four quads, four black
squares.

The original's rule was never a judgement call. `iq.class` does exactly this once at startup and
never touches either again:

```
glEnable(GL_ALPHA_TEST);          // 3008
glAlphaFunc(GL_GREATER, 0.1f);    // 516
```

So it is on for **every pass in the game**, and `glAlphaFunc` is called from nowhere else in the
jar. 0.1 of 255 is 25.5 and GL_GREATER passes what is strictly above the reference, so GL admits
alpha ≥ 26 — and `GPU_GREATER` against **25** admits exactly that set. Written as 25 and not as a
rounded 26 for that reason: 26 would discard a texel the original keeps.

Two things worth carrying forward. The wireframe atlas is strictly 255 or 0, so it never needed a
threshold of its own and the two cases collapse into one; what wireframe still changes is the
*combiner*. And this was latent for more than the torch — **glass and leaves are `cube` render
type with cutout textures**, so the same bug was waiting for the first texture pack with real alpha
in it, in the pass that carries 97 % of the world's geometry.

This raised a question — the PICA disables early-Z while the alpha test is on, and the cube pass
carrying 97 % of the world is where that would show — which **the sixth launch answered: nothing
measurable.** See §2. The toggle that answered it has been removed from the settings page; the test
is on unconditionally, in every pass, set once per frame.

**Controls and the bottom screen**, added after that launch and not yet run on hardware:

| | |
|---|---|
| circle pad | move |
| C-stick | look — through `ir:rst`, not `hid`, so a Circle Pad Pro on an **old** 3DS gets it too |
| touch drag | look. **Yaw was inverted**: the view turned the opposite way from the finger. Both axes now follow the mouse convention, drag right look right, and the reason it is `+=` is that `Camera::look` sends yaw 0 to +Z and positive yaw toward −X — south turning to west, which is right |
| L / R | down / up |
| X | sprint, unless SELECT is held |
| Y + d-pad | tune the 3D, unless SELECT is held |
| SELECT + Y / X | cycle the bottom screen forward / back |

The bottom screen is three pages: the player's screen (empty of diagnostics on purpose — it is
where the hotbar goes at M3), the debug readout, and a **settings page**. Three settings, all live —
one of them a measurement instrument rather than a setting anyone should ship with, which is why the
page explains itself on screen. A fourth, the cube alpha test, was there and is gone; see below:

- **Render distance**, 2 to **24**, which is the hardware's ceiling and deliberately not the
  player's. The 8 and 12 that used to bound this page are a judgement about where a 3DS stops
  giving anything back for the cost; they are a *play* limit and belong to the M3 settings menu,
  where they now live as `kPlayMaxDistanceOld3DS` / `kPlayMaxDistanceNew3DS`. They have no business
  stopping a maintainer from looking at distance 20 to see what breaks.

  What 24 is: the streamer holds (2d+3)² columns at a measured mean of 18,013 bytes, against a
  40 MB newlib heap that also carries the mesher's scratch and the storage buffers, so somewhere
  around **d = 20** the heap runs out. It does not run out gracefully — `Section` allocates its
  palette and index arrays through ordinary `new` and the build has no exceptions, so an allocation
  failure is `std::terminate`, not a column that fails to load. 24 is past the estimate on purpose,
  because a sparse world holds far less than the mean and a maintainer asking for 24 should get it
  and find out; what the bound prevents is only the case where the number is so far past the heap
  that the console dies before drawing anything. Making it genuinely unlimited means threading
  nothrow through the whole section decode, which is worth doing when something needs it and is not
  worth doing for a debug page.

  Changing it rebuilds the
  field, the size-class table and the VBO pool, and rebuilds the streamer's grid — whose width is
  part of how a column is addressed, so a new radius genuinely means a new grid. **Columns already
  in memory are moved across rather than reloaded**, because re-reading them costs seconds on a
  console and this is a setting a player is expected to try both ways. The order is load-bearing
  and is why `--fly` grew a switch-to argument: the renderer has to be rebuilt first, since the
  streamer republishes every column it kept into whatever field it finds. Exercised under ASan in
  both directions, 10→4 and 4→10, with nothing refused and 121 columns kept each way.
- **Wireframe.** The PICA has no line primitive — triangles, strip, fan, geometry-shader, and no
  polygon mode — so this is not a wireframe in the usual sense. Every quad in this renderer maps to
  exactly one atlas tile, so a second atlas whose every tile is a one-texel outline over
  transparency draws each quad's border and nothing else, and the alpha test discards the interior
  before it writes depth so the mesh behind shows through. Unlit and unfogged, which is the reason
  the lightmap and the fog are separate combiner stages rather than one. 256 KB of linear memory,
  built on first use.
- **Cube format**, 4-vertex or geoshader — **the M2 gate's open question**, and the reason to reach
  for this page at all right now. Like the render distance it throws the pool away and re-meshes
  everything, so it is not an instant A/B: give the world a second to settle before reading the
  numbers back. Exercised under ASan by `--fly … flip`, which changes it halfway over a real world:
  361 columns kept, nothing refused, nothing re-read from the SD card.
- ~~**Cube alpha test.**~~ **Removed.** It was a measurement instrument, the question it was added
  for is answered — turning it off changes GPU draw by nothing measurable — and a switch that can
  only produce the same number twice is not worth a row on a 30-row page or a branch in `drawEye`.
  The alpha test is now set once per frame and never touched again. If something later claims to be
  fragment-bound, this is a five-line change to put back.

`--fly <world> [distance] [frames] [switch-to] [quads|flip]` reproduces both of the page's rebuilds.
The optional arguments all default to off, so every documented invocation still reports the numbers
it always did — checked: distance 10 over 1,200 frames still settles at frame 277.

**The page was scrolling, and the reason it looked like a lag symptom is the reason it took a while
to see.** On a struggling console the bottom screen started showing the frame-time and GPU-time lines
several times over, each copy one sample block older than the one below it. That is the page's own
history: `newRow()` in libctru's console scrolls the whole window when the cursor passes row 30, and
the body was walking off the bottom.

Nothing was printing extra lines. **Lines were printing extra rows.** The console is 40 columns and
wraps, the overlay ended every line with `\n` and trusted that to mean "one row", and six of the Info
page's lines were 41 to 44 characters wide *with ordinary numbers in them* — `columns` was 44,
`walked` and `queued` 42, `pool` and `xyz` 41. Each of those had always been silently taking two
rows. The body fitted anyway, at exactly rows 4..30, until the generation rows appeared and pushed it
over; and it got worse the busier the console was, because that is when the numbers are widest and
`uploads` and `xyz` grow digits too. The correlation with lag was real and it was not causal.

The fix is that **the overlay no longer writes a newline at all.** `row(line, fmt, ...)` places text
at an absolute row and clips it to the console's width, so the cursor cannot advance a row on its own
and `newRow()` cannot be reached from the drawing path. A line that outgrows the screen now loses its
tail where you can see it, and a page that asks for a row outside its body does not get one. Every
line was re-measured at its *widest* values rather than its typical ones — a Far Lands coordinate is
eight digits, a count of cells in the grid is four — and `xyz` became two rows so that the one place
those numbers matter is not the one place they get clipped. Info is 23 rows of the 24 available and
Settings still spends all 24.

Clipping counts drawn characters, not bytes: the settings cursor is `"\x1b[33m> \x1b[0m"`, nine bytes
and two columns, and a byte-counted clip would cut that row seven characters short. That is fiddly
enough to be worth testing, so it lives in `core/util/console_text.cpp` with ten cases on it rather
than in the platform layer where nothing could reach it.

**The seventh launch did not launch: black screen, red text, before a pixel.** The dump on it was
`loader` — a system module — faulting on core 1, not 3DAlpha, which appears as `3dsx_app`. Since the
game never started, nothing about its *runtime* can be the cause; only the image handed to the
loader can, and that image had **doubled to 774 KB**.

The cause was one symbol. `MathHelper::table_` is a1.1.2's 65,536-entry sine table, 256 KB, and it
was a `.bss` array. It had been in the tree for weeks and never in the binary: nothing on the console
called worldgen, so `-Wl,--gc-sections` dropped the lot. **Wiring the generator into `WorldStreamer`
linked it in**, and `.bss` went from 56 KB to 319 KB in one step. It is on the heap now — the table
is built at runtime either way, so the image was paying for a quarter of a megabyte of zeroes it
could have asked for later — and `.bss` is back to 56,392 bytes against the 56,144 it was before
worldgen existed. Image 774 KB → **497 KB**.

Two things to keep from it. **`--gc-sections` means a subsystem's cost does not appear until
something calls it**, so the binary can double on a commit that adds no code. And `tools/lumadump.py`
now prints the **process name first** and refuses to decode `dfsr`/`far` on an exception that does
not set them: it had reported this undefined-instruction trap as "permission fault on write to
0xDFBFFFFF", which is a different bug with a different cause. See `crashlogs/003-loader-rejected-the-3dsx/`.

~~**Unconfirmed:** that the size was the cause.~~ **The size was not the cause, and the dump says so
outright.** The eighth launch failed identically at 497 KB, which is the test the paragraph above
asked for, and reading the dump properly the second time answers it:

- `pc` is `0xE7F000F0`, `udf #0` — GCC's `__builtin_trap()`. Luma's `panic()` in
  `sysmodules/loader/source/util.h` is that one instruction and nothing else, and it is `noinline`,
  so **this is a deliberate abort with its argument still in `r0`**, not a wild jump.
- `pc` and `lr` are above `0x14000000` because Luma links its own sysmodules there
  (`-Wl,--section-start,.text=0x14000000`). No ELF of ours will ever resolve them; the reader now
  says so instead of shrugging.
- **`r0 = 0xFFFFFFFF`.** Exactly one thing in that whole sysmodule hands `panic` a bare `-1`:
  `hbldrLoadProcess` returning `(Result)-1` because `Ldr_Get3dsxSize` returned false. `r6`/`r7`
  hold `0x000400000D921E00`, hb:ldr's 3DSX title id, so it is the homebrew path; the title's
  codeset info (`0x00100000`, `0x20` pages) is on the stack two frames up, which is the "34-page
  module" the first reading mistook for a size limit.

`Ldr_Get3dsxSize` fails in three ways, and only three: it could not read 32 bytes of header, the
magic was not `3DSX`, or a segment size overflowed when page-rounded. **The file loader opened was
empty, truncated, or not a 3DSX.** Nothing about what we compiled is implicated. Our code never ran,
and it was never going to: `LoadProcess` traps *before* `svcCreateProcess`.

**`make run` is not exempt from this — it is the likeliest way in.** 3dslink does not hand the
image to the console's memory; hbmenu's netloader writes it to **`sdmc:/3ds/<name>.3dsx`** and then
passes hb:ldr that *path*, so loader reads it back off the card like any other file. The netloader
checks `fwrite`, and **does not check `fclose`** — a card with no room left fails on the flush, and
nothing on either end says so. The result is a silently short `/3ds/3DAlpha-a1.1.2.3dsx` that gets
launched anyway. So: **check free space on the card, and check that file's length**, before
suspecting anything in the build.

The trap is why this is so hard to see from the couch. Luma has no channel to tell the Homebrew
Launcher "I will not load that file", so **a rejected image and a crashing one look the same**:
black screen, red text, a dump belonging to `loader`.

`tools/check3dsx.py` now runs Luma's acceptance checks — `Ldr_Get3dsxSize`'s three, then every read
`Ldr_CodesetFrom3dsx` performs, against the file's actual length — so the question "is the image bad
or is the copy on the card bad?" is answerable without a console. **Point it at a copy taken from the
SD card, not at `build/`**; the two being different is the entire finding. The image in `build/` at
474,292 bytes passes.

The 256 KB sine table was still worth moving to the heap. It just was not this.

**The ninth launch had a real texture pack in it, and found a one-row bug that four launches of
placeholder art had hidden.** Grass, dirt, stone and the flowers drew with a line of gray across the
top of every face. It sat *on* the texture rather than shifting it, it repeated per block, and it
did not move with the camera, so it was texture content and not geometry.

Three observations pinned it to one row before any code was read:

- **Only tile-row-0 blocks showed it.** Grass top (0), stone (1), dirt (2), grass side (3), rose
  (12), dandelion (13), sapling (15) — every affected tile is in atlas tile row 0. Sand, gravel,
  log, leaves and cobblestone were clean, which rules out a per-tile edge and leaves the atlas's own
  outer edge.
- **It did not change with time of day**, which rules out the lightmap's texels being read — the
  first suspicion, and the wrong one.
- **It was on the build that already had `kUvInset`.** The inset had done its job: rows 1–15 no
  longer bleed into the tile above. Only the outer edge was left.

`Atlas::init` uploaded with `GX_TRANSFER_FLIP_VERT(1)`, so **sampled `v = 0` is the last row in
memory** — the very end of the 256 KB buffer — and that row carries `terrain.png`'s row 0, which is
the top texel row of every tile in atlas row 0 and of nothing else. With the inset in place the
fragment samples texel row 0 squarely. The sample was right; **the row was wrong**.

**Which half of that upload lost the row was never isolated, and the fix did not depend on knowing.**
The transfer was doing a tiling, a vertical flip and a format conversion in one step, and the
destination was VRAM; the change below removes all four variables rather than choosing between them,
and it worked. So the question is *closed but unanswered*, which is worth saying plainly rather than
letting the fix read as an explanation. If anything like it returns, the untested variable that is
left is the VRAM destination, and the one-line probe is to skip `C3D_TexInitVRAM` and let the atlas
fall back to linear — the M0 probe's exact configuration.

Nothing upstream could be blamed. `scaleToAtlas` is a straight `memcpy` for a 256×256 pack, and
`--pack` on the jar-extracted pack writes an `atlas.pam` whose row 0 is grass green,
`(116,180,74,255)` at x=0. The corruption entered at upload, in the one step of the whole pipeline
that had never been checked against its input: a display transfer doing a flip *and* a tiling *and*
a format conversion, into VRAM, on hardware, with no read-back.

**So the flip and the tiling moved to the CPU**, through the same `tiledOffsetFlipped` the lightmap
has always used and the M0 probe's own atlas used before it. **Confirmed fixed on the console**: the
line is gone. `Atlas::init` was already walking all
65,536 texels once to swap R,G,B,A into the A,B,G,R word `GPU_RGBA8` wants, so the tiling is folded
into that pass and costs a scattered write instead of a sequential one, once per pack selection.
What is left of the upload is `C3D_TexUpload` moving bytes that are already final — read out of
`libcitro3d.a`, it range-checks the destination against `[0x1F000000, +0x600000)` and routes VRAM
through `C3D_SyncTextureCopy`, falling back to `memcpy` otherwise, which is exactly the VRAM rule
`gpu_memory.cpp` already records. It does not flush the source, so `Atlas::init` still does.

Two things to keep from it:

- **A placeholder that is flat colour cannot show a texture bug.** Dev Art's tiles are one colour
  plus jitter, and one wrong row in a flat tile is invisible. The generated art earns its keep by
  making a wrong texture *index* obvious; it says nothing at all about whether the texels arrived.
  Every future upload path needs a real pack in front of it before it is called run.
- **The tiling map is testable and now is tested.** `tiledOffset` and `tiledOffsetFlipped` moved out
  of `platform/ctr/textures.cpp` — where no host test can reach them and nothing is sanitised — into
  `core/texture/tiled.hpp`, with `tests/tiled_test.cpp` on them. The check that would have caught
  this without a console is the covering one: 65,536 source texels must produce 65,536 **distinct**
  offsets, all inside the buffer. A map that drops a row, doubles one, or walks off the end fails it.
  This is the third time moving code into `core/` has found or would have found something.

What has to be checked next, roughly in the order it will break:

- ~~**The atlas swizzle.**~~ **It was wrong, and this entry called it.** `Atlas::init` used
  `C3D_SyncDisplayTransfer` with `FLIP_VERT(1)` to tile a linear image, and the texture came out
  with one row wrong. It is the CPU Morton path now — the fallback this entry named, the one M0
  validated on hardware. See the ninth launch above. The prediction was right about everything except the size of the failure:
  a scrambled atlas would have been obvious at a glance, and **one row was not**, which is why it
  survived every launch until a real texture pack was loaded.
- ~~**The fog LUT.**~~ Gone — see the fourth launch above. What replaced it needs checking instead:
  no fog at all up to a quarter of the render distance, and terrain reaching the sky colour exactly
  at the render distance rather than being visibly clipped by the far plane one ring further out.
  The second thing to look at is **water's alpha**, which must not change with distance: the
  combiner chain deliberately routes alpha past all three stages so that fog cannot multiply it.
- **Stereo.** ~~`kMaxInterocular` and `kFocalBlocks`~~ — done, and they were wrong. See below.
- **Whether geometry appears at all**, which is the winding, the depth test sense (`GPU_GREATER`
  against a depth cleared to 0) and the culling mode. All three match the M0 probe, which did render
  a correct cube on hardware.
- **Fluid surfaces**, which have never been seen. Four things fail visibly and differently: a
  water surface at the wrong height (the corner algorithm), a surface at the right height with the
  wrong texture on it (the flow rotation, or a placeholder atlas whose fluid group disagrees),
  faces where there should be none (the culling rules), and water that is opaque or that vanishes
  behind other water (the blend state and the back-to-front order). The test world has lakes at the
  surface, so this is the first thing visible on stepping outside. Every rule below the blend state
  is pinned by `tests/fluid_test.cpp` on the host, so a discrepancy on hardware means the
  *renderer* is wrong, not the mesher.
- **Torches.** The first of the four predicted failures happened on the fifth launch and is fixed:
  a **block-sized slab** — black, in fact — was the alpha test, above. **The mount sign is now
  settled too, and without hardware**: the world has 24 torches and in all 24 the block the mount
  predicts is solid, several of them discriminating because the opposite face is air and a mirrored
  convention would hang the torch on nothing. `tools/`-side scan, cross-checked against
  `torchMount()` in `tests/torch_test.cpp`. Two left: a torch **lying flat or leaning the wrong
  way** is the shear inverted, and a torch **dark in a cave** is the lightmap's `u` axis, above.
- **The geometry-shader cube pass**, which has never run at all — a third program, a second DVLE, a
  `GPU_GEOMETRY_PRIM` draw with no index buffer, and an 18-vec4 uniform table. It fails in ways that
  are each distinct at a glance, which is the reason for listing them:

  | what you see | what it is |
  |---|---|
  | nothing at all in the cube pass | the geometry shader is not emitting: the gsh input stride, or `GPU_GEOMETRY_PRIM` |
  | the world inside out in patches | the strip's `inv prim`, i.e. the second triangle's winding |
  | every face the same brightness | `faceBasis[].w` shade not reaching the shader — check the uniform table upload survives the bind |
  | textures scrambled per face | `uvSign`, or `tileX`/`tileY` swapped |
  | one corner of every quad wrong | `a0` read too soon after `mova` — the one hardware question §2 could not settle |
  | garbage colour on three of four vertices | output registers do *not* persist across `emit`, which the shader currently assumes they might not and writes per vertex anyway |

  The last two are the two unknowns worth watching for specifically. Everything else about the
  format — corners, winding, UVs, shade, light, byte layout — is pinned on the host by
  `tests/quad_format_test.cpp`, which expands every quad of a section through the same basis the
  shader uses and compares it vertex for vertex against the 12-byte mesher.
- **The translucent pass itself**, which is three GPU state changes that have never run. Blending
  on, depth *writes* off with the depth *test* still on, and back-face culling back on — unlike the
  opaque detail pass, because a fluid face is single-sided and drawing its back too would blend the
  same surface in twice. Each fails differently: no blending is opaque water; depth writes left on
  hides the further of two water surfaces; culling left off makes every lake read twice as deep.
  The placeholder atlas gives water alpha 150 so all of this is judgeable at a glance.
- **The spectator map (§0l), which is the first thing this project has drawn with the CPU.** Four
  things it could get wrong that no host test reaches, in the order they would be obvious:
  *nothing on the right-hand two thirds of the bottom screen* is `gfxGetFramebuffer` not returning
  what `consoleInit` left there -- the code checks for 240 x 320 and draws nothing rather than
  guessing, so a blank map with live text is that check firing; *the map drawn sideways or in
  stripes* is the framebuffer's column-major, bottom-up layout, which
  `tests/map_test.cpp` pins against the exact strides but only against this build's idea of them;
  *the map appearing and then being eaten a row at a time* is console text reaching past sixteen
  columns; and *the whole thing tearing or lagging a frame behind* would mean `gfxFlushBuffers` is
  not enough for a screen the CPU is writing under a GPU-driven top screen. **Read the `draw` figure
  in the left-hand column while flying** -- it already earned its place once: it read 5,000 µs, which
  is what the patch cache in §0l was built to answer. It should now read a fraction of that while
  flying, and jump for one frame when the texture pack or the grid changes.

### 2. The M2 gate — **half measured, and the baseline fails**

Performance gates: o3DS ≥ 30 fps at distance 6 with 3D off; n3DS ≥ 30 fps at distance **8** with
3D on.

**The New 3DS gate was distance 10 and is now 8, provisionally.** Distance 8 is what the rest of the
engine is already sized around, so it is the honest floor to hold the renderer to while the
geometry-shader path is unmeasured; distance 10 stays the number worth wanting and is the first
thing to reconsider if the seventh launch says the cost was vertex fetch. **Lowering it does not
rescue the baseline** — see the estimate below. Refine once there is a measurement rather than a
judgement.

**The 12-byte/4-vertex baseline was profiled on a New 3DS XL at the sixth launch and misses the
second gate by 3.2× at distance 10** — and by an estimated 2.1× at distance 8, which is where the
gate now sits. The comparison the gate asks for is half done, because only this format has run on
hardware, but the half that exists is decisive about *why*, and the reason points straight at what
`docs/3ds-performance.md §2` changes.

**The cost is linear in quad count and indifferent to screen coverage.** Three views, one constant:

| view | GPU draw, less the 0.8 ms fixed cost | quads | µs/quad |
|---|---|---|---|
| distance 10, looking along the surface | 16.2 ms | 76,077 | 0.213 |
| distance 10, a denser view | 51.8 ms | 254,292 | 0.204 |
| distance 4, same spot | 12.4 ms | 58,582 | 0.212 |

Quad count moved **4.34×** between the last two and GPU time moved **4.18×**. Looking straight down
— 2,838 quads covering the whole screen — costs 1.0 ms; looking along the surface at 27× the quads
costs 16× more. Fill rate is not what is being spent.

**0.208 µs per quad = 52 ns per vertex = 13.9 cycles at 268 MHz.** Against the gate:

| configuration | quads, both eyes | GPU | |
|---|---|---|---|
| distance 10 dense, 3D off | 254,292 | 53.7 ms | 18.6 fps |
| distance 10 dense, 3D on | 508,584 | 106.6 ms | 9.4 fps |
| distance 10 along the surface, 3D on | 152,154 | 32.4 ms | 30.8 fps |

30 fps with 3D on allows **78,433 quads per eye**. The dense view at distance 10 is 3.2× over it;
60 fps is 6.6× over.

**Distance 8 is the gate now, and it has not been measured — but the estimate says it fails too.**
Nothing on hardware was taken at distance 8, so this is arithmetic and labelled as such. Two
independent proxies agree on what dropping 10 to 8 buys: geometry in range falls from 34.76 MB to
22.76, a factor of **0.65**, and a 70° wedge's area scales as *d²*, giving **0.64**. Against a dense
view's 254,292 quads both eyes that is roughly **163,000–173,000**, or ~85,000 per eye with 3D on —
call it **2.1×** over the 78,433 the gate allows, against 3.2× at distance 10.

So the baseline fails the new gate as well, by a smaller factor. That is the point of moving it: a
gate the answer has to clear by 2.1× is a target the geometry-shader path could plausibly hit, where
3.2× was a number no vertex-side change was going to reach. **Take the real distance-8 dense figure
at the seventh launch** — in both formats, one session, one position — and replace this paragraph
with it.

**The geometry-shader path is now built, and the seventh launch is what it is waiting for.** One
8-byte vertex per quad, expanded by `shaders/quad.g.pica`. Everything a host can check about it is
checked and in `docs/3ds-performance.md §2`; the summary is that **it costs nothing to adopt** —
identical quad counts to the last quad, 107.34 MB of world geometry down to 25.96, 31.6 µs a section
down to 28.7, and the VBO pool stops being a constraint at every configuration (distance 8 on an old
3DS goes from 11.15 MB at its ceiling with 1,014 evictions to 3.33 MB with none). None of that is
the gate. The gate is GPU time per quad, and it can only be re-taken on hardware:

> **Settings page → cube format → geoshader.** Wait for the world to re-mesh, then compare GPU draw
> on the Info page against the same view in the 12-byte format. Take both halves in one session, at
> one position, or the comparison is between two different views.

**Either answer is decisive, which is why it was worth building before the measurement rather than
after.** The two paths emit *identical triangles* — same count, same positions, same winding — and
differ only in what is fetched and how many times the vertex shader runs. So:

- **Faster** means the cost was vertex fetch and shading. 60 bytes a quad becomes 8, and the vertex
  shader runs once instead of four times. The remaining lever after it is a shorter shader.
- **Unchanged** means the cost is per-triangle setup, and **no vertex-side optimisation will ever
  help** — not a shorter shader, not a smaller vertex, not this. The only lever left is fewer
  triangles, i.e. greedy meshing, and that has a blocker of its own recorded below.

The risk in the design, stated in advance so the measurement is not read as confirming it: the
vertex half is 44 instructions divided across three shader units, and the geometry half is 29
divided across none. If a geometry-shader invocation is serial on one unit, the shading side gets
*worse* and only the 7.5× less fetch is working in its favour. That is precisely the split §2 was
never able to name, and this measurement names it.

**Where the 13.9 cycles go, and why the vertex shader is the smaller half.** The PICA has four
vertex-shader units, so a 22-instruction shader is 5.5 cycles of shading per vertex. The other
**8.4 cycles are fetch**: at 12 bytes a vertex and four vertices a quad, plus six 2-byte indices,
the dense view reads **15.3 MB per frame per eye — 295 MB/s of FCRAM**. Shortening the shader
therefore addresses at most 40 % of the cost, which is why it is not the answer on its own.

**This is the case for §2 restated in measured terms.** One 8-byte vertex per quad is 8 bytes where
the present path spends 60, **7.5× less traffic**, against a cost that §2 already names: geoshader
mode drops vertex-shader parallelism from four units to three. It attacks the 8.4 cycles, which is
the part that dominates.

Two things multiply with it and neither has been measured yet:

- **Greedy meshing** reduces the quad count itself, so it multiplies with anything that reduces
  cost per quad — and it is the *only* thing that helps if the cost turns out to be per-triangle.
  It is listed under "deferred" as legal only when smooth lighting is off, and smooth lighting is
  off, so lighting does not block it. **The atlas does.** A merged 4×1 face needs its tile repeated
  four times across the quad, and the PICA's wrap mode is a property of the whole texture: with
  every block sharing one 16×16 atlas, `GPU_REPEAT` repeats the atlas, not the tile. Nothing in the
  mesher can work around that. The fixes are all changes to the atlas — a padded atlas where each
  tile is stored as an *n×n* repeat, capping merge runs at *n*, or one texture per tile and a draw
  call per tile per section — and each has a cost that has not been measured. This is a real
  blocker and it was not recorded before.
- **A shorter vertex shader.** The light-nibble unpack is 6 of the 22 instructions and a 256-entry
  lightmap indexed by the packed byte would make it one; the two setup `mov`s go if the position
  attribute is declared 3-component, since w then defaults to 1.0.

**What is now known not to be the cost**, recorded because it was predicted in writing and the
prediction was wrong. The fifth launch turned the alpha test on for every pass, and this file said
to expect the PICA to disable early-Z and for the cube pass to show it. Toggling the cube pass's
alpha test on hardware — a settings-page switch, sound only because the placeholder atlas has no
cutout cube tiles — **changed GPU draw by nothing measurable**. Early-Z is a fragment-stage concern
and this renderer never reaches the fragment stage in quantity. The alpha test is correct, faithful
and free, so it is now unconditional and the switch is gone: `drawEye` no longer sets it per pass,
`drawFrame` sets it once. Putting it back is five lines if anything ever claims to be
fragment-bound.

### Deferred but not forgotten

- **A bundled art pack.** The generated Dev Art is the fallback and it is not going anywhere — it
  needs nothing off the card, and a wrong texture index is *visible* in it rather than plausible.
  What is not built is a CC-licensed pack in RomFS to sit beside it, and there is no RomFS in the
  tree. Low value now that a real pack is two button presses away.
- **The rest of a pack's files.** `gui/`, `mob/`, `default.png`, `char.png`, the sun and the moon are
  all carried into a pack, counted on the pack screen, and read by nothing. Each needs a consumer
  rather than any more pack machinery: a glyph atlas and a text drawer for the font, a widget sheet
  for the menu, mobs for the mob skins.
- **`cache/<pack>.3dtex`.** Deliberately not built — see [assets.md](assets.md). It was designed
  against a pipeline with per-image GPU format selection in it; what is left is one PNG decode and
  one CPU pass over 65,536 texels at the moment the player picks a pack. Build it when hardware says
  that pass is slow enough to notice — and the swizzle being back on the CPU (ninth launch) makes
  that measurement worth taking rather than assuming.
- **A map that survives closing the world.** The sample store is memory only. Persisting it needs
  two things that are each a design step: the card is not allowed on the render thread, so tiles
  would have to go through the I/O thread or through the two moments a world already blocks; and the
  packed-world converter stashes every file it does not recognise *into* the container, so a map
  directory would vanish from plain view the first time a world was packed unless it were given the
  same "carried as well as stashed" treatment `3dalpha.ini` has. Getting the second one wrong is data
  loss on a conversion. Until then the map remembers 512 chunks on an old 3DS and 1,536 on a New one,
  which is a session's worth.
- **Meshing on a worker thread.** Everything is on the main thread behind a 4-sections-per-frame
  budget. `WorldStreamer` is the seam. Doing it now would be building on a guess: the budget that
  makes the main thread survivable is measurable, and there is no frame time to measure yet.
- **Smooth lighting / AO.** The 12-byte vertex format already carries per-vertex colour and light
  for it; **the 8-byte quad format cannot carry it at all**, so if the geometry-shader path wins,
  these two are mutually exclusive and that is a choice rather than an oversight.
- **Greedy meshing.** Legal as far as lighting goes — smooth lighting is off — and blocked by the
  atlas: see the note in §2 above. Whether it is worth unblocking depends entirely on the seventh
  launch.
- **Translucent cubes.** Ice is flagged `translucent` in the block table and the mesher emits it
  into the *cube* stream, which is drawn in the opaque pass, so ice is currently solid. It is the
  only such block in a1.1.2 and fixing it means either a fourth range in the 12-byte format or
  promoting those blocks to the 16-byte one. Worth doing when a second version needs it; ice is not
  worth a whole vertex range on its own.
- **The rest of the non-cube render types.** The second vertex format and its draw pass exist, and
  **cross** (saplings, flowers, mushrooms, sugar cane), **fluid** and **torch** are emitted through
  them. `mesh::hasEmitter()` is the list, `--mesh` prints it, and a test meshes one block of every
  render type to check it against what the dispatch actually reaches — "implemented" and "reached"
  are different claims and the gap between them is invisible. Still missing:

  | type | blocks in the measured world | what it needs beyond the format |
  |---|---|---|
  | rail, ladder, door, crops, lever | — | metadata |
  | stairs, fence, cactus | 6 | shape only, no metadata |

  None of these is worth much geometry — between them they are 0.004 % of the measured world's
  blocks — but a torch that is invisible is worse than a lake that is opaque, so they are gameplay
  work rather than renderer work. Measure any of them against **n3DS distance 10**, which is the
  configuration with the least pool headroom left.

  Torch cost nothing measurable, and that was checked rather than assumed: the world held none *at
  the time this was measured*, so every `--mesh` number was unchanged to the last quad — 1,282,900
  cube quads, 31,626 opaque and 55,618 translucent detail, 64.05 MB — and mesh time with and
  without the dispatch,
  best-of-five on the same host in the same session, was **30.8 against 30.6 µs**. (Both arms read
  high against the 28.5 µs recorded above; that is this host today, not the change.)
- **Face textures that depend on metadata or on the world.** `MeshScratch` carries metadata now,
  but `blocks.json` only records the no-metadata, no-neighbours answer, so using either needs the
  extractor to emit a fuller table. `extract_blocks.py` names the fourteen blocks affected on every
  run — six on the world, eight on metadata — so the gap stays visible rather than silent. Grass
  never wears snow, and chests and furnaces always face the same way. Seven of the eight
  metadata cases are non-cube types with no emitter yet; for cubes it is farmland alone, wet
  versus dry.
- **The rest of the `MeshScratch` fill.** The interior is bulk-read now; the 1,736 shell cells still
  go through `ChunkColumn` one at a time. Worth roughly 2 µs of the remaining 10.0, so it is the
  smallest thing on this list rather than the obvious one it used to be.
- **Chunk index cache** (`cache/<world>.idx`) so the base36 tree is not walked on every open.
- **Protocol-2 field names/order** must be pinned against an OrnitheMC decompile and a live capture
  before M5. Payload sizes are certain; names and order are inferred. The jar is available locally.
- **Chests/furnaces/signs with contents** round-trip as opaque preserved blobs and have never been
  parsed. The richest chunk tested had 3 tile entities and 2 entities.

### 0m. Four hardware symptoms, two faults — the generator's live set, and the map's budget

Reported from hardware together, which is why they were treated as one report: **the minimap stays
blank on joining a world until you move around a bit, though chunks are generating the whole time;
after a lot of chunks in one session generation stops until the world is reloaded; trees sometimes
generate only half; and in a snow world the snow is sometimes missing, and sometimes covers only
half a tree.** Two faults, and three of the four are the same one.

The last of them was a fidelity question rather than a bug report — *does a1.1.2 do this?* — and it
splits in two. **A half-snowed tree is the original's own behaviour.** Snow is the last thing a
population pass does and it is offset by +8 like everything else, so a pass snows 64 columns of its
own chunk and 192 of its neighbours'; a tree whose canopy crosses into the next pass's square gets
snow on that half only if that pass runs *after* the tree was placed, and `ft`'s trigger order
follows where the player walked. **A half tree is not.** Population writes land only in the 2×2
quadrant east and south of the pass — asserted exactly against a real World, `changedColumns == 4` —
so both halves of a tree on a chunk border are written by one call. Nothing in the original can
place one of them and not the other. That made the tree the thing to chase, and it led to the same
fault as the stopping.

**What was wrong: the generator's live set grows with the distance walked, and nothing drained it.**
A column `ChunkGenerator` has not handed over is *live* — its neighbours' population passes are
written into it and nowhere else — and `acquire` refuses to evict one for exactly that reason. What
finishes a live column is its four passes running, and a walk never finishes the ones at the sides
of the corridor it sweeps: their eastern and southern neighbours are never asked for, so they stay
live for the rest of the session. `cacheColumnsFor` was fitted to `--generate`, which fills a disc
from a *fixed* centre and stops, and that measurement cannot see this at all.

Measured under `--fly ... gen` at render distance 8 with the cache made effectively unbounded, peak
live against distance walked:

| chunks walked | 75 | 200 | 400 | 600 |
|---|---|---|---|---|
| peak live | 235 | 274 | 372 | **468** |

against a cache of 240. Linear in distance travelled and bounded by nothing, so **no size is the
right size** — which is why the counter that fires, `evictedLive`, had been read as "the cache is
too small" and was not. Past that point `acquire` finds no delivered victim and takes a live column
anyway; it comes back as bare terrain with its neighbours' work missing (**the half trees, and a
chunk with no snow in it**), it is never final again, so `lightable` fails for it and its neighbours
every frame for the rest of the session (**generation stopping**), and reloading the world is the
only thing that clears it — which is exactly what was reported. On the host, 3,000 frames of walking
gave `662 sweeps failed — 662 unlightable`, 223 columns still owed, and the world visibly
un-generating as the player walked out of what was already made.

**The fix is `ChunkGenerator::retire(centre, radius)`: forget everything further than `radius` from
the player, live or delivered.** Dropping a *region* is what makes dropping a live column safe, and
dropping one on its own is what is not — a lone live column comes back bare while the passes that
wrote into it stay marked done, and its share of their work is lost silently. Everything outside the
radius goes together, so the neighbourhood re-derives as a unit: the state a world reload leaves
behind, which the design already accounts for (see `ChunkCache::save` on regenerating against
populated neighbours, and the note that a1.1.2 has the same hazard in a worse form). `WorldStreamer`
publishes the player's chunk under `queueLock_` and the generation thread retires against it before
each sweep, at `loadRadius + 6` — three rings clear of the furthest a sweep can reach.

The result, same runs: `evictedLive` 0, failed sweeps 0, and peak live flat in both distance and
time — 196 at 6,000 frames of walking and 188 at 9,000, where it had been 372 and 468. Across render
distances 3 to 11 it comes out at 128, 158, 192, 224 and 256, which is **`16 × loadRadius + 64`
almost exactly** against `cacheColumnsFor`'s `16 × loadRadius + 96`: a flat 32 columns of headroom
at every distance. So the old formula was right after all — it was only ever measuring the
standing-still case, and retirement is what turns walking back into it.

**And the generator no longer depends on being told.** `acquire` has a last resort of its own: with
nothing delivered that it could take, it lets go of everything outside the sweep in progress — a
region, so the same argument holds — rather than taking a live column. A caller that never retires
(a probe, a test, a future one) then gets a few failed sweeps that clear on the retry the streamer
already does, instead of a silently corrupted world. `evictedLive` is now genuinely unreachable, and
it stays in place as the assertion it always was.

`tests/generate_test.cpp` pins all of it: a forty-column walk with retirement produces the **same
world block for block** as the same walk with a cache large enough never to need it, and the same
walk in the same small cache with no retirement at all is checked to finish anyway, with
`evictedLive` at zero and no column left unmade.

**The blank minimap was its own fault, and a much smaller one.** The map samples chunks the game
already holds, and it took **one a frame on an old 3DS and two on a New one**, scanned in raster
order from the north-west corner of the window. A cold window is up to 196 chunks, so that is six
seconds — and because the scan started north-west, the ground *under the marker* was not reached
until halfway through it. It also asked `WorldStreamer::residentColumn`, which required the column
to be `published`: that means the renderer has it, which needs all eight neighbours *and* the mesh
distance to reach it. At world entry almost nothing is published yet, so there was nothing to sample
however large the budget was; and below render distance 7 the map reaches further than the renderer
does, so its outer ring was permanently unexplored ground.

Three changes, all small: the scan runs in **square rings out from the player's chunk** (which is
also the order the streamer brings columns in, so the budget is rarely spent on a chunk that has not
arrived); the budget is **8 an old 3DS, 16 a New one** against a sample's ~50 µs, with a **cold-start
burst of 64** that ends the first time a pass finds nothing left to take; and `residentColumn` now
answers for any `Loaded` cell. A `Loaded` column came from the card or from the generator and both
hand over finished, lit work — publishing is a *meshing* gate and was never the honest one here.

---

### 0n. The two waits a player sits through — a bar, and a square of chunks arriving

Two screens in this shell were a still picture while the console worked: **leaving a world**, which
writes a file per dirty column, and **entering a world that was just created**, which generates one
before it hands over. Both had a number on them — the save counter is 0d above, the generation
screen printed the streamer's own counters — and both were the bottom screen's text console under a
frozen top screen. A still top screen for tens of seconds is read as a crash, whatever the bottom
one says.

Both are now `ctr::ProgressScreen`: a green bar over the frozen world on the top screen, and on the
bottom screen — while a world is being made — a square of the chunks around the player, one cell
each, coloured by what that column is doing.

**It draws into the renderer's frame, like the pause menu.** `Renderer::drawFrame(camera, this,
entry)` — no render target of its own, both eyes, the same scrim `GuiScreen` draws over an open
world. That is the arrangement 0d settled and the reason it is reused here without argument.

**The bar's geometry is core's.** `gui::barFillWidth` decides how much of a track is filled, and
both the top screen's citro2d bar and the bottom screen's software one ask it, so they cannot round
differently and be visibly out of step at the ends. The green is the XP bar's own, RGB (128, 255,
32). Nothing owed fills the bar rather than emptying it — a world the pause menu saved a moment ago
owes nothing and has not failed to save, which the line under the bar says in words.

**The square is a five-colour ramp, and the states are the streamer's.** `WorldStreamer::progressGrid`
fills a `gui::ChunkState` per cell; black is a cell nothing has been asked about, red is owed, orange
is the column the worker has in hand, yellow is loaded but not yet drawable, green is published. It
emits the drawing enum rather than one of its own on purpose: the grid's `CellState` is private and
must stay so — `Ungenerated` versus `Absent` is a meshing rule, not something a screen may act on —
and the alternative was a second public enum existing only to be switched into the first.

`Absent` is drawn as green rather than as a sixth colour. It is the edge of a finite world: nothing
is owed there and nothing is coming, so a colour of its own would be a part of the square a player
would watch, waiting.

**It scales with the render distance.** `gui::fitChunkGrid` derives the cell pitch from the box
rather than from a constant, so distance 2 is seven cells at 16 pixels (a ceiling, or five cells
would fill a 176-pixel box) and distance 12 is twenty-five at 6 — and distance 24, the debug page's
ceiling, still fits at 3. The pitch floors at one pixel and every draw clips, so no render distance
can put a pixel off the screen. The whole bottom-screen layout is `static_assert`ed against 320 x 240
at compile time: overlaps and overruns are arithmetic, and checking them on the target is cheaper
than checking them on a console.

#### The generation wait now covers the whole render distance

It used to stop at "geometry exists, or nine columns are resident" — the smallest thing that is not
an empty screen, and also exactly what a player then walks straight off the edge of. **The target is
every column inside the render distance published**, which is the streamer's word for "the renderer
has it and it can be drawn". That in turn forces the ring one chunk further out to be generated,
because a section cannot be meshed until its eight neighbours are in memory — so the world handed
over reaches the horizon in every direction and one chunk past it. At distance 12 that is 625
columns; at distance 6, 169.

The square and the bar both count at the render distance rather than at the load radius, and that is
deliberate: the renderer's field is only as wide as the render distance, so the outer ring can never
be published and a square drawn out to it would have a border that never turned green.
`streamer_progress_test.cpp` asserts exactly that — the ring at the load radius settles on `Ready`
and stays there.

Three ways out, because "generating" must never become "hung":

- the square fills;
- the player presses START, which is why the hint is on both screens; or
- nothing new becomes drawable for 45 seconds. That is a generator that has stopped rather than one
  that is slow — the first published column is the slowest, since nothing can publish until a 3x3 of
  columns exists and on an old 3DS the worker shares core 0 with this loop. The outer cap is six
  minutes and exists only in case the stall detector is itself wrong.

#### What holds it

`tests/progress_test.cpp` covers the painter — the fill fraction (including the 64-bit multiply a
column count off the cache needs), the two-tone edges, the pitch derivation at every distance from 2
to 24, the gap coming out of the pitch rather than being added to it, and the square's row-major
north-up orientation — all of it through both stride conventions, the row-major one and the
console's own 240-down-a-column bottom-up.

`tests/streamer_progress_test.cpp` covers the states against a real streamer rather than a mock: the
square is black before a frame has run, fills to green out to the render distance and no further,
holds every colour on the ramp part way through with generation inline, never counts backwards while
the centre is still, and shows nothing as owed in a world that generates nothing.

**559 tests pass, and TSan is clean.** `progressGrid` takes `queueLock_` once for a whole square to
read which column the worker has in hand; everything else it reads is the main thread's grid, which
is the same rule `residentColumn` follows.

**Unrun.** It builds, links and `check3dsx.py` accepts the image; nothing here has been on hardware.
The two numbers worth taking off a console are how long a distance-12 creation actually takes, and
whether redrawing the square every frame is visible in the generation rate.

### 0o. The world tick -- the clock, the update list, and fifteen blocks that do something

**M3's foundation, and the first thing in this project that makes the world change on its own.**
Everything up to here made world and drew it; nothing in it ever moved. The tick is what grass
spreading, ice melting, a crop growing and sand falling all hang off, and it is the same object a
player's own edits will go through when there is a player. The whole derivation -- the clock, the
scheduled-update list, the 80-samples-a-chunk random tick, the per-block tables and every deviation
-- is in [tick-a1.1.2.md](tick-a1.1.2.md), which is where to read before touching any of it.

**The clock was wrong and it did not look wrong.** `main.cpp` advanced `worldTicks += dt * 20.0` as
a double, which is right for the sky and useless for anything else: a block update happens a whole
number of times or not at all, and at 30 fps two thirds of a grass tick is not a thing that can run.
`ir.class` is a1.1.2's own accumulator and it is now transcribed -- twenty ticks a second out of an
accumulator, the whole part run and the fraction kept for the renderer's interpolation. Three
details in it are load-bearing and all three are the original's: the raw delta is clamped to **one
second** before it is scaled; the whole part is taken, subtracted, and only **then** capped at ten,
so ticks past the tenth are **dropped** rather than deferred and the world falls behind instead of
spiralling; and the fraction survives across frames, which is what keeps a day twenty minutes long
at a frame rate that is not a divisor of 20. `TickTimer::droppedTicks()` counts what the cap threw
away, which is not in the original and is how "the console is behind" stops being invisible.

**The one thing not transcribed is the two-clock correction**, and that is a deviation with a
reason rather than an omission: a1.1.2 corrects `System.nanoTime` against `System.currentTimeMillis`
once a second and smooths the ratio, because those are two clocks on a 2010 PC and the fast one
drifts. A 3DS has `svcGetSystemTick` off a fixed oscillator and nothing to correct it against, so
the filter would be a ratio of exactly 1.0 for ever.

**The tick dispatches on a behaviour, never on a block id**, which is the same rule the mesher
follows with render types and it is there for the same reason. `BlockDef` grew three columns --
`tick` (a `TickBehaviour`), `tickRate` and `tickRandomly` -- and 41 of a1.1.2's 70 blocks carry a
behaviour. A version whose grass is not id 2 needs no code change.

**Two of those columns are read out of a running JVM rather than out of the bytecode, and that
caught three real errors.** `tools/extract_ticks.java` loads `Block`'s class initialiser and prints
`tickOnLoad[]` and `tickRate()` per id. Reading the constructors statically gets **still water,
still lava and the two redstone ores** wrong, because those constructors branch: `BlockStationary`
sets `setTickRandomly(false)` and then `true` again only for lava, and `BlockRedstoneOre` is one
class constructed twice with a flag, so the unlit ore does not tick randomly and the lit one does.
Every one of the three is invisible until a world has been left running for a while.

**What the tick actually costs, stated before anyone measures it.** a1.1.2 samples **80 positions
per chunk per tick** over a 19x19 square of chunks around the player -- 28,880 samples a tick,
577,600 a second -- and almost every one of them is `tickOnLoad[id]` coming back false. On a 3DS the
square is `min(9, loadRadius)` instead, because a column that is not held cannot be ticked; at the
render distances this console runs, ours is the smaller number and a world simulates a little less
far out than the original. **Nothing here has been measured on hardware**, and the sampler is the
one part of this module with a real chance of showing up in a frame -- the block read goes through
palette-compressed sections rather than a1.1.2's flat byte array.

**It is on the main thread on purpose.** The order of block updates is the world in the same way
generation order is (§0g), so moving it is a decision to take with a measurement in hand rather than
on the way past. What could go to core 2 later is the read-only half -- sampling the positions and
filtering them against `tickRandomly` -- with the effects still applied in order on core 0, and the
seam for that already exists: `tick::TickWorld` reaches chunks through a pair of function pointers
and not through `WorldStreamer`, which is what lets the whole module be tested on a host with no
renderer, no streamer and no card.

**The scheduled-update list is the original's ordering in a structure the frame path is allowed to
use.** a1.1.2 keeps a `TreeSet` and a `HashSet` side by side and checks their sizes against each
other every tick; entries are equal on `(x, y, z, blockId)` and order on `(scheduledTime, insertion
sequence)`. That insertion tie-break is not decoration -- it is what makes water spread in the same
pattern twice. Ours is a fixed-capacity binary heap with an open-addressed membership set beside it,
the same two answers in one object, with the original's consistency check kept as `consistent()`.
Both of a1.1.2's numbers are kept: the **8-block residency box** around anything scheduled or run,
and the **1,000-per-tick** cap. The pool refuses the newest entry when it is full and counts it,
because refusing the newest is the failure that recovers.

**Twenty-five behaviours are ported and the rest are named rather than quietly absent**: grass spread
and death, leaf decay, sapling and crop growth, flower and mushroom placement, farmland wetting and
reverting, sugar cane, cactus, ice, both snows, torch support, sand and gravel falling, **the fluids, fire,
and all of redstone but its inputs** -- wire, torch, ore, lever, button, pressure plate and door.
Not ported: **the inputs themselves** (pressing, flipping, an entity on a plate, opening a door by
hand), **rails**, and **a sapling actually becoming a tree** (both generators exist but write through `PopulationView`, so it wants an adapter
rather than a call), and TNT, sponge and the tile-entity ticks.

#### The fluids, and the one thing about them that matters on this console

Water and lava are the largest single behaviour in a1.1.2 -- three classes and a recursive search --
and `core/tick/fluid.cpp` is their own file for the same reason `core/mesh/fluid.cpp` is. The whole
transcription is in [tick-a1.1.2.md](tick-a1.1.2.md) §*The fluids*; what belongs here is the part
with a cost.

**A fluid is a pair of blocks, and that pair is the performance design.** The flowing form ticks;
the still form does not. A flowing block that finds nothing left to change turns itself into the
still block and stops being scheduled, and a still block that hears a neighbour change turns back
and schedules itself. **An ocean therefore costs nothing per tick and a waterfall costs one
scheduled update per block per five ticks** -- which is the difference between a1.1.2 being playable
on a 268 MHz ARM11 and not. Neither transition notifies its neighbours, and that is load-bearing
rather than an omission: notifying would wake the neighbours, which would set *them* not-static,
which would notify back for ever.

**Writing a block turned out to be three things, and only one of them was implemented.** The fluids
found it: water spread exactly one block and stopped for ever. `Chunk.setBlockID` runs
`onBlockRemoval` on what was there, **clears the metadata**, and runs **`onBlockAdded` on the new
block** -- and that last one is not a neighbour notification, it is how a flowing fluid and a
falling sand block schedule their own first update. Both were missing. Clearing the metadata matters
on its own: without it a cell that held water at level 5 and is emptied and refilled inherits the 5,
and a torch keeps the face it used to hang on. Seventeen block classes override `onBlockAdded`, so
this was never only a fluid problem -- it was just the first behaviour big enough to notice.

**Where a fluid spreads is a search, not a fan-out**, and it is the one thing here worth measuring
on hardware. `getOptimalFlowDirections` looks up to four blocks along each of the four horizontal
directions for somewhere the fluid could fall and spreads only along the directions tied for the
shortest path -- recursive, depth-limited at 4, four-way. It is what makes water find a hole across
the room instead of creeping outwards evenly, and it runs on every flowing block every five ticks.
Nothing about it has been measured yet.

#### Fire, and three tables that are not one table

Fire's own file, and the thing worth carrying away from it: **a1.1.2 asks three different questions
about burning and answers them from three different places.** `chanceToEncourageFire[id] > 0` is
"there is fuel here" as a fire block sees it; `abilityToCatchFire[id]` is rolled to decide whether
that block is actually consumed; and `Material.getCanBurn()` is what **lava** reads. The first two
cover six blocks. The third covers fourteen -- chests, crafting tables, signs, doors, jukeboxes and
fences as well -- so those catch from lava and are invisible to fire's own spread. Conflating them
lights the wrong things in both directions, and it would have been the easy mistake: two of the
three look interchangeable until you count them.

All three now ride in `blocks.json`, read from a running jar. `tools/extract_ticks.java` finds
BlockFire's tables **by shape** -- the only block carrying two *instance* `int[]` as long as the
block table -- and tells the two apart by their contents rather than by a name that means nothing
across versions. The first attempt matched every block instead of one, because `getDeclaredFields`
hands back Block's own statics too.

One rule reads backwards and is worth stating so nobody "fixes" it: the per-direction spread chance
is the **bound** of a roll that has to land under the target's ability to catch, so a *lower* number
is likelier -- and a1.1.2's numbers are 200 below, 250 above and 300 to the sides. **Fire spreads
downward most eagerly.**

#### Redstone: the power model, the wire and the torch

**The line the whole system rests on is one branch in `World.isBlockIndirectlyProvidingPowerTo`:**
an opaque cube answers with whatever is powering *it*. That is why a torch under a block powers what
stands on top of it, and why the indirect query cannot be written as a wrapper round the direct one.
There are four power queries, not one, and a1.1.2 uses all four.

**Two things in the wire are easy to lose and each breaks something specific.**
`wiresProvidePower` is a flag on the shared Block object that the wire turns *off* for the length of
one `isBlockIndirectlyGettingPowered` call, so that a wire working out its own strength does not
count itself and its neighbours as sources -- without it every wire in the world reads 15. And the
strength is decremented a **second** time after being written, with the propagation comparing
against that smaller value; comparing against the written one re-walks the whole net at every step.
The flag lives on `TickWorld` here rather than beside the wire, because it is read through the power
queries.

**Fifteen blocks is not a rule anywhere in the code.** It is what falls out of starting at 15 and
losing one per block. Likewise the wire's *shape* is stored nowhere: which sides a lit wire powers
is worked out from what is around it every time it is asked, and a corner powers neither of its
ends.

**The torch is the only active element a1.1.2 has**, and it is a NOT gate: lit unless its support is
powered, at a tick rate of 2. That delay is what every circuit in the game is timed by. Its burnout
rule -- eight toggles at one position inside 100 ticks and it stays dark -- is the brake on a torch
wired to itself, and it is asymmetric: going out records a toggle, coming back on only reads the
count. a1.1.2 keeps that record in an unbounded static list; ours is a bounded ring on `TickWorld`
that drops the oldest entry, which can only ever make a torch *less* likely to be called burnt out
-- the safe direction, since a torch that goes dark for no visible reason is the worse failure.

**One bound is ours.** `updateAndPropagateCurrentStrength` recurses through the wire net with no
guard in the original, which is safe there because a signal dies after fifteen blocks and so does
the wave of changes. A 3DSX main thread has 32 KB of stack, so the recursion is capped at 64 --
four times the reachable depth, and a guard rather than behaviour.

**The failing tests this round were the tests, again, and the reason is worth keeping.** Placing a
block *raw* runs `onBlockAdded` but not the neighbour fan-out, and a torch's `onBlockAdded` notifies
the six cells **around** it rather than the six cells **of** it -- so a wire laid beside a torch
placed raw never hears about it. Nothing was wrong with the code; the test was placing blocks in a
way the game never does.

#### What of redstone needs a player, and what turned out not to

The question worth answering was how much of the remaining redstone was really blocked on the player
entity, and the answer is: **only the inputs**. Pressing a button, flipping a lever, an entity
standing on a plate and opening a door by hand are inputs; every other part of those four blocks is
reachable by writing the metadata the input would have written, which is what a test should be doing
anyway. So they are built and tested now, and what is left is a player, not a design.

| Block | Needs a player | Built and tested |
|---|---|---|
| Lever | flipping | which sides it powers, falling off a wall that goes |
| Button | pressing | the same, plus letting itself back out 20 ticks later |
| Pressure plate | an entity on it | which sides it powers under load, falling off |
| Door | opening by hand | **opening and closing because a circuit said so**, both halves in step |

**A lever powers what it is attached to, and a torch powers what is above it** -- and that
difference is the one thing here a test caught. A floor lever's face 5 answers side 1, which is the
query the block *below* makes; a torch answers side 0, which is the query the block *above* makes.
The failing check was the assertion, not the code, for the fourth time in this module -- which is
itself worth noting: every failure in the tick work so far has been a wrong expectation about
a1.1.2, never a wrong transcription of it.

**The door has one economy worth keeping**: `onNeighborBlockChange` returns early unless the block
that changed can provide power, so a doorway in an ordinary wall costs nothing at all.

**Rails are skipped deliberately.** A rail's only behaviour is recomputing its shape from its
neighbours, and shape means nothing without a minecart to run on it. It is the one part of redstone
whose payoff really does wait for entities.

And one more thing that looks like a leak: **a fire that reaches age 15 stops scheduling itself.**
The metadata write and the re-schedule are both inside `if (age < 15)`, so a fully aged fire that is
still being fed leaves the scheduled list and is revisited only by a random tick. It is the
original's behaviour, it is pinned by a test, and the test exists because the obvious "fix" is
wrong.

**Lava's ignition is wired now**, which was the one piece of the fluids left out while fire did not
exist. Not wired: TNT primed by fire, which needs an entity to be primed into.

**One bound that reads like an artefact and is not.** a1.1.2's leaf decay walks outward from a
changed block and two adjacent leaves can each decide the other needs re-running, so the walk is
bounded -- `iz.c`, a counter on the shared `Block` object capped at 100 and reset at the entry
points. It was tempting to drop it as a Java artefact; it is copied exactly instead, because the
3DSX main thread gets **32 KB of stack that nothing in the binary can enlarge** and an unbounded
walk through a canopy is precisely the shape of the stack overflow §1's first launch already found
once.

**Two gaps worth knowing about before building on this.** Sand and gravel land in one tick, because
`EntityFallingSand` has nowhere to live yet -- the resting place is the one the entity would have
found, so the world ends up identical and only the fall is missing. And **block light is not
repropagated when a tick changes a block**: `LightEngine` computes a whole column against a 3x3
window and there is no incremental path, so a melted ice block leaves the light as it was until the
column is next lit. The **height map** *is* maintained, because `canBlockSeeTheSky` is what decides
whether a plant may stay. Incremental relighting is the largest open item in this module.

**What holds it.** `tests/tick_test.cpp` covers the clock (including that the first call owes
nothing, that 60 fps still yields exactly 20 ticks a second, and that a one-second stall drops ten
ticks rather than deferring them), the scheduler's ordering, identity, tombstone sweep and overflow,
and each ported behaviour driven directly rather than through a random tick -- waiting for a random
tick to land on one nominated block is 1 in 32,768 an attempt, so a test that does that measures the
sampler. Two tests cover the sampler itself. `--fly` runs one tick a frame over a real world under
ASan and UBSan, which is the only place the tick meets real columns, the mesher and the saver.

**Edits reach the card through the autosave rather than through the frame.** A block a tick changes
invalidates its section -- and its neighbour's, per axis, only when it is on a boundary -- and its
column is added to a small dirty set. That set is handed to the cache at the save boundary, so a
fluid that touches one column a hundred times in a second costs one clone rather than a hundred.
This is the coalescing §0e predicted the timer would be for.

---

### 0p. Audio -- the music timer, a decoder and a DSP that may not be there

**a1.1.2's random background music, transcribed rather than approximated.** The whole feature is
`of.c()` and one `int`, and the constants are out of the jar, not out of a wiki: the counter starts
at `nextInt(12000)` -- 0 to 10 minutes -- and resets to `nextInt(24000) + 24000`, 20 to 40 minutes.
[audio-a1.1.2.md](audio-a1.1.2.md) has the bytecode beside the transcription.

**The rule that would have been lost.** Both `playing()` checks sit *before* the decrement, so the
counter does not run while a track is on: the 20-40 minutes is silence *between* tracks and the
real period is the track's own length plus that. A ticker that decremented unconditionally plays
about one track an hour too many and looks perfectly correct while doing it.
`--music-schedule` shows the difference (13 tracks in six hours against 12), and
`counterDoesNotAdvanceWhileATrackIsPlaying` pins it.

**Two random streams, not one.** `eb` keeps its own `Random` for picking a track, separate from the
SoundManager's for the counter. Sharing one would make adding a file to the card silently shift the
schedule. The pools also draw *uniformly over every entry*, keys ignored -- so nine files in
`newmusic/` against three in `music/` really does make a calm track 3/12 likely, and that is
a1.1.2's behaviour and not a bug to fix.

**No track name is hardcoded, because the jar has none.** a1.1.2 shipped no audio at all and walked
whatever an S3 bucket offered. The pool is whatever the player put in `sdmc:/3dalpha/resources/`.

**The shape underneath.** `audio::Backend` is the `IAudio` architecture.md always listed -- narrow
on purpose: start a stream, stop it, ask whether it is still playing. ndsp sits behind it on the
console, a `.wav` writer on the host. Music is channel 0 of 24, fed by eight 1024-frame wave buffers
in linear memory -- 32 KB, taken once -- and decoded by a third `WorkerRole`, `Audio`, running
Tremor. The ndsp callback signals an event and decodes nothing; starting a track hands over a path
and not an open file, so no card read lands on the frame.

**Where the decode thread runs, and why depth beats priority.** On a New 3DS it takes core 2 at the
main thread's own priority, **sharing with the generation worker rather than outranking it**. The
first cut asked for one step above; that was wrong twice over. The kernel refuses a priority below
what the process was granted and a refused `threadCreate` is silent, so it would have fallen through
to the Old 3DS path and put the decoder on core 0 of a console with a spare core, with nothing on
screen to say so -- and it buys nothing, because the decoder needs about 15% of a core against a
third of a second of buffer and does not have to win a race to stay ahead. Equal priority on core 2
is also the one arrangement already proven on this hardware: generation has used it since M4.

**On an Old 3DS it is core 0**, because a 3DSX has no other core, so CONTRIBUTING's "no
decompression on core 0" cannot be met literally. It is met in substance -- never the main thread,
one buffer per wake, and a 186 ms ring. **That path is unexercised and likely to stay that way:
there is no Old 3DS to hand.** Treat it as designed-but-unproven, not as tested.

The claim is instrumented rather than asserted either way: the Info page reports microseconds per
buffer and an underrun count. **This is going onto a console whose M2 gate currently fails by 3.2x**,
though on a New 3DS the decoder is on a different core from the renderer, which is most of why that
is survivable. If the numbers say no, the fallback is a one-time transcode to raw PCM in `cache/`,
which turns playback into a `readAt` -- a `PcmSource` implementation, not a redesign. Do not build it
before measuring.

**One bug found before it ever ran, and worth remembering as a shape.** `gWorkerIsNew3DS` and
`setWorkerThreadOps` were set inside `runGame`, which was fine while a world was the only thing that
wanted a thread. Audio comes up in `runShell`, before any world exists, so it found `workerSpawn()`
still null, failed to spawn, and reported itself `Unavailable` **on every console** -- an
instrumented failure that would have looked exactly like a missing DSP firmware. Both are installed
in `main` now. Process-wide state belongs where the process starts.

**Silence is a first-class state.** No `dspfirm.cdc`, no `resources/` folder, no decoder in the
build, or `audio=0`: all four reach `of.c()`'s own first line and return. Options -> Sound says
which, and distinguishes a missing firmware from a DSP something else is holding -- telling someone
to dump a firmware they already dumped is worse than saying nothing.

**Licensing: the notice lives in [licences.md](licences.md) and nowhere else.** Tremor is Xiph
BSD-3 and statically linked, so those 99 KB are inside the `.3dsx`, and BSD-3 asks for the notice to
accompany a binary redistribution. The decision is that this repository carries it and the binary
does not -- which is fine while builds stay among people who have the repo, and **has to be revisited
the first time a `.3dsx` or `.cia` is handed to someone who does not**. The cheap route then is a
`const char[]` behind an About screen, not a RomFS. `CONTRIBUTING.md:77,79` still names
`romfs/licenses.txt`, which does not exist; that wording is stale and was left alone deliberately.

**Sound effects are deliberately absent.** Nothing in the port can emit one: no block placement, no
player body, no entities, and `streaming/*.mus` is Mojang's own container. The reachable emitters
when M3 arrives are fizz, fire and the ambient cave counter; the seam is the pools and `Backend`, so
each is a call site rather than a subsystem.

**Both decoder branches compile and link.** `3ds-libvorbisidec` 1.2.1-3 is installed, so the 3DS
build has `MC_HAVE_VORBIS=1 MC_VORBIS_TREMOR=1`, links `libvorbisidec.a` and `libogg.a`, and passes
`tools/check3dsx.py`.

**One trap, closed.** `find_library` caches NOTFOUND, and a cached NOTFOUND is never retried -- so a
tree configured before the decoder was installed went on producing a silent binary afterwards, with
no warning, because the second configure never looked again. That is exactly the case for a
dependency somebody installs *because* the first build told them to. Both branches now clear the
cache entry when it is empty before searching, so installing the package and running `make` is
enough. It cost one real build to find. Tremor's header is `<tremor/ivorbisfile.h>`, its `ov_read` really is the
four-argument form with no endian/word/signed arguments, `vorbis_info` really does carry
`int channels` and `long rate`, and `OV_HOLE` is `-3` -- all four assumptions the `#if` rests on,
confirmed against the installed headers rather than remembered.

**What the decoder costs the binary: about 99 KB, one twelfth of the code budget.** `.text` goes
762,772 -> 809,852 (+47.1 KB) and `.rodata` 68,916 -> 120,540 (+51.6 KB); the `.3dsx` goes 861,252
-> 960,224 bytes and the loader allocates 249 pages against 240. That is Tremor's own tables, and it
is the price of audio being in the build at all -- a `MC_HAVE_VORBIS=0` build gets it back.

**Still unverified, and only hardware can settle it:** that `ndspInit` succeeds with a real dumped
firmware, that the ring does not underrun on an Old 3DS, and what a buffer actually costs. The
decode thread is 3DS-only, so ThreadSanitizer cannot reach it either -- the host has no audio thread
to race. The Info page reports decode microseconds and underruns for exactly this reason.


### 0q. Four hardware symptoms after the tick landed, and the three faults underneath them

Reported from a console once the tick system was in: **chunks being updated flash transparent for a
frame**, one **hard crash** and one **freeze**, **severe intermittent frame drops**, and **lava that
flows does not relight the world**. Four symptoms; the causes do not line up with them one to one.

The tick system is the first thing in this project that mutates blocks *during* a frame. Three
subsystems were written on the assumption that nothing does, and it reached all three.

#### The stretched polygons, and probably the crash and the freeze: the pool wrote over memory the GPU was reading

`C3D_FrameEnd(0)` only **enqueues**. The wait for the GPU queue to drain lives inside the next
`C3D_FrameBegin` -- which `renderer.hpp` already said, and nothing acted on. So the CPU leaves
`drawFrame` with the frame it just recorded still executing and runs the whole of the next frame's
streaming and meshing against a pool the GPU is fetching vertices from. `GSPGPU_FlushDataCache`
pushes CPU caches *toward* the GPU and waits for nothing; there was no fence anywhere on that path.

Three ways a live block was clobbered:

- **The free list was LIFO.** `takeFree` took `list.back()`, so the block a section gave up when it
  was invalidated was the first one handed to the next upload of that size -- the section got its own
  in-flight block back and `memcpy`'d over it.
- **The eviction guard was one frame short.** It refused slots with `frame == frame_`, which protects
  the frame being *recorded*; the frame being *executed* is the one before it. A section drawn last
  frame and dropped from this frame's draw list -- which is what turning the camera produces -- was a
  legal eviction target mid-draw.
- **`rebuildChunks` and `setAtlas` freed everything with a frame in flight.** Both are reached from
  the settings page while the loop is running.

Fixed by retiring blocks over frames: `VboPool::kRetireFrames = 2`, tested against
`slot.frame`, which `lruPushBack` already maintained -- no new state was needed. A slot last drawn in
frame N is referenced by frame N's list, which is in flight for the whole of CPU frame N+1, and is
free at N+2. `C3D_FrameSync()` covers the two wholesale teardowns. **The pool now needs one block of
headroom over the drawn set**, because at any moment one block is in that limbo; that is a real cost
and `tests/vbo_pool_test.cpp` states it.

Two more edges found while doing it, both able to draw one chunk's geometry where another chunk is:

- The draw list is snapshotted at `beginFrame` and `drainEvictions` never patched it, so a slot that
  changed hands later in the frame was still drawn -- with the *old* section's model matrix. Slots
  carry a generation now and `drawPass` skips a mismatch.
- `kMaxQuadsPerSection` is derived from the checkerboard, which bounds a pass whose faces are culled
  against their neighbours. **The detail pass has no such derivation** -- a torch is five quads
  whatever is beside it -- and the only guard was an `assert`, compiled out in Release, which is the
  only build where reading past the 144 KB index array does harm. The mesher stops emitting at the
  bound now and `drawPass` clamps as well.

#### The crash: the tick recursed with no bound, on a 32 KB stack

`writeBlock` calls `blockAdded`/`blockRemoved`, behaviours call `notifyNeighbours`, and that
dispatches `neighbourChanged` synchronously -- which can write another block. Nothing counted the
depth. A fire field going out unwinds as one recursion as deep as the field is wide, and **lava
starts fires**, which is the reported scenario. `crashlogs/001-rungame-stack-overflow` is what this
class of failure looks like here.

Redstone had a guard of its own, `kPropagateDepth = 64`, and it did not work: `wireNeighbourChanged`
restarted the count at zero on every hop through `notifyNeighbours`, so 64 bounded one straight run
of wire and not the cascade.

The bound is global now, shared by every path that can recurse, and **counted in bytes of stack
rather than in levels**, because the levels are not the same size. Measured with `-fstack-usage` on
the devkitARM build at -O2:

| Cycle | Frames | Bytes per level |
|---|---|---|
| Notify cascade | notifyNeighbours 32 + neighbourChanged 56 + behaviour ~24 + setBlockWithNotify 48 + writeBlock 72 + blockRemoved 8 | **240** |
| Redstone wire | wirePropagate 16 + wirePropagateInner 72 | **88** |

`TickWorld::kCascadeStackBudget` is 8 KB -- a quarter of the stack, leaving the rest for whatever the
tick was called from. That allows 93 nested wire levels against the 16 the original's own comment
calls reachable, and 34 nested notify levels, so nothing a real world does comes near it. Past the
budget a notification is **deferred to a bounded queue and run before the tick ends**, not dropped,
so the work still completes and only its order past that depth differs.

#### The frame drops: three costs, one of them quadratic

- **`tickDirty_` was unbounded and scanned linearly for every changed block.** Its comment said the
  set was "what one frame's ticks touched"; it was only cleared at the autosave boundary, so it
  accumulated every column touched since the last save -- and with autosave set to **Off** it was
  never cleared at all. A flowing fluid changes hundreds of blocks a tick, so the scan got
  monotonically worse the longer a session ran. It is a flag on the grid cell now: O(1), no
  allocation, bounded by the grid.
- **`pump()` walked the whole cache table under the lock, every frame,** for two numbers on a debug
  page. The counters are maintained incrementally at the five places an entry gains, loses or changes
  the dirtiness of its column, and `stats()` answers live rather than as of the last pump.
  `ChunkCache::debugCountColumns` re-derives them the slow way and a host test keeps the two honest.
- **The random tick decoded a palette entry at a random offset for every sample.** a1.1.2 does 80 per
  chunk per tick over a 19x19 square, so at render distance 8 that is **28,880 reads a tick and
  577,600 a second** -- and `TickTimer` allows ten whole ticks in one frame after any stall, which is
  a hitch that causes the next hitch. `section.hpp` already recorded that this access path measured
  at 46 % of the time to mesh a section. `Section::mayTickRandomly()` answers from the palette
  without touching the index array, and the sample loop rejects whole sections against it. **The
  random sequence is untouched** -- `nextLcg()` is still called exactly eighty times per column -- so
  the tick's output is byte-identical and the existing vectors are the proof.

Also on the frame path and now off it: `TickScheduler::indexRebuild` allocated and freed a 128 KB
table inside `runScheduled`, roughly every 2,048 pops, which an active fluid reaches routinely.

`FrameTiming` has a `tickMs` bucket now. It used to be inside `streamMs` along with `update()`,
`sound.tick()` and `tickSaves()` -- four things under one number, and no way to tell from a console
which of them a frame went into.

#### Found while looking: tick edits were silently lost

`dropCell` hands a departing column to `cache_.give()`, which installs it as **clean**, and once the
cell is gone `flushTickDirty` cannot find it -- it looks the column up in the grid. So a column a
tick wrote into and the player then walked away from was never written to the card, and with autosave
Off that was every edit of the session. `give()` also keeps whatever the cache already holds on the
grounds that it is "at least as fresh", which stops being true the moment the resident copy carries
edits the cached one does not, so the edited column was discarded outright rather than merely
unsaved. `dropCell` saves first now.

Related, and the invariant this broke: `ChunkCache::save`'s over-cap back-pressure loop deflates and
writes a column **on the calling thread**, and the comment above it said "it is never the main thread
today and must not become it -- an edit path reaching here would want to give up frame budget
instead." `flushTickDirty` was exactly that edit path. `SavePressure::Defer` makes the choice
explicit rather than a comment, and the main thread now queues the work instead.

#### The lava: there was no runtime lighting at all

`LightEngine` solves a whole column against a 3x3 window and ran **once**, on the generation worker,
when a column was first finalised. Nothing recomputed light after that. `TickWorld::refreshHeight`
maintained the height map, which is why `canBlockSeeTheSky` stayed right while the light did not --
and `TickWorld::lightValue`, which grass, crops, saplings, ice and the snow pass all read, kept
answering with values that no longer described the world. It was not lava-specific; lava is just
where it is obvious.

`world::LightUpdater` is the incremental half: the standard removal-then-addition pair over the cells
a change actually disturbed, budgeted per frame, with bounded queues. Re-running `LightEngine` per
edit was the alternative and is not affordable -- 576 KB and 294,912 cells against a fluid that
changes hundreds of blocks a second.

**Why it is allowed to work differently from the engine**, and this is the whole argument:
`lighting.hpp` already establishes that a1.1.2's update rule is a monotone operator with a strictly
positive decrement, so it has exactly one fixed point and every algorithm that finds it finds the
same numbers. `tests/light_update_test.cpp` does not take that on faith -- it lights a 3x3 with the
engine, applies an edit, lets the updater settle, re-lights from scratch and compares all 32,768
cells of each plane. It needs no JVM of its own, because `LightEngine::computeCentre` is already
compared nibble by nibble against a real a1.1.2 `World` by `tests/light_test.cpp`.

The two engines now share `clampedOpacity` and `emittedLight`, hoisted out of `LightEngine` into free
functions, including the `def.known` carve-out: the block table gives unknown ids an opaque cube on
purpose, and the jar's `lightOpacity` array leaves them transparent. Two engines disagreeing there
would disagree everywhere an unknown id appears.

**Light is baked into vertices**, so every cell whose stored light moves is a remesh. That is why the
relighter had to land after the flash fix rather than before it.

#### The flash: "needs remeshing" was encoded as "has no mesh"

`invalidateSection` handed the section's VBO block back and set its slot to `kNoMesh`. The draw list
is built at the top of the frame and the remesh runs after it, so the replacement geometry could not
reach a draw list until the frame after next: one frame of hole at best, and while a fluid is flowing
the 4-per-frame mesh budget never catches up, so it is a hole that stays. `main.cpp`'s comment
claimed the tick was placed before the draw so a changed block reached the mesher in the same frame;
it never did, and re-ordering could not have fixed it, because the draw list is fixed before any of
it.

A section is marked **dirty** now and keeps its slot. It goes into *both* lists -- drawn from
one-tick-stale geometry, and queued to be replaced -- and `uploadSection` uploads before it releases,
so a pool too full to place the replacement leaves the old mesh on screen instead of a hole. Falling
behind costs latency rather than a hole. Re-meshes are also ordered ahead of first meshes in the
queue, so an edit twenty metres away does not queue behind every unmeshed section nearer the camera.

#### Two follow-ups from the first hardware run, and one of them was mine

The console reported the flicker **still there** and lava pools causing "a loop of the chunk
refreshing". The block tick and the relighter were both cleared by host repro first -- a still lava
pool settles in one tick and a flowing one in under sixty, and a relight that lands on the values
already stored reports no section at all (`tests/light_update_test.cpp`). The fault was on the render
side, and it was introduced by the fix above.

**The staleness guard put the flash back.** `drawPass` refuses to draw a section whose recorded slot
generation no longer matches -- which is right, and stops one chunk being drawn with another's model
matrix. But `uploadSection` gives the old block back the moment the replacement is in hand, and that
bumps its generation. The draw list, built before the frame's meshing, still names the old block. So
on *every* remesh the guard correctly refused to draw an entry it no longer recognised, and the
section vanished for that frame: the same one-frame hole, arriving by a different route, and
continuous wherever something ticks constantly.

The fix is to repoint the list rather than skip it: a successful upload rewrites that section's entry
to the new slot and generation. The section is then drawn *this* frame from the *new* geometry --
a frame earlier than the stale-draw behaviour managed -- and the guard keeps doing its real job for
slots that genuinely changed hands. `a_remesh_leaves_the_section_drawable_in_the_same_frame` fails
without it and passes with it.

**And a leak of my own making.** `SavePressure::Defer` took the main thread off the write path, which
was the point -- but it removed the *only* back-pressure on the dirty set, and a dirty column cannot
be evicted because it is the only copy of that world. Relighting then made columns dirty far more
often, because every section whose light moves has to be saved. That is the same unbounded set the
cache already had once (11.28 MB against a 4 MB cap), reintroduced from the other end. Past a ceiling
of twice the cap the deferred path now writes one column itself: a hitch is worse than a clean frame
and much better than running the console out of heap. See `ChunkCache::deferCeilingLocked` and
`crashlogs/005-lava-flicker-session/NOTES.md`.

#### What the retirement rule costs, measured

The obvious worry about holding a block for two frames is that the pool loses capacity. It does not,
in the case that matters. `--mesh` over a copy of the reference world, three full turns on the spot,
after the change:

| Configuration | Peak resident | Uploads | Recycled | Evictions | **Refused** |
|---|---|---|---|---|---|
| o3DS d6, 12 MB | 8.40 MB | 417 | 0 | 0 | **0** |
| o3DS d8, 12 MB | 11.16 MB | 2,290 | 1,638 | 1,794 | **0** |
| n3DS d8, 32 MB | 15.52 MB | 722 | 0 | 0 | **0** |
| n3DS d10, 32 MB | 29.69 MB | 1,332 | 0 | 0 | **0** |

The o3DS distance-8 row is the one to read: the pool is full, it is evicting hard, 71 % of uploads
are served from a recycled block -- and nothing is refused. The two-frame hold costs one block, and
the size-class slack already carries more than that.

`--fly` over the same world at distance 6, 600 frames: settled at frame 122, 225 columns resident,
550 sections meshed, 0 evictions, 0 refused, and **0 main-thread SD checks**. The tick and the
relighter ran on every one of those frames.

#### What is measured and what is not

Everything above is derived from the source or measured on the host; **the ARM stack figures are
measured, from `-fstack-usage` on the devkitARM build.** What is *not* measured yet, and needs a
console:

- `kLightBudgetPerFrame = 1024` is an estimate sized to sit under a millisecond at a pessimistic
  microsecond per cell. The debug page carries `light pend / lit / drop` so the real figure replaces
  it rather than being guessed at twice.
- What the pool's one-block retirement headroom costs in refused uploads. `heldInFlight` is on the
  Info page.
- Whether the random-tick rejection is enough on its own, or whether the ten-tick catch-up burst
  still needs spreading across frames. `tickMs` is the number that answers it.

## Open questions

None outstanding. Both of M2's are answered below.

### Answered

**Grass and foliage tint** — a1.1.2 has **none**, and the mesher writing face shade with no tint was
already right. Three independent checks from the jar, all agreeing:

- `Block.colorMultiplier(world, x, y, z)` is `ly.d(Lnm;III)I` and its whole body is
  `ldc 16777215; ireturn` — plain white. **No subclass overrides it**, checked across all 402
  classes, so every block in the game multiplies by 0xFFFFFF.
- There is no `misc/grasscolor.png` or `misc/foliagecolor.png` in the jar. The colour-map sampling
  that question was about arrived later.
- `terrain.png`'s grass and leaf tiles are already green (average RGB 117,176,73 and 59,191,40
  against stone's neutral 125,125,125), not the greyscale masks a tinting renderer needs.

`RenderBlocks` does still carry the multiplier through — `k(block,x,y,z)` splits the int into three
floats and hands them to `a(block,x,y,z,r,g,b)` — so the *mechanism* exists in a1.1.2 and does
nothing. That is why the vertex keeps its three colour bytes: later versions turn the same path on,
and the field is already there for them.

Consequence: **there is nothing to fold into the atlas and no `tint` byte to define.** The vertex's
r,g,b carry face shade today and will carry face shade x AO when smooth lighting lands.

Note for whoever builds the asset pipeline: those tile averages are stated here as evidence about
a1.1.2's rendering, not as a palette to copy. The Dev Art colours in
`core/texture/dev_art.cpp` were chosen independently and must stay that way — see the licensing
rules in [assets.md](assets.md).

**PICA clip range** — settled by disassembling citro3d rather than by testing on hardware.
`Mtx_Persp` and `Mtx_PerspTilt` both write, for a right-handed projection:

```
M[2][2] = near / (near - far)      M[2][3] = far * near / (near - far)      M[3][2] = -1
```

which puts the **near plane at z/w = -1 and the far plane at z/w = 0**. That is neither OpenGL's
range nor Direct3D's, so `ClipRange` grew a third entry, `NegativeOneToZero`, and the failure modes
are not symmetric: reading a citro3d matrix as `ZeroToOne` takes `M[2]` alone as the near plane,
which is the far plane negated and culls the entire world; reading it as `NegativeOneToOne` gets the
near plane right and leaves a far plane that never rejects anything. `tests/frustum_test.cpp`
reconstructs citro3d's matrix from the disassembly and pins all three readings.

---

## Standing constraints

- **Never ship Mojang assets** — textures, sounds, fonts, jar contents. The bundled fallback pack
  must be CC-licensed and attributed in `romfs/licenses.txt`.
- **ViaLegacy is GPLv3: documentation only.** Protocol IDs and wire sizes are facts about a 2010
  protocol; its code is never copied.
- **craftus_reloaded is MIT** — reusable with attribution.
- **Jar extraction is a maintainer step**, never a player step and never part of the build. Its
  output is checked in.
- **The real world is worked on through copies only.**
