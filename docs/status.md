# Project status and handoff

The authoritative "where we are" document. Everything here is either a decision that has been made,
a number that has been measured, or a task that is next. Estimates and guesses are labelled as such.

Update this file when a milestone moves.

Entity persistence update (2026-09-09): the six persistent entity pools now save and reload
through `core/entity/persistence.{hpp,cpp}` and the existing streamer/cache save queue.
The versioned `Data/3DAlphaEntities` compound carries position, motion and simulation state
in both folder and packed worlds. Autosave, pause and close capture immutable snapshots;
empty pools overwrite previous snapshots. Falling blocks wait for resident chunks after load.
Native Java chunk `Entities` are still opaque and preserved; this extension does not import
or export them. **Tile entities are a separate thing and are no longer pending**: `TileEntities`
is modelled, decoded and re-encoded, so sign text and a spawner's mob persist through the chunk
file itself -- see 39. Restoring vehicle rider attachment remains pending.
Validation: 1059/1059 host tests pass with ASan/UBSan; the four persistence tests and
two housekeeping tests pass with TSan. The 3DSX cross-build succeeds; no hardware run yet.

**Start with root `AGENTS.md`.** `docs/task-map.md` routes tasks to code and tests;
`docs/current-work.md` holds a short, dated handoff. This file keeps the detailed historical
record. Search `docs/doc-index.md` for the relevant section and read its line range rather
than opening this document whole. Longer working instructions are in `docs/working-guide.md`.

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
| **M2** Renderer | **in progress; the gate failed and the answer to it is built but unrun** — the whole pipeline exists and runs end to end on hardware. Six launches that ran: a stack overflow, a VRAM write, a wrong daylight curve, fog/depth/frame-time, black torches, and the profile below. **Two more did not launch at all, and neither was a bug in the build** — `loader` refused the file on the SD card both times, which looks exactly like a crash; see §1. **The 12-byte/4-vertex path costs 0.208 µs per quad and misses the M2 gate by 3.2× at distance 10, and by an estimated 2.1× at the distance 8 the New 3DS gate has been lowered to.** The geometry-shader path §2 always pointed at now exists, is measured on the host, **draws the whole render distance on hardware without stalling, and is what the game boots into as of this change** — the 4-vertex path stays as the watchdog's fallback and as the only encoding that can ever carry per-corner light. What is still *not* taken is the gate measurement itself: GPU draw time per quad, both formats, one session, one position |
| M3 Singleplayer gameplay | not started as *gameplay*, **but its foundation is now built: the world ticks.** a1.1.2's 20 Hz clock (`ir.class`, accumulator, partial ticks, the ten-tick cap that drops rather than defers), its scheduled-update list with the original's ordering and both of its limits, its 80-samples-a-chunk random tick, and fifteen block behaviours -- grass, leaves, saplings, crops, farmland, flowers, mushrooms, sugar cane, cactus, ice, both snows, torches, sand and gravel. Blocks are dispatched by a **tick behaviour**, never by id, the way the renderer dispatches on render type. Redstone, fire and the fluids are named and not done; see §0o and [tick-a1.1.2.md](tick-a1.1.2.md). Also **the main menu and the pause menu, which are built** — title, world list, create-a-world with a typed seed, delete, and an options screen for render distance. The game now starts from it rather than opening whatever `readdir` returned first; see §0b. **START pauses instead of exiting**: Resume, World Settings, Options, Exit World, over a world that stays open and stops dead while the menu is up. **World Settings is the world's own screen, as against Options, which is the console's** -- Gamemode, Format, Size, Copy, Delete, reached from the pause menu and from `X` on the world list, and cut to Gamemode alone when a world is open behind it. Gamemode is real: it lives in `<world>/3dalpha.ini`, a file of ours that a real Alpha client never reads, because a1.1.2 has no gamemode key for `level.dat` and a per-world value has no business in `3ds.ini` either. **Spectator and Creative are both implemented; Survival is the one drawn disabled.** Spectator is free flight with no body and is offered under a name that says so; Creative landed at M3 step 3 with a body that collides, a four-block reach, break and place, a nine-slot hotbar on the bottom screen, a block palette page fed from the registry, and flight that collides -- **and a1.1.2 has no Creative mode at all**, so all of it is invented and none of it has an oracle. See section 0s. Survival stays disabled because everything it adds -- damage, hardness, drops, depletion -- is a rule on top of the same body and does not exist. **Worlds have a storage format now**, Folder or Packed, converted losslessly from that screen; new worlds are packed. See §0j. Built and linked; not yet seen on hardware. It is the same `Menu` object, so Options and Texture Pack in a world are the ones the title screen uses and both apply live; see §0d. **It is transparent**: the world is redrawn behind it every frame and dimmed with a1.1.2's own gradient, because the menu now draws into the renderer's frame rather than into a target of its own. **Opening it costs a frame** -- the two card listings that used to run on every `Menu::init` are asked for by the screens that show them instead. **Opening it also saves**, which is more than the original does -- a1.1.2 only writes everything out on Save and quit to title -- and only when a column is dirty, so pausing twice costs one save. Leaving a world counts the drain out as a percentage. Options has an autosave row beside render distance and texture pack. **The bottom screen is now a tabbed HUD, and the debug pages are behind it rather than beside it.** The player's half is three pages switched by touching tabs along the **bottom** -- **Map**, **Items** and **Look** -- drawn as panels, slots and bevels in a1.1.2's own GUI colours over the pack's tiled dirt, while the three debug pages stay shared, unchanged and behind `SELECT + Y`. It is still libctru's text console underneath: the furniture is written straight into the RGB565 framebuffer and the console's glyphs are printed on top of it, on backgrounds set per row with `\x1b[48;2;R;G;Bm`, which is what stopped text punching black rectangles through the panels. **Map** is 208 by 200 blocks at one pixel per block, centred on the player, drawn from the columns the streamer already holds -- **in every gamemode now, not only Spectator's** -- and it shows **coordinates and nothing else**: the chunk, the map tile, the chunk count and the redraw time were the maintainer's questions and have moved to the Info page, and the debug grids to the settings page. **There is no strip of button hints along the bottom**, because it was the same three lines on every page spending a twelfth of the screen to repeat itself; the debug pages keep theirs, which is where `SELECT + Y` is worth naming. Those 24 rows and 24 columns off the coordinate panel are what made the window a quarter bigger than the 192 by 176 it started at -- an estimated 790 us a redraw against 700, measured on the host as 16.9 us against 14.2. The marker is an **arrowhead rotated to a real yaw** rather than a diamond with a whole-block tick, which is what fixes both of the old one's faults at once -- it pointed eight ways and it was 41 % longer on a diagonal than on a straight. **Items** is the inventory, nine across and three rows with **the four armour slots down its left**, filling the whole page band and not offered in Spectator -- it lost its fourth row, because **the hotbar is a band of its own across the *top* of every player page** in every mode that has one, edge to edge, with slots 35 and 36 pixels wide so that nine of them land exactly on both screen edges. See §11. Creative adds a fourth tab, **Blocks**, which is the block palette and is deliberately not the inventory: a catalogue held by nobody against what a player is carrying. The map lost 32 pixels to that band and got cheaper for it, 208x168 rather than 208x200. **Look** is a pad that hands the drag to the camera, with a compass ribbon over it -- which exists because the bottom screen is both the UI and the only pointing device an old 3DS has, and dragging on a map used to turn the view. **Run on hardware, where a redraw read 5,000 us**; a per-chunk patch cache took that to a copy, and the number is on the Info page. **The HUD itself has not been seen on hardware.** See [map.md](map.md) |
| **M4** a1.1.2 worldgen, seed-exact | **done, wired, and on a worker thread.** Terrain, caves, **the Far Lands**, lighting, the whole population pass, **and `ft`, the chunk provider above them all** match a real a1.1.2 World byte for byte, reflected under a real JVM by `tools/genref.java`. `ChunkGenerator` turns "there is no chunk here" into a finished, populated, lit column, and `WorldStreamer` now asks it for one and writes what comes back — so the game makes world where there is none, which is what an Alpha world does. **Generation runs on its own thread**, below the render thread, so making ground costs latency rather than frame rate — and the world it produces is byte-identical to the one generating inline produces, which is a test rather than a hope. `--fly <empty-dir> 8 2000 gen` creates a world, generates it, meshes it and saves it under sanitizers. It found a real bug in `WorldGenBigTree` that no per-generator test could. See [worldgen-a1.1.2.md](worldgen-a1.1.2.md). **Run on hardware now, and the cost is exactly what was predicted**: generation is slow and a walking player outruns it and never sees it catch up. The cause was not the generator but the thread it was on — `std::thread` had put it on core 0 at the bottom priority, where it ran on scraps. It is on **core 2** on a New 3DS now; see §0 |
| M5 Multiplayer (protocol 2) | not started |
| M6 Audio, mobs, texture-pack browser, packaging | **the texture-pack browser is done and run on hardware, and background music is built but unheard**, both ahead of the rest of M6; mobs and packaging not started. **Music is a1.1.2's `of.c()` transcribed exactly** -- the counter seeded at `nextInt(12000)` and reset to `nextInt(24000)+24000`, and, the part that is easy to lose, *not decremented while a track is playing*, so the period is the track's own length plus 20-40 minutes. It runs on the same `elapsedTicks()` the world does, beside `stepTicks`. Underneath it: `audio::Backend` (the `IAudio` docs/architecture.md always named), an ndsp voice on channel 0 fed by a ring of eight 1024-frame wave buffers in linear memory, and a third `WorkerRole` -- `Audio` -- decoding Ogg Vorbis through Tremor. **On an Old 3DS that decode is on core 0**, because a 3DSX has no other core; it sits below the main thread and the 186 ms ring is what makes that safe, which is a claim only hardware can settle -- the overlay counts underruns and decode microseconds for exactly that reason. Options -> Sound carries the volumes and the one line that says why a console is silent, distinguishing a missing DSP firmware from a DSP something else is holding. **Sound effects are deliberately absent**: nothing in the port can emit one yet, so the machinery arrives with its first caller rather than as dead code. See [audio-a1.1.2.md](audio-a1.1.2.md). Options -> Texture Pack lists the packs on the card and applies one; Extract from a jar turns a player's own `minecraft.jar` into a pack and offers to delete the jar afterwards; the generated art is now "Dev Art", one pack among them. **Three of a pack's files have consumers now**: `terrain.png` is the block atlas, and `default.png` and `dirt.png` are the menu -- the font every label is drawn with and the backdrop behind them, both a1.1.2's own rules read out of the jar, both optional and both with a fallback that needs no file. A pack's gui, mob and item textures are still carried and counted and unread. **The menu art is built and not yet seen on hardware.** See §0c and [assets.md](assets.md) |

**946 tests pass** under ASan/UBSan/float-cast-overflow, at `-O3`, and the 3DS target links clean.
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

  **This was written when the list round-tripped as an opaque blob, and that is no longer how it
  works** -- see 39. `TileEntities` is decoded and re-encoded now, which is what let sign text and
  a spawner's mob persist; the save is still semantically 1:1, and `--rewrite` plus
  `tools/nbtdiff.py difftree` over a copy of the real world is the check that says so.
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

> **The one real out-of-memory report, kept verbatim.** `free 4405k of 40960k  blocks 10688k
> owed 1582k  pool 13425k  cols 382  sect 1314  chunk -63 -2000007  geoshader`. Resident columns are
> **29 %** of the heap in use; the rest is the chunk cache, the generator's cache and overhead, and
> is **not measured**. 10688k over 382 columns is 28.0 KB a column -- against the 21.7 KB below,
> because these have been lit and ticked and `NibbleArray::set` never re-collapses a plane.
>
> **A resident column costs 14.0 KB over ordinary terrain and 21.7 KB at the Far Lands.** Measured
> through the real generator, 169 columns each at chunk (0,0) and at chunk (784426,0), by plane:
>
> | | ordinary | far lands |
> |---|---|---|
> | **per column** | **14.0 KB** | **21.7 KB** |
> | blocks | 9.0 KB | 12.8 KB |
> | sky light | 2.8 KB | 6.6 KB |
> | block light | 1.1 KB | 1.2 KB |
> | metadata | 0.0 KB | 0.0 KB |
> | non-uniform sections | 763/1352 | 1078/1352 |
>
> **1.55x, and the reason is uniform sections, not the palette.** Ordinary terrain is all-air above
> the surface and all-stone below, and a uniform section costs literally nothing; the Far Lands has
> far fewer of them. `crashlogs/006` guessed "several times" and "the palette buys nothing", and
> both are wrong -- the palette is working, there is simply more that is not uniform.
>
> So the grid costs, at `(2d + 1)^2` columns:
>
> | render distance | columns | ordinary | far lands |
> |---|---|---|---|
> | 12 | 729 | 10.0 MB | 15.5 MB |
> | 16 | 1089 | 14.9 MB | 23.1 MB |
> | 20 | 1681 | 23.0 MB | 35.6 MB |
> | 24 | 2401 | 32.8 MB | **50.9 MB** |
>
> **Two obvious savings were measured and both are dead.** `compact()` recovers **0.0%** -- nothing
> in the game calls it, which looked like a lead, but the generator already uses
> `NibbleArray::assign()`, which collapses to uniform on load, and metadata is never materialised at
> all (0 of 1352 sections). A narrower palette tier recovers **2%**: the distinct-id histogram peaks
> at 5-8 ids per section and only 36 of 1352 Far Lands sections hold four or fewer, so 1- and 2-bit
> encodings take 12.8 KB to 12.5 KB. **The representation is close to optimal; the room was in the
> heap split**, see §*The heap split*.


> **Audio costs about 99 KB of binary and nothing else measured yet.** Linking Tremor takes `.text`
> from 762,772 to 809,852 and `.rodata` from 68,916 to 120,540 -- a1.1.2 Release build, `-O3`.
> The `.3dsx` goes 861,252 -> 960,224 bytes. **Every other audio number in this document is a
> prediction, not a measurement**: decode cost per buffer, underruns and whether the Old 3DS thread
> policy holds are all waiting on a console, and the debug Info page exists to answer them in one
> launch. See §0p.
>
> **The menu click is measured on the host, because it can be.** `newsound/random/click.ogg` from the
> real a1.1.2 resources folder decodes to **12,332 frames, 2 channels, 44,100 Hz -- 280 ms, about
> 49 KB of PCM**, taken once at boot as a single `linearAlloc`. Gains are **0.250** for a choice
> (`random.click` 1.0/1.0) and **0.075** for a cursor move (0.3/0.5). `--audio-list <resources>`
> prints all four numbers.
>
> **Adding one-shot effects cost the binary 18.7 KB**: the `.3dsx` goes 960,224 -> 978,936 bytes
> and the loader allocates 254 pages against 249. `.text` 809,852 -> 828,108, `.rodata` 120,540 ->
> 122,344. That is the sample path, the four voices and the menu call sites; the samples themselves
> are linear memory at runtime and not in the image.

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

> **The visibility walk made about twenty software divisions per section it visited, and now makes
> none.** ARMv6k has no divide instruction, so every `%` was an `__aeabi_idivmod` call; counted out
> of the shipped object, `visible_set.o` held **34 call sites** and holds **6**. All six are now
> once a frame or once a render distance (`setCentre`, `reset`, and the camera's seed index, which
> keeps its `floorMod` because the camera is not guaranteed to be inside the field). The walk also
> resolves each column **once** instead of asking four accessors that each resolved it again, and
> stamps its visited array instead of clearing `edge² × 8` bytes every frame.
>
> **Behaviour-preserving, and checked rather than asserted**: `--fly` over the real world at
> distance 8 for 400 frames is identical frame for frame, and `--mesh`'s tables are unchanged to the
> section — distance 8 reaches 1,525 of 2,312 and draws 309–554 (mean 423). Host suite 684/684.
> **This removes no quads and cannot move the M2 gate**; what it frees is core 0, which the walk
> shares with meshing, the tick, the relighter and streaming. See `docs/3ds-performance.md` §4.
>
> **The hardware number has not been taken.** `walkMs` is on the debug overlay and the before/after
> at New 3DS distance 8 with 3D on, on the surface and underground, is what would settle it.

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
| `setBlockBoundsForItemRender` | `ly.e()V`, empty on Block. Overridden by **two** classes in a1.1.2: `hu` (the button) and `al` (both pressure plates). It is what `renderBlockAsItem` draws, so it is the hand's and the slot's shape and not the world's — see `kItemRenderBoxes` |
| `tickRate` vs `getRenderType` | `ly.a()I` is tickRate (10 by default; `hu` and `al` return 20) and `ly.f()I` is getRenderType. A button overrides the first and not the second, so it is render type **0** — a standard block drawn from its own bounds |
| `canPlaceBlockOnSide` | **does not exist in a1.1.2.** `av.a(...)` — ItemBlock.onItemUse — calls `cn.a(IIIIZ)Z`, canBlockBePlacedAt, with no side; the side arrives only in `onBlockPlaced`. So a button may be placed at metadata 0 on a face that cannot hold it |
| `getBoundingBox` | `kh.f_()Lcf;`, and it is what makes an entity solid: `cn.a(Lkh;Lcf;)` adds whatever it returns to the list every `moveEntity` clips against. **`kh`'s own returns null and exactly two classes override it** — `dc` (EntityBoat) and `oc` (EntityMinecart), each with `return this.boundingBox`. Checked across all 402 classes. So "you can stand on it" is true of a boat and a cart and of nothing else in this version |
| `getCollisionBox` | `kh.b_(Lkh;)Lcf;`, null on `kh` and overridden by the same two classes, which answer with the **argument's** box — `return e.boundingBox`, with no liveness or `canBeCollidedWith` test. `cn.a(Lkh;Lcf;)` calls it on the *moving* entity, so a cart or boat collides with everything near it while a player reaches only the `f_()` branch |
| Entity candidates | `ga.a(Lkh;Lcf;Ljava/util/List;)V` filters on `e != excluded && e.boundingBox.intersectsWith(box)` and nothing else — no liveness, no rider, no kind. A vehicle's own rider is in its list and costs it nothing, because `oc.h()`/`dc.h()` (getMountedYOffset) are `height * 0.0 - 0.3` and an overlapping box never clips |
| Particles are not in it | `bq.a(Lnq;)V` is `lists[particle.getFXLayer()].add(particle)`: `nq` (EntityFX) goes into the effect renderer's own `List[]`, never into the world's entity list, so nothing collides with a particle |
| `canBeCollidedWith` / `canBePushed` | `kh.d_()Z` and `kh.c_()Z`, both false on `kh`. The boat and the cart return true and `!isDead` respectively — these are the crosshair's pick and `applyEntityCollision`, and neither is what holds a player up |
| Dismounting | `kh.g(Lkh;)V` — mountEntity, called with the vehicle already ridden. Its tail is `setLocationAndAngles(vehicle.posX, vehicle.boundingBox.minY + vehicle.height, vehicle.posZ, ...)`, and `c(DDDFF)` stores `posY = y + yOffset` — so **the rider's feet land on the vehicle's roof**, not in its seat |

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
  the 12-byte vertex has no flip bit. (2026-09-11: doors now mirror. `addDoor` ports
  `fw.a(II)I` per face and `addBox` takes a mirror mask; the detail stream carries plain UVs.)

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

> **Superseded by section 0s.** Every mode has the map now, the hotbar and the palette exist rather
> than being promised, and the screen is three bands rather than two -- the hotbar has the bottom 32
> pixels in every mode, which is why the map below is 208x168 and not the 192x192 or the 208x200
> this section describes. What survives unchanged is the split this section is about: the player's
> half is the gamemode's and the debug pages are everyone's.

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

**And it found it again, twice over.** The 700 µs prediction was never checked directly, and when
zoom landed a console read **3,283 µs** — which turned out not to be about zoom at all. Most of what
was left after the patch cache was `MapStore::patch` hash lookups (2,704 of them per redraw, for 182
distinct answers) and a stale-scan running on frames where nothing could be stale. Both are fixed;
the host copy went 389 → 86 µs at 1:1. **See §0r**, which has the table and the console figure that
still has to be taken.

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
`kCommandBufferBytes` is 4x the default, and `drawPass` checks the real headroom before each
section.

> **The first version of that check split the list, and a split reclaims nothing.** Read out of
> libctru's disassembly rather than assumed, `GPUCMD_Split` does `gpuCmdBuf += offset;
> gpuCmdBufSize -= offset; gpuCmdBufOffset = 0`. Free space is `size - offset` before it and
> `(size - offset) - 0` after it -- **the same number**. A split hands the recorded words to the GX
> queue and carries on recording *in the space that was left*, so the guard flushed a list, bought
> nothing, and drew the section anyway: exactly the overrun it was written to prevent. The budget
> is per frame and only `C3D_FrameBegin` refills it. `drawPass` now stops drawing when the room is
> gone -- holes in the world, far ones first, `droppedSections` on the Info page -- and the page
> reports `cmd` as a percentage of the budget rather than a split count. See
> `docs/3ds-performance.md` section 2.

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
| circle pad | move. Focused **on the Map page**, it scrolls the map instead and the body stands still — panning and walking at once would be two things fighting over one window |
| C-stick | look — through `ir:rst`, not `hid`, so a Circle Pad Pro on an **old** 3DS gets it too |
| touch drag | look. **Yaw was inverted**: the view turned the opposite way from the finger. Both axes now follow the mouse convention, drag right look right, and the reason it is `+=` is that `Camera::look` sends yaw 0 to +Z and positive yaw toward −X — south turning to west, which is right |
| B | Spectator: up. Otherwise **jump** |
| Y | Spectator: down. Creative flight: down. Riding: dismount. Otherwise **toggles sneak**, which walks the move back at a ledge, silences the footsteps, drops the camera 0.08 and cuts the stick to three tenths. **A toggle rather than a held button**: what a player sneaks for is edging over a drop to build under themselves, and that is a thumb on the pad and a finger on a shoulder already. The three uses above keep the button and also stand the player up, so a crouch cannot outlive the button that ends it |
| R / L | **break and place** — R breaks, L places, repeating every five ticks. **The opposite way round from a mouse, and deliberately so**: on a 3DS the shoulders are triggers rather than a left and a right hand, R is under the finger that also holds the console up, and breaking is what a player does most often — M3 §2. Focused (see X), they **change tab** instead — Map, Items, Blocks — and the focus survives the change; break and place are suspended for as long as the focus is on, so one press does one thing |
| ZL / ZR | **change the held hotbar slot**, wrapping. New 3DS only, which is why the focused d-pad below does the same job |
| X | Spectator: sprint, unless SELECT is held. Otherwise **focus the bottom screen** — the d-pad drives a cursor over the palette and the hotbar and A picks, while the camera keeps working. A dark banner under the tab strip says so and says what the buttons mean on that page. It exists because the screen is resistive and a player walking has no stylus out |
| B B (double tap) | Creative: **toggle flight**. A quarter of a second, timed in frames because it is a gesture and not physics |
| forward forward (double tap) | Creative: **sprint**, until the stick comes back. **Creative only** — a1.1.2 has no sprint and Survival here is meant to *be* a1.1.2. Not a1.1.2's — that jar has no sprint at all — but the console editions' gesture, and the speed is Beta's `landMovementFactor * 1.3` applied where Beta applies it. Two thresholds rather than one, so a pad resting near the edge cannot chatter itself into a sprint. See `core/entity/sprint_gesture.hpp` |
| A | focused: pick. Creative and flying: **fly faster**. **X is Spectator's sprint and A is Creative's**, which is not an inconsistency to tidy: X is the bottom screen's focus in every mode that has a hotbar, so it cannot also be a held modifier, and A is the focused screen's pick button and does nothing unfocused |
| SELECT + d-pad | tune the 3D. **Was Y + d-pad**, which Y being sneak took away; SELECT already claimed only Y and X for the page cycle, so the d-pad under it was free. The map page's d-pad had a matching Y guard, removed with it — left in, the map's zoom would have died whenever the player crouched |
| SELECT + Y / X | cycle the bottom screen forward / back |

**The crouch is a toggle, and its camera drop came from 1.8.9 because nothing earlier has one.**
a1.1.2's `Entity.isSneaking` is a hardcoded `false`, so the jar has the walk-back and no answer at
all for what a crouch looks like. The Betas were checked and do **not** settle it, which is worth
recording so it is not re-derived: b1.6.2 and b1.8.1 leave `yOffset` at 1.62 while sneaking and
lower only the *model*, by `0.125D` in `RenderPlayer`, so their camera does not move. The eye drop
is release-era — 1.8.9's `EntityPlayer.getEyeHeight` is `f = 1.62F; if (isPlayerSleeping()) f = 0.2F;
if (isSneaking()) f -= 0.08F;` — so the crouched eye is `1.62f - 0.08f`, which is exactly `1.54f`,
and it is applied instantly there and here. It is a **camera** and not a position: `yOffset` stays
1.62, so `eyeY()`, the physics' `posY` and `Pos[1]` are untouched and a world saved while crouching
does not reload 0.08 lower. `PlayerBody::cameraEyeY`, `kSneakEyeHeight`, `kSneakEyeDrop`.

**The slowdown, though, is a1.1.2's own, and it had simply been left out.** The crouch is split
across two classes in that jar and only one half of it is dead: the tail of
`MovementInputFromOptions.updatePlayerMoveState` (`gd.a(dm)`) does
`this.a = (float)((double)this.a * 0.3D)` to `moveStrafe` and `moveForward` whenever the sneak key
reads true, and it asks `isSneaking` nothing — which is exactly why that part works in the game
while the stance, the walk-back and the silent step do not. b1.6.2, b1.8.1 and 1.8.9 carry the same
guarded pair of multiplications, so this one needs no era label. Two details it is easy to get
wrong: the 0.3 scales the **input** rather than the speed, so it lands before `moveFlying` and is
diluted by friction and existing momentum rather than capping anything (`moveFlying` clamps the
magnitude up to 1 and never normalises a short input back up, so the tick stays linear in the stick
and a third of the push does settle at a third of the pace); and the multiply is `f2d; dmul; d2f`,
which is a different float from `x * 0.3f` — 0.7 gives `0.20999999344348907` against `0.21000001`.
Ported as `applySneakSlowdown` / `kSneakMoveScale` and called from `readBodyInput`, the layer the
jar puts it in: the generated vectors drive `moveEntityWithHeading` directly, so a `PlayerBody` that
scaled its own input would disagree with its own oracle. Transcript in `docs/physics-a1.1.2.md`
§ *Sneaking scales the stick*.

**Movement is per gamemode from M3 §1.** Spectator keeps frame-rate free flight with no collision,
which is what it has always been and is now offered under a name that says so. Every other mode
drives `entity::PlayerBody` on the **world's 20 Hz tick**, because a gravity of 0.08 a tick and a
drag of 0.98 a tick mean nothing at any other rate. See `docs/physics-a1.1.2.md`. Creative adds a
third case: `PlayerBody::tickFlying`, which is Spectator's mover with `moveEntity` under it, so
flight collides. Its speeds are Spectator's own 12 and 40 blocks a second converted to ticks; see
§0s.

The bottom screen is three pages: the player's screen (the tab strip, a page, and the **hotbar band**
along the bottom in every mode that has one), the debug readout, and a **settings page**. Three settings, all live —
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
- **The geometry-shader cube pass**, which has now run once and **hung the console** — a third
  program, a second DVLE, a `GPU_GEOMETRY_PRIM` draw with no index buffer, and an 18-vec4 uniform
  table. It fails in ways that are each distinct at a glance, which is the reason for listing them:

  | what you see | what it is | what happened |
  |---|---|---|
  | **both screens dead, HOME does nothing, power off** | a non-geoshader draw following a geoshader one: turning the geometry stage off repartitions the shader units inside a list whose earlier draws are still in flight | **this is what the game path did, and it is fixed** — `Renderer::drawEye` splits the command list on both sides of the cube pass. This row was not on the table before the run |
  | nothing at all in the cube pass | the geometry shader is not emitting: the gsh input stride, or `GPU_GEOMETRY_PRIM` | **ruled out** — the probe's cube draws |
  | the world inside out in patches | the strip's `inv prim`, i.e. the second triangle's winding | **ruled out** — the probe's cube is a cube |
  | every face the same brightness | `faceBasis[].w` shade not reaching the shader | **ruled out** — the probe's faces are shaded apart |
  | textures scrambled per face | `uvSign`, or `tileX`/`tileY` swapped | **ruled out** — the probe's six faces step across its atlas in order |
  | one corner of every quad wrong | `a0` read too soon after `mova` — the one hardware question §2 could not settle | **ruled out**, and this is the answer to that question: seven instructions of separation is enough |
  | garbage colour on three of four vertices | output registers do *not* persist across `emit` | **ruled out** — the probe's quads are evenly coloured |

  Everything about the format itself — corners, winding, UVs, shade, light, byte layout — is pinned
  on the host by `tests/quad_format_test.cpp`, which expands every quad of a section through the
  same basis the shader uses and compares it vertex for vertex against the 12-byte mesher.

  **The probe answered every row but the first, in one launch.** Six quads, one draw, one eye, no
  pool and no command-list split: the cube draws, correctly, and the GPU comes back. So the shader
  is right, the pipeline is right, and the hang was not in either.

  **The first row took four more.** An automatic ramp — start below anything that could break,
  loosen one limit a second, read the rung being held when the machine stops — walked the draw
  count, the quad count, and then the passes. Unlimited geoshader draws over the whole render
  distance were fine; one `DrawElements` behind them was not. The fix and the mechanism are in
  `docs/3ds-performance.md` §2. Two lessons worth keeping:

  - **A ramp only answers about the axis it varies.** The first one pinned the cube pass and left
    the detail passes unlimited, so its smallest rung still had the fatal draw in it. It reported a
    hang at "1 draw, 16 quads" that had nothing to do with either number, and cost a launch.
  - **A watchdog that falls back to the blocking call is not a watchdog.** The first one spent its
    deadline and then called `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` anyway; a wedged GPU never returns
    from that either. It now opens no frame at all, and the report goes to the bottom screen, which
    `consoleInit` leaves single-buffered and which therefore needs no GPU to update.
  - **A watchdog is only as good as the predicate that reaches it.** The second one was correct in
    every way except which frame it asked about: it took the unbounded wait whenever `cubeFormat_`
    was not `Quads`, and `C3D_FrameBegin` waits for the *previous* frame. Toggling the debug page
    back to 4-vertex flipped the field instantly and the next `drawFrame` blocked forever on a queue
    still holding geoshader draws — top screen black, HOME dead, no exception, and no `geo:` line,
    because the watchdog was two lines below the branch that was taken. Reported from hardware as
    "switching back and forth in the farlands killed it"; the gate is now
    `Renderer::geoWorkInFlight_`, set where a `GPU_GEOMETRY_PRIM` draw is recorded and cleared only
    where the queue is proven empty. **A silent bottom screen is the diagnosis**: `geoTrace` needs
    no GPU, so a watchdog that fires always leaves a line, and a death without one is a main thread
    that never reached it.
  - **The heap ran out -- and resident columns were only 29 % of it.** The first
    `sdmc:/3dalpha-oom.txt` reads `free 4405k of 40960k  blocks 10688k  owed 1582k  cols 382`, so
    the grid was 10.4 MB of the 35.7 MB in use and had not even finished loading. The larger half is
    the chunk cache's clean side, the generator's own cache and allocator overhead, **and the
    reporter named none of them**; `clean` and `gen` are on that line now. **The fix for this crash
    is the heap split, not the column budget** -- at 10.4 MB the budget would not have fired.
    Recorded because it is the only measurement of a real failure this project has, and because the
    obvious reading of it is wrong.
  - **The render distance still had no bound in bytes.** Residency was
    purely geometric -- a cell held if inside the radius, dropped when it left, and nothing anywhere
    asked what it cost. Measured, a column is 14.0 KB over ordinary terrain and 21.7 KB at the Far
    Lands, so the debug page's ceiling of 24 needs 50.9 MB out there against a heap capped at 40.
    Both halves of the fix are in `docs/3ds-performance.md` §2, *The heap split*: the split itself
    was chosen from what linear memory actually needs rather than as a fraction (New 3DS heap
    40 -> 75 MB, out of ~38 MB of linear the VBO pool was never going to ask for), and
    `WorldStreamer::setMemoryBudget` is the real bound under it -- the admission radius shrinks a
    ring at a time over budget and grows back under seven eighths of it, so a view too big for the
    heap costs rings rather than the process. **Two ways to make a column cheaper were measured
    first and both are dead**: `compact()` recovers 0.0%, a narrower palette tier 2%.
  - **A guard whose failure mode is the failure it guards against is not a guard.** Three defects
    were reported next, all three inside the fix above rather than anywhere new, and all three of
    that shape. `geoWorkInFlight_` was cleared after a `drainGpu` that had just returned *false*,
    which handed the next frame the unbounded wait on the strength of the evidence that it must not
    — the third failure, re-armed inside its own fix. The watchdog's deadline was picked by
    `gpuStalls_ == 0`, and `gpuStalls_` never clears, so one stall put every later frame on the
    32 ms retry deadline for the session; at the Far Lands an honest frame outlasts that, so the
    watchdog fired on merely-slow frames and `drawFrame` drew nothing ever again — a top screen
    frozen with HOME still alive. And the menu's `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` was the one
    unbounded GPU wait left in the binary, missed because `Renderer::shutdown` drains before handing
    the screen over — but that drain has a deadline, and the case where it expires is the case where
    the GPU is already gone. Now: cleared only on a successful drain; `gpuWedged_` (state) split
    from `gpuStalls_` (history); `ctr::beginFrameBounded` behind the menu's frame too. See
    `docs/3ds-performance.md` §2, *The fourth hardware failure*.
  - **`C3D_FrameSync` is not a GPU drain**, and three callers went on believing it was after that
    had already been disassembled and written down. `rebuildChunks` — which is what a format switch
    *is* — freed the whole VBO pool behind it, `setAtlas` freed the block atlas, and `shutdown`
    freed the pool, the index buffer and all three shader programs, every one of them while the GPU
    could still be fetching from exactly that memory. All three now use `drainGpu`, which polls
    `C3D_FrameBegin(C3D_FRAME_NONBLOCK)` on a deadline. See `docs/3ds-performance.md` §2, *The third
    hardware failure*.

  **The hang is not a table row that was got wrong; it is a row that was missing.** Every entry
  above describes something drawing *incorrectly*, because the table was written by someone
  reasoning about what the shader computes. A command list the GPU does not complete is not about
  what the shader computes at all, and nothing in the reasoning that produced this table would ever
  have reached it. The lesson is the table's own: a first run of any path that programs the GPU
  differently needs a "does the machine come back" row before it needs a "does it look right" row.

  What was done about it is in `docs/3ds-performance.md` §2 under *The first hardware run, and the
  hang*: the whole static half of the path was verified against the built shbin and against
  libctru's own source and is correct, so the remaining work is bisection on hardware, and the probe
  and the four knobs in `renderer.cpp` exist to do it.
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

**The geometry-shader path is now built, it draws, and it is the boot default.** One
8-byte vertex per quad, expanded by `shaders/quad.g.pica`. Everything a host can check about it is
checked and in `docs/3ds-performance.md §2`; the summary is that **it costs nothing to adopt** —
identical quad counts to the last quad, 107.34 MB of world geometry down to 25.96, 31.6 µs a section
down to 28.7, and the VBO pool stops being a constraint at every configuration (distance 8 on an old
3DS goes from 11.15 MB at its ceiling with 1,014 evictions to 3.33 MB with none). None of that is
the gate. The gate is GPU time per quad, and it can only be re-taken on hardware:

> **Settings page → cube format → 4-vertex, and back.** The page boots on `geoshader` now, so the
> comparison runs the other way round: read GPU draw off the Info page, step the row to `4-vertex`,
> wait for the world to re-mesh, and read it again. Take both halves in one session, at one
> position, or the comparison is between two different views.

#### Why it is the default before the gate is measured

**Because "off by default" was a statement about the hang, and the hang is fixed.** The flag was off
for five hardware launches while the quad path wedged the GPU — that history is below, and it is the
reason `Renderer` still carries a watchdog, a stall counter and a latch. What has changed is the
evidence: it now draws the whole render distance across sessions, including at the Far Lands at
distance 12 (`crashlogs/008`, where the thing that failed was the newlib heap and the geoshader was
merely present). Every host-side number favours it by a wide margin, and there is a proven way back
— `quadDrawsStopped_` drops the format on a frame that does not return, and `main.cpp` re-meshes into
the 4-vertex encoding. Booting into the slow path to guard against a failure that no longer happens
costs 7.5× the vertex traffic on every frame in exchange for nothing.

**What was not removed, and why the question of removing it is closed for now.** The 4-vertex path is
not legacy:

- It is the **watchdog's fallback**. Delete it and a wedged GPU has nowhere to go.
- It is the **only encoding with per-corner data**. The 8 bytes carry one light byte and one shade
  index for the whole quad, so smooth lighting and ambient occlusion — both of which this project
  intends — cannot ride on it, and neither can the Beta-era biome tint that `porting-to-other-versions.md`
  is written around. `QuadVertex::ao` is a reserved byte, not an implementation.
- It is the **reference the quad path is checked against**. `tests/quad_format_test.cpp` asserts the
  geometry shader's basis rebuilds `kFaceCorner` exactly; there is nothing to rebuild against if the
  corner tables stop being drawn by anything.

What *is* worth removing is the experiment scaffolding around it — `kGeoMaxSections`, `kGeoMaxQuads`,
`kGeoCubePassOnly`, `kGeoForceMono`, `kGeoAutoRamp` and `kGeoRamp` in `renderer.cpp`, all of which sit
at their "no limit" values and exist to bisect a hang that is fixed. They are kept for now because the
next unexplained stall is exactly when they are wanted, and they cost nothing while `kGeoAutoRamp` is
false. Delete them when a season of hardware runs has gone by without one.

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
  blocker and it was not recorded before. **Resolved in §22** with the first of those: a 512×512
  cube atlas of 4×4 repeats, for the 54 tiles cube blocks can show.
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
  loss on a conversion. Until then the map remembers 768 chunks on an old 3DS and 1,536 on a New one,
  which is a session's worth. Both figures are set by the widest zoom level rather than the default
  one; see §0r.
- **Meshing on a worker thread.** Everything is on the main thread behind a 4-sections-per-frame
  budget. `WorldStreamer` is the seam. Doing it now would be building on a guess: the budget that
  makes the main thread survivable is measurable, and there is no frame time to measure yet.
- **Smooth lighting / AO.** The 12-byte vertex format already carries per-vertex colour and light
  for it; **the 8-byte quad format cannot carry it at all**, so if the geometry-shader path wins,
  these two are mutually exclusive and that is a choice rather than an oversight.
- **Greedy meshing.** *Done — see §22.* Kept here for the constraint it leaves behind: smooth
  lighting, if it ever lands, varies per corner, so it will only let faces with equal corners merge.
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
on purpose: start a stream, stop it, ask whether it is still playing, take a decoded one-shot and
fire it. The two halves have different shapes deliberately: music arrives as a `PcmSource` because it
is minutes long and decoded as it plays, an effect arrives already decoded and is afterwards played
by handle. ndsp sits behind it on the
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

**The menus click, and that is the only sound effect there is.** `of.a(String, float, float)` --
playSoundFX -- is transcribed, including the `0.25f` interface factor and the clip-before-multiply
that makes a volume above 1 mean full rather than more. `GuiScreen.mouseClicked` (`bh.a`) plays
`random.click` at 1.0/1.0 for every press that lands on an *enabled* button, sliders included, and
the port does the same on A; the greyed Multiplayer row stays silent because `fk.c` returns false for
it, and B/START are Escape and are silent as Escape is. **The cursor-move click is the one
deviation**: a1.1.2's menus are pointed at with a mouse and have no cursor, so rather than invent a
tone it plays the quieter, lower setting the game already uses for its own button blocks and levers
-- `random.click` at 0.3/0.5, out of `no.class` and `hu.class`. Both live in `Menu::step` and
`Menu::playClick`, one site each.

**Effects are preloaded, and that is CONTRIBUTING's rule rather than taste.** a1.1.2 hands paulscode
a URL at the moment of the click; we cannot, because that is an SD read on the frame the button was
pressed. So `SoundEngine::preloadSound("random.click")` decodes at boot and `playSoundFX` is
afterwards a handle and two floats.

**The preload runs on a borrowed worker, not on the main thread, and that is not a nicety.** A
3DSX's main thread has 32 KB of stack and nothing in the binary can enlarge it; `kAudioStackBytes`
is 32 KB for the decode thread *alone*, because Tremor's inverse MDCT is not a shallow call.
Decoding inline in `runShell` would therefore overflow the main stack on exactly the consoles that
have a resources folder to decode -- the ones where the feature works -- and the first cut did
exactly that. It is spawned and joined immediately: not concurrency, just borrowing a stack. A sound that was never preloaded is
silence and not an error, which makes the preload list an honest statement of what this port can
make a noise about. When `dig.*` arrives with three hundred files that cannot all be resident, the
answer is a decode request queued onto the audio worker behind the same `playSoundFX`, not a bigger
`kMaxSamples`.

**Measured, on the host, against the real a1.1.2 resources folder:** `newsound/random/click.ogg` is
12,332 frames, 2 channels, 44,100 Hz -- **280 ms and about 49 KB of PCM**, one linear allocation for
the life of the process. The gains are 0.250 for a choice and 0.075 for a cursor move.
`--audio-list <resources>` prints all of it, so a player's folder can be checked before it goes near
a card.

On the console a sample gets its own `linearAlloc`, is flushed out of the data cache once, and plays
on ndsp channels 1-4 round-robin. **The ring steals**, as a1.1.2's rotating `"sound_" + (id % 256)`
steals -- a click that sometimes does not happen is worse than one that cuts another off. Channel 0
stays the music voice and is touched only by the decode thread, so the two threads never name the
same channel and neither needs a lock of ours. The one thing that is an assumption and not a fact is
that libctru's per-channel state is not a structure two threads can tear -- its sources are not
installed here, the music path has rested on the same assumption since it was written, and the
decode thread is 3DS-only so ThreadSanitizer cannot reach it. `platform/ctr/audio.hpp` says so at
the point where it matters.

**Still absent**: positional attenuation (no listener -- no player body), records (no jukebox, and
`streaming/*.mus` is Mojang's own container), and everything blocked on M3. The reachable emitters
when M3 arrives are fizz, fire and the ambient cave counter; the seam is the pools and `Backend`, so
each is a call site rather than a subsystem, as the menu click already was.

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

**Confirmed on hardware: the menus click.** This is the first time any part of this subsystem has
made a noise on a console, and it settles more than it looks like. `ndspInit` succeeds with a real
dumped firmware -- the line above this one used to say that was unverified. The effect path works
end to end: `linearAlloc` plus one `DSP_FlushDataCache`, channels 1-4 handed out round-robin,
`ndspChnReset` -> interp -> rate -> format -> mix -> `ndspChnWaveBufAdd` in that order, and
`interfaceGain`'s 0.250 audible through the console's own speakers. The preload on a borrowed worker
runs and joins without incident.

**What it does not settle** is anything about the *streaming* half: whether the ring underruns on an
Old 3DS and what a buffer actually costs are still open, because a click is one wave buffer queued
once and a track is eight of them refilled for three minutes. The Info page reports decode
microseconds and underruns for exactly that. ThreadSanitizer still cannot reach any of it -- the
decode thread is 3DS-only and the host has no audio thread to race.

**What went wrong first, and it was mine.** The first cut decoded the sample inline in `runShell`,
on a main thread with 32 KB of stack that nothing in the binary can enlarge, when the decode thread
is given 32 KB *by itself* precisely because Tremor's inverse MDCT is not shallow. It reported no
sound and did not crash, which was the tell: nothing had been decoded at all. The lesson is the
older one restated -- **work moved to boot is not thereby moved somewhere it fits**, and "at boot"
and "on the main thread" are two different claims that this file had been treating as one.


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

### 0r. The map's d-pad — a zoom, and the grids taken off the debug page

**The map page owns the d-pad now.** Left and right cycle the grid overlay; up and down zoom. Both
were reachable before only through the maintainer's half of the bottom screen, and one of them was
not reachable at all.

#### The grids moved because the reasoning that put them on the debug page was half right

They were on the settings page on the argument that "is the map aligned with the chunks" is a
*claim this project makes* rather than scenery a player wants. That is true and it is not the whole
truth: a chunk grid over a map is also the single most useful overlay Minecraft has ever given
anybody, and burying it behind a `SELECT` chord on a page of frame timings was answering the wrong
question. It is three states — off, the 128-block map-tile grid, both — under d-pad left and right,
off by default, with the state named in the panel beside the map so it is not something the player
has to press a button to discover.

The settings page is a row shorter for it: render distance, cube format, wireframe, teleport.

#### The zoom is four levels, and the range is asymmetric on purpose

`map::MapWindow` carries a `zoom` as a power of two: **−1 (two blocks a pixel), 0 (one to one, the
default), +1 and +2**. Up magnifies.

**Zoom is a property of the window and not of the stored pixels.** A chunk's 16×16 patch is always
drawn at one pixel per block, so a zoom step invalidates nothing — the patch stamp does not move, the
store keeps everything it had, and the cost of a zoom step is one ordinary redraw rather than a whole
window re-shaded. That is the decision the whole feature rests on, and it is what makes magnifying
free: there is no larger patch to hold.

Which is also why the range is lopsided. Magnifying costs nothing; **shrinking costs the store**,
because the window covers four times the ground per level:

| Zoom | Ground shown | Chunks the window touches |
|---|---|---|
| −2 | 832 × 800 blocks | ~2,600 — **4.0 MB of patches** |
| **−1** | 416 × 400 | ~702 |
| **0** | 208 × 200 | ~196 |
| **+1** | 104 × 100 | ~64 |
| **+2** | 52 × 50 | ~25 |

A store that cannot hold the whole window does not degrade gently, it thrashes, and it thrashes
*backwards*: the sampling scan runs in rings from the player outward, so the least-recently-touched
entry is the ground under the marker and eviction would take exactly what is being looked at. So the
store is sized for the widest level instead — **768 chunks on an old 3DS (1.13 MB) and 1,536 on a New
one (2.25 MB)**, up from 512 and 1,280 — and −2 is out of range, because 4 MB of map patches on a
console whose newlib heap has already been measured running out (`crashlogs/008`) is not a trade
worth making for a level nobody asked for.

#### What it costs to draw — **and the 3,283 µs that found a much older bug**

The estimate here was "under 1.5× the 1:1 copy, neither measured on hardware". **The hardware said
3,283 µs at zoom −1**, four times what the 1:1 blit was believed to cost. That is too much to be
explained by moving the same 41,600 pixels a different way, and it was not the pixels.

**Two things, and the bigger one had been there since the patch cache landed.**

- **The walk was buying its patch pointers by the pixel column.** `renderMapWindow` asked
  `MapStore::patch` for a pointer every time it crossed a chunk edge going south — 208 columns × 13
  chunk rows = **2,704 hash lookups to obtain 182 distinct answers**, each a probe into an index in
  front of 2.25 MB of entries that no 3DS cache can hold. Sixteen output columns share a chunk
  column, so the pointers are gathered once per chunk column now, into 64 on the stack. Zoom did not
  cause this; it made it four times worse and therefore visible.
- **A redraw driven by turning was re-scanning the window for staleness.** A patch goes stale exactly
  two ways — a chunk is stored, or the stamp is bumped — and a stale patch only comes into view when
  the window moves or its zoom changes. `MapScreen` records what the last refresh covered and skips
  the scan when all of that matches, which is the common case: the yaw step moves and nothing else.

Measured on the host over the 1,119-chunk reference world, one 208 × 200 window, sanitised build:

| | zoom −1 | zoom 0 | zoom +1 | zoom +2 |
|---|---|---|---|---|
| copy, before | 1,052 µs | 389 µs | 280 µs | 130 µs |
| copy, after | **397** | **86** | **187** | **78** |
| stale scan, now skipped on a turn | 89 | 24 | — | — |

The 1:1 copy is **4.5× faster than it has ever been**, and the same change is most of what shrinking
cost. Magnifying measures *cheaper* than 1:1, which the estimate had backwards: it reads a quarter of
the source columns and block-moves three quarters of what it writes.

Output is byte-identical before and after, checked at all four zoom levels through `--map`.

**The console figures that replace 3,283 have not been taken, and it is worth taking all four.**
The host ratios say −1 should land near a quarter of what it did and 1:1 near a fifth of what it has
been believed to be since the patch cache — but the host is thirty to forty times faster here and
cannot answer for an ARM11's cache, which is precisely the thing that was being missed. `--map
<world> zoom=<n>` prints all three host costs; the Info page's map redraw row is where the console
ones land.

**Shrinking point-samples rather than averaging.** Later versions average, and averaging four
already-shaded RGB565 pixels 41,600 times a redraw is arithmetic an ARM11 does not have spare. What
it costs is that a one-block feature has an even chance of falling between samples. What it buys,
beyond the time, is that the grids survive: chunk and tile lines sit on origins, origins are
multiples of the sampling step, so every line lands on the lattice at every level.

Two alignment details that are load-bearing, both in `MapScreen::windowFor`:

- **Powers of two only**, so a chunk is a whole number of pixels at every level — 64, 32, 16, 8.
- **The origin is snapped to the sampling step.** Unsnapped, a shrunk window would flip between the
  even and the odd blocks as the player walked, so the whole picture would change colour on alternate
  steps: it would shimmer rather than scroll.

#### Where it is checked

`tests/map_test.cpp` gained five tests. The one worth naming draws the same window into a row-major
buffer and into a **column-major, z-reversed** one — the console's own framebuffer layout — and
demands the same picture at every zoom level. Every `memcpy` path in `renderMapWindow` is taken only
when `strideZ == -1`, which the row-major canvas every other test uses can never be, so those paths
were untested by construction until now. `--map <world> grid zoom=<n>` draws the whole thing to
`map.pam` for looking at.

### 0s. Creative — a hotbar, a palette that is not an inventory, and flight that collides

**M3 step 3, and the first thing in this project with no oracle behind it.** a1.1.2 has no Creative
mode: `Minecraft` carries no gamemode field, `EntityPlayer` no capabilities object, and the word
does not appear in the client. Creative is Beta 1.8's, two years later. So nothing here was
recovered from a jar, nothing here can be pinned against a reference implementation, and there is no
`creative_vectors.hpp` — which is also why gamemode has always lived in `<world>/3dalpha.ini` and
never in `level.dat`. `tests/creative_test.cpp` says so in its header comment, because a suite that
looks like the oracle suites next to it and is not one is worth labelling.

`settings::gamemodeImplemented` now answers true for Creative. Survival is still false, and the
reason is specific rather than general: everything it adds — fall damage, block hardness and break
progress, drops, stack depletion — is a rule on top of the same body, and a mode that is selectable
but plays exactly like Creative would be a label that lies. The World Settings row was reordered so
the two live modes are adjacent, because stepping it is one button and a disabled mode between them
would make every switch pass through a state that refuses.

#### The palette is derived, not curated

Beta's own creative inventory is a hand-written list in `CreativeTabs`. A hand-written list here
would be ids typed into a source file, which CONTRIBUTING forbids and for a better reason than
style: the next version manifest would silently inherit a1.1.2's list.

**The first version of this was "every block the registry defines, minus air", computed in C++.**
It was wrong in three visible ways and §4 below replaced it with a generated column: it offered the
*burning* furnace next to the furnace, flowing water next to water, and the **block** form of a
door — whose texture is the door's lower panel — where the door item belongs. Those are not things a
player holds; they are states the engine writes. The palette is now `mcver::kPalette`, a column of
`data/<version>/items.json` measured out of a running jar, and it is 66 items rather than 70 blocks.

What it deliberately still does not do: no filtering by "would a player ever get this" (bedrock, the
mob spawner and the double slab are all in it, because a Creative mode with opinions about what you
should want is worse than one without); no ordering by category, since id order is the one ordering
that is stable across builds and derivable from the data.

#### The palette is a separate page from the inventory

They are different things — the palette is a catalogue held by nobody, the inventory is what a
player is carrying — and drawing a catalogue inside an inventory frame would imply the blocks in it
were owned. So Creative's tab strip is **Map, Items, Blocks, Look**, which is exactly `kMaxTabs`;
Survival's is Map, Items, Look; Spectator's is Map, Look. The mapping lives in one function,
`Overlay::playerPagesFor`, because the strip, the selected index and a tap on a tab used to work it
out separately and could disagree about which page a mode's third tab was.

The Items page kept its 27 slots and stayed empty, on the argument that Creative carries nothing.
**That argument was wrong and §4 below undid it**: a1.1.2 writes all thirty-six slots into
`level.dat` whatever mode you are in, so there was always something to show — the only thing missing
was an item table to name it with.

#### The hotbar is a band on every page, and the bottom screen grew a third one

`docs/3ds-performance.md` §11 has said since before any of this was written that the hotbar goes on
the **bottom** screen, so the top screen renders nothing but the world — no HUD overdraw at all on a
fill-bound device. `docs/todo-m3.md` §3 said "top screen", which was wrong on both counts: it
contradicts §11, and the top screen has no 2D pass in game at all (no citro2d, no crosshair, only
the outline pipeline), so it would have meant a new textured screen-space GPU pass and a hardware
fill-rate measurement to go with it. The bottom screen is also the only one that can be touched.

So the layout is four bands: tab strip 0–24, **the focus banner's band 24–40**, page 40–208,
**hotbar 208–240**. Both new bands are reserved in *every* gamemode, Spectator included, where they
are backdrop. A page whose height depended on whether the mode had a hotbar would be two layouts,
two sets of constants and two map window sizes to measure.

**And the map got cheaper for it.** It was 208×200 — 41,600 pixels against the 36,864 that were
actually measured at about 700 µs a redraw on a New 3DS, so an estimated 790. It is 208×158 now:
32,864 pixels, *below* the window the 700 µs was measured on, so the estimate goes the other way to
roughly **625 µs**. For once a layout change made a measured cost smaller. `--map`'s window and
`tests/map_test.cpp`'s were moved with it so the harness prints the cost the console pays, and the
map panel's three readouts moved up with it — the wordmark was one row from landing inside the
hotbar, which is now a `static_assert` rather than something to notice on hardware.

Two dirty flags rather than one, because the hotbar and the page above it change at completely
different rates: a shoulder press moves the selection, and the 45-cell palette behind it has not
changed at all. Redrawing the page for a hotbar move would be 45 slot bevels and 45 icon blits to
move one white rectangle.

#### A block icon was a flat tile, and "there is no way to do that here" was wrong

a1.1.2 draws a block in a slot through `RenderBlocks` with the GUI's transform — a three-quarter
view showing a top face and two sides at three brightnesses. This section used to say there was no
way to do that here, because the bottom screen has no GPU access at all and is a CPU blit into
libctru's framebuffer. **The conclusion did not follow from the premise.** A cube seen from the
corner is three parallelograms, and filling three parallelograms by inverse-mapping each destination
pixel back into its tile is forty lines of software rasterising — no GPU, no geometry path, and it
runs on the host where it can be tested.

So §4 below replaced the flat blit with `core/gui/item_icon.cpp`. A plain block draws as a cube; a
torch, a sapling, a door and everything on the items sheet draw flat, which is the same split
`RenderItem` makes. Alpha stays a cutout rather than a blend, which is what leaves a torch standing
on the slot instead of in a black box.

The atlas pixels are **borrowed, not copied** — 256 KB is not something the overlay should hold a
second time — so `Overlay::setAtlas` is called at world open and again after the pause menu's
Texture Pack screen, and an atlas that has not been built leaves the icons unpainted rather than
painting the wrong thing.

#### X focuses the bottom screen, and it is not a convenience

The 3DS screen is resistive: it wants a stylus, and a player holding the console to walk does not
have one out. **X turns the focus on, and B is the only way off it** (X used to toggle). Focused, the d-pad drives a cursor over the palette grid and the
hotbar band and A picks; the circle pad and the camera keep working underneath, so the world is not
paused. The palette grid and the hotbar are both nine columns wide on purpose — `static_assert`ed —
so the cursor steps straight down from the bottom palette row onto the hotbar slot in the same
column.

ZL and ZR change the held slot from anywhere, focused or not, which is the New 3DS's shoulder pair
doing what a mouse wheel does in the original. **They do not exist on an old 3DS**, which is the
other half of why the focused d-pad is not a convenience — left and right on the hotbar row are an
old console's only button route to the selection.

While focused, L and R **change tab** and **break and place are suspended**, `main.cpp` reading the
same `Overlay::uiFocused()` flag the overlay sets. One press does one thing.

L and R were the palette's pager first and that was wrong: a focused screen with no button route
between Map, Items and Blocks could only be navigated by touching it, which is the one thing the
focus exists to avoid. The palette pages instead by running the cursor off either end of its grid,
and by the two arrows on its title row for anyone holding a stylus — a page control invisible to a
player who never focuses the screen is not a page control.

**The focus survives a tab change**, so filling a hotbar is one press of X and then buttons all the
way. B lets it go. X used to as well; since 2026-09-13 it does not, because on a container screen
X is a shift-click -- `item::ContainerSession::quickMove`, tested in
`tests/container_session_test.cpp` -- and the button that acts inside a focused screen must not be
the one that throws the player out of it. A stack on the cursor is still thrown by X first.

#### The banner, and the one thing that made it awkward

A mode with no indicator is the worst kind, so the focus draws one: a near-black label row under the
tab strip saying `Bottom screen focused` and what the buttons mean *on that page*, with a gradient
under it fading back into the page.

**The fade darkens the pixels it finds, and that is not an operation you can apply twice.** Drawn
over the page, the map alone would redraw through it on every block walked and the strip would go a
shade darker each time — black within a minute. Three fixes were possible and only one is simple:
give it a band that nothing but the backdrop ever paints. That is `hud::kBannerTop`, 24–40, and
every page below it now starts at `kPageTop`. So the banner is drawn once per page clear and never
repaired, everything that changes what it says sets `dirty_`, and leaving the focus is a full redraw
because there is no way to undo a dim. The cost is 16 pixels of map, and unfocused the band reads as
the page having a margin.

#### The focused stick scrolls the map

On the Map page, focused, the circle pad pans the window and the body stands still — the two would
otherwise fight over the same window, and the map would be dragged back under the player every step.
The pan is added in exactly one place, `MapScreen::windowFor`, so the sampling rings, the redraw
signature, the picture and the panel's numbers all move together and none of them had to learn about
panning.

Three details worth having decided:

- **Half a window a second, at every zoom**, expressed in windows rather than blocks — pushing the
  stick over should take about the same time to cross the picture whether the picture is 104 blocks
  across or 416, and a fixed blocks-per-second would feel like four different speeds.
- **The panel's `x` and `z` become the middle of the picture** while it is panned, drawn in the same
  amber as the focus cursor, with a four-armed cross at the centre that leaves the marked pixel
  uncovered. `y` stays the player's: a point on a map has no height. The marker stays on the player
  and simply clips off the edge when they scroll away from it, which is the point of a panned map.
- **Letting the focus go puts the map back on the player.** A pan is something you did with the
  focus on; leaving it with the window parked four hundred blocks away would be a mode with no exit,
  since the stick walks again the moment X is let go.

**The map keeps its own d-pad even focused** — zoom and the grids — because the stick is what moves
a focused map and there is no cursor on that page to walk over. Which is also why the hotbar draws
no cursor there: marking a slot no button moves is worse than marking none. ZL and ZR still change
the held slot, on the map page like everywhere else.

Spectator has no focus and therefore no pan, because X is its sprint and it has no hotbar to focus
on. That is a consequence rather than a decision, and it is the one place the two modes' controls
genuinely diverge.

#### Instant break was already true, and nothing is spent

"Instant break" is not something Creative had to add here. Block hardness governs the *progress* of
a break, accumulated per tick through `Block.getPlayerRelativeBlockHardness`, and that mechanism is
Survival's and does not exist. So one press is one broken block in every mode — which is Creative's
rule, and is a thing Survival will have to take away rather than a thing Creative added. The same
goes for depletion: the hotbar holds a stack of one and the place path never touches the count.

The five-tick repeat stays, in every mode, because it is not hardness — it is
`Minecraft.runTick`'s own `ticksRan - lastClickTick >= Timer.ticksPerSecond / 4` on both mouse
buttons, and the timer is built with 20.0f.

#### Flight is Spectator's mover with `moveEntity` under it

`PlayerBody::tickFlying` is invented and says so in its header comment, next to constants that are
all measured. It clears the motion, sends the stick heading through the original's own `moveFlying`
so a direction means the same thing flying as walking, takes the vertical from B and Y — the same
two buttons Spectator rises and falls on — and then sweeps the box through `moveEntity`. **That
sweep is the whole difference from Spectator**, which has no body to collide with.

The speeds are Spectator's own, converted from frames to ticks: `flyCamera` moves 12 blocks a second
or 40 held down, which at 20 Hz is 0.6 and 2.0 a tick. A tick of flight is a velocity and not an
acceleration — no drag term, motion cleared at the end — so letting go stops you dead, which is
what a camera does and is the point. `fallDistance` is cleared every tick so flight cannot bank a
fall for Survival to cash in the moment it is switched off over a canyon.

The toggle is a double tap on B inside a quarter of a second, timed in **frames** because it is a
gesture rather than physics and has to mean the same thing at 30 fps as at 60. Leaving Creative from
the pause menu without leaving the world switches it off, or the player would be hanging with no way
to turn it off.

`tests/creative_flight_test.cpp` checks the one property flight is *for*: it does not fall, it
climbs and descends at the speed it is given, it lands on a floor rather than through it, it stops
at a ceiling, and a one-block wall stops it **at both speeds** — the sprint speed's two blocks a
tick is exactly the case a ray would tunnel through and a swept box does not. All of it again across
the negative axis.

#### What is not done

- **Nothing here has been seen on hardware.** It builds for the 3DS and the host suite is green;
  the console run has not happened. The same is still true of the selection outline from step 2.
- **Placement is still air-only.** a1.1.2 also replaces water, lava and snow, which wants the
  replaceable-material test — a Survival-shaped question, unanswered.
- **status.md still has no write-up of M3 steps 1 and 2.** The body and the reach/break/place seam
  landed before this and their results are in `docs/todo-m3.md` and `docs/physics-a1.1.2.md`; they
  have not been folded in here. That is a handoff gap and it is this file's, not theirs.

Two entries that used to be here — the hotbar not being saved, and a door from the palette placing
half a door — are done and are written up in §4 immediately below.

### 4. The item table, the inventory, and five things the first Creative pass got wrong

Everything in this step came out of one play session's list of complaints. They looked like six
unrelated bugs and were three: **the camera was pinned to the tick**, **the body and the free camera
never handed the position to each other**, and **there was no item table**, which is what put half a
door in the hand and the burning furnace in the palette.

#### The camera was drawn where the body was, not where it is

`camera.x = body.x` ran after the body's ticks and nowhere else. The body moves at 20 Hz and the
screen draws at 30 or 60, so the camera stood still for a frame or two and then jumped a whole
tick's travel — about a fifth of a block at a walk, which reads as *stepping* rather than walking
and was reported as "moving in whole integers".

`Entity` has carried `prevPosX/Y/Z` since forever for exactly this, and `EntityRenderer.orientCamera`
reads `prevPos + (pos - prevPos) * partialTicks`. `PlayerBody` now snapshots the same three at the
top of `tick` and `tickFlying`, `renderX/renderEyeY/renderZ` are that line, and `TickTimer` already
had the fraction. `setFeet` snaps the previous position to the new one, because a teleport has
nothing to interpolate from — without that a gamemode change drew the camera *sliding* to its new
place over the following tick.

**What is saved is still `body.eyeY()`, not the interpolated value.** A position part way through a
tick is a picture rather than a state: writing one into `Pos` puts a number in the file the physics
never produced.

#### Changing gamemode put you in the ground

Spectator flies a bare camera and never touches the body; every other mode reads the camera off the
body. Nothing handed the position across, so a mode change re-read a body that had last been ticked
at world entry — usually the spawn point, frequently inside terrain by then. The fix is six lines
where the pause menu returns: leaving Spectator puts the body under the camera, clears the motion
and the banked `fallDistance`, and cancels the sprint. The other direction needs nothing, because
the camera is already at the eye the body was handing it.

#### The item table, which is what the other four bugs were

`data/a1.1.2/items.json` is generated by a new `tools/genref.java --items`, and every column but the
name came out of a running jar:

| Column | Where it comes from |
|---|---|
| `icon`, `stack`, `durability` | `Item.itemsList`, read off the constructed singletons |
| `sheet` | a1.1.2's own rule: `RenderItem` draws terrain.png below id 256 and gui/items.png above |
| `places` | **measured** — a real `EntityPlayer` in a real `World` uses each item on each face of each of five grounds, and the block that appears is the answer |
| `palette` | one derived rule and one four ids long; see below |

Measuring `places` rather than reading it is what makes doors, signs, reeds, seeds and redstone come
out right without anything knowing they exist: `ItemRedstone`, `ItemSign` and `ItemDoor` decide
inside `onItemUse`, and `ItemDoor`'s answer depends on the material it was constructed with. Five
grounds because placement has preconditions — seeds want farmland, cactus wants sand, reeds want
sand or dirt with water beside it — and five faces because a ladder and a wall torch refuse the top.

163 items, of which 161 fit the 512-entry table; the two music discs are 2256 and 2257 and are left
out rather than costing a 2,258-row array for two items no a1.1.2 player can obtain. A stack holding
one still round-trips through a save untouched.

**The palette rule.** An ItemBlock is hidden when some item above 255 places the same block, because
that item is the form a player holds — one rule, read out of the measured column, covering doors,
signs, reeds, seeds and redstone without naming any of them. Four ids are then excluded by hand:
flowing water, flowing lava, fire and the burning furnace, which are block *states* with a resting
form already in the palette. That is the only judgement in it and it is spelled out in the generator
rather than dressed up as a derivation. 66 items offered.

#### The inventory is real, and it is saved

`core/item/inventory.{hpp,cpp}` replaces the nine-slot `Hotbar`: 36 main slots plus 4 armour, with
the hotbar a *view* of 0..8 exactly as it is in `InventoryPlayer`. `src/impl/items/b1_2/` — empty
since the slot was invented — now holds the numbering that gives a slot index its meaning, and the
encoding was already in `level_data.hpp` and the Alpha `level_dat.cpp`, so saving was a plumbing
job: `WorldStreamer::setPlayerInventory`, called on the edit rather than on the frame because it
copies up to forty stacks and their preserved NBT.

**A slot number this version does not model is kept, not dropped.** `InventoryPlayer.readFromNBT`
throws away anything that is neither `< 36` nor in `100..103`, and this project's own reference
world has a stack at slot 81. Dropping it would be a load/save cycle destroying data.

The Items page is 27 live slots with a cursor. One press picks a stack up and the next puts it down,
rather than a drag: a resistive screen sampled once a frame reports a drag as a sequence of jumps,
and the d-pad has no drag at all, so a one-press gesture is the only one both input routes can make.

#### Two sheets, and a cube in the slot

`gui/items.png` is loaded into a second 256×256 plane of `AtlasImage`. **Nothing on the GPU ever
sees it** — the icons it feeds are drawn by the CPU into the bottom screen's framebuffer — so it
costs 256 KB of heap and no VRAM. A pack without one is a pack: every icon that wanted it falls back
to the terrain tile of the block the item places, which is what this screen drew before.

`core/gui/item_icon.cpp` draws a plain block as a cube seen from the corner and everything else
flat, which is the split `RenderItem` makes. The projection is the flat 2:1 isometric one rather
than the original's 30-degree matrix: at sixteen pixels across the difference is under half a pixel
and the 2:1 version lands its edges on pixel boundaries, so the silhouette has no stair-stepping.
The three shades **are** the original's — `mesh::kFaceShadeFloat`, the same table the world mesher
uses, because a slot lit differently from the world is the kind of small wrongness that is felt
rather than seen. Stack counts are drawn above one, which is what `RenderItem` does.

#### Fire's texture really does say "fire"

Tile 31 of a real a1.1.2 `terrain.png` is red pixel-lettering on transparency and tile 47 is a flat
magenta square. Neither is ever shown by the real client: `Minecraft` registers a `TextureFX` for
each at startup and overwrites those 256 texels every tick with a procedural flame. A build that
loads terrain.png and stops there draws the placeholder.

`core/texture/texture_fx.cpp` is `TextureFlamesFX` transcribed from the class file — a 16×20 heat
field, a seed row of noise, a six-neighbour blur weighted 18:1 toward the cell below, and a ramp
where red is linear, green is the square of the heat and blue is its **tenth** power. The randomness
cannot be transcribed: the seed row comes from `Math.random()`, whose generator is seeded from the
wall clock, so two runs of the *original* disagree. Ours draws from `JavaRandom` with a stated seed,
which makes the output reproducible and therefore testable.

**It is baked, not animated.** The simulation is run 200 ticks at pack load and written into the
atlas, so the tile is a flame rather than a placeholder wherever it is drawn and costs nothing per
frame. Animating it needs the atlas texture updated 20 times a second in VRAM *and* a fire emitter
in the mesher to be visible in the world at all — neither exists, and both are named here rather
than half-built.

A pack's own fire tile is overwritten, deliberately: that is what the original does with it.

### 5. Ten shapes that drew nothing, a slab that drew as a cube, and swimming

The second play session's list, and again it was fewer bugs than it looked. "A slab and a double
slab both place a double slab" and "the fence texture is just the wood texture in 2d" turned out to
be the same bug seen twice -- **the mesher could only draw unit cubes** -- and "you can place crops,
saplings and cacti on anything" turned out to be a rule the tick system already knew and the
placement path had never asked.

#### The cube vertex formats cannot express a slab

`WorldVertex` stores a corner as three bytes of block coordinate and `QuadVertex` stores a face
index the geometry shader rebuilds a *unit* cube from. Neither can put a corner half a block up. So
the five standard blocks whose bounds are smaller than their cell -- the slab, the snow layer, both
pressure plates and the button -- were all drawn as full blocks, and a slab and a double slab were
pixel for pixel identical.

`core/mesh/box.hpp` is the fix: one axis-aligned box, down the 16-byte **detail** stream that
torches and fluids already use. The UV rule is `RenderBlocks`' own, read out of `bc.a` through
`bc.f` -- one method per face -- and the interesting half is the sides, which take v from the box's
**y** range with the *top* edge on the low end of the tile. A slab therefore shows the top half of
its tile, which is what makes an upside-down slab's texture look shifted in every version of the
game.

**A full cube through the box path is identical to one through the cube path**, asserted vertex for
vertex in `tests/box_test.cpp`. That is what says the new geometry agrees with what the console has
been drawing since M0 rather than being a second opinion.

#### Ten emitters, and `renderItemIn3d`

`meshSection` skipped a render type it had no emitter for, on the argument that a missing ladder is
obvious and a cubic one looks deliberate. That was right while nothing could place a ladder;
Creative can place all ten, so the choice became "invisible" rather than "not yet".

`core/mesh/shapes.cpp` has them: **stairs, doors, ladders, cactus, fence, crops, rails, redstone
wire, levers and fire**. Four need no new geometry -- stairs, doors, ladders and cactus draw the box
they collide as, already measured against a running jar -- and the rest carry constants read out of
their own `RenderBlocks` method. `bc.a(ly,III)`'s if-chain is the map from a render type to the
method that draws it, and it is quoted in the header.

Two were honestly simplified and said so in place: the lever's arm pointed straight out of its
mounting rather than swinging, and fire is four wall-hugging sheets rather than the original's
flapping diagonals. **The lever is a transcription as of step 8** -- and the angle named here was
wrong as well, it is 0.69813174 radians, 40 degrees, not 22.5.

The fence also fixed its own inventory icon. `RenderBlocks.renderItemIn3d` is a static `(I)Z` in the
jar and answers true for render types **0, 10, 11 and 13** -- the standard block, stairs, the fence
and the cactus -- so a1.1.2 draws a fence in a slot in three dimensions and this build drew a flat
plank tile. `core/block/model.hpp` now holds the box list, and both the mesher and the icon read it,
so a fence in the hand and a fence in the world are the same shape rather than two sets of
constants.

#### Measured: +0.9 µs a section, and the two attempts before it

Over the real 1,119-column world, on the dev host at `-O3` with sanitizers off:

| | before | after |
|---|--:|--:|
| cube quads | 2,135,496 | 2,135,496 |
| opaque detail quads | 56,038 | 56,074 |
| mesh, per section | 34.5 µs | 35.2 µs |
| of which face emit | 23.5 µs | 24.4 µs |

Thirty-six extra quads, because a played world has few ladders and no slabs. **The first version cost
13 µs a section for those thirty-six quads** -- face emit went 23.5 → 36.4 -- because it asked
`selectionBox` for every block in the world to find out whether it was a unit cube. Folding the two
facts the loop needs into one `unitCube` column of the generated block table, in the row it has
already loaded, brought it back: a second table is a second cache line on the hottest loop in the
project, and that is the whole of the difference.

#### The placement rules were already written; nobody asked them

Every `canBlockStay` predicate has been in `core/tick/behaviour.cpp` since the tick system landed,
because a neighbour change has to ask the same question -- that is what makes a flower pop when you
mine the dirt under it. The edit path tested `blockAt(...) == air` and stopped.

So a cactus went on glass, wheat on stone and a sapling into mid-air, and the tick deleted them a
moment later, which reads as the game losing your block rather than as a rule. `tick::canPlaceAt` is
`Block.canPlaceBlockAt` and its dozen overrides, built on the predicates that were already there,
and the placement path asks it. `tests/tick_test.cpp` checks the two agree: **a block placement
accepts must not be one the next tick deletes**, over every ground the version defines.

It also ended the "placement is air-only" deviation, because the base `canPlaceBlockAt` counts a
liquid as free space. You can build into a pond, as you can in the game.

#### Swimming

`docs/physics-a1.1.2.md` had the two liquid branches transcribed and ended "not yet written". They
are written, and fourteen new oracle cases match a real `EntityPlayer` bit for bit -- sinking,
swimming forward, rising on a held jump, wading out onto a shore, both liquids, and a one-block
puddle that is deliberately *not* deep enough to swim in. (The case named for rising on a held jump
did not rise, and neither did the jar the oracle was taken from. See step 6.)

The predicates underneath were where the reading was. `isInWater` is not a material test: a cell
only counts if the fluid's **surface** reaches the probe, and the comparison is against the loop's
upper bound rather than the cell being examined -- read it as the cell and every puddle is deep
enough to swim in. `isAnyLiquid` floors its minimums twice for negative coordinates, which is the
original's and is reproduced. The four methods are written up in the physics doc.

**The current is not implemented.** `handleMaterialAcceleration` also pushes the body along the
fluid's flow vector, and that needs a flow field this port does not have. It is why every oracle
case is a *still* pool.

#### Smaller things from the same list

- **Sprint is Creative only.** a1.1.2 has no sprint at all; Survival here is meant to *be* a1.1.2,
  and Creative is already openly not.
- **Two slabs make a double slab**, which is `BlockStep.onBlockAdded` and therefore a tick
  behaviour rather than anything the placement path knows. The class file tests the block *below*
  and not the block being added, so a double slab placed on a single slab also collapses into one --
  a real quirk, measurable in a running client, and reproduced.
- **The palette lost three more ids**: the double slab (which is now *made* rather than placed), the
  lit redstone ore and the *unlit* redstone torch. The one you carry is the lit one, which is why
  that pair is the way round it is. 63 items offered.

#### What is not done in step 5

- **Still nothing on hardware.** Both targets build, 785 host tests pass, ThreadSanitizer is clean.
- ~~**The lever's arm does not swing**~~ -- fixed in step 8. **Fire does not flap.** Named above
  and in the source.
- **The fluid current does not carry you.**
- ~~**A door still places one block, not two**~~ -- fixed in step 6. Its two sides are still not
  mirrored: the block table drops the sign that means "mirror this face", because the 12-byte vertex
  has no flip bit.
- ~~**Redstone wire is drawn with the crossing tile always and no colour**, because the detail
  vertex has no per-quad tint.~~ -- fixed in step 8, and the reason given here was wrong: a1.1.2's
  wire renderer has no tint at all, and the glow is the atlas row below.

#### What is not done in step 4

- **Still nothing on hardware.** Both targets build, 760 host tests pass, ThreadSanitizer is clean
  over the streamer and the cache — the console run has not happened.
- **Buckets place nothing.** `ItemBucket` works from a ray trace in `onItemRightClick` rather than
  from a face, so there is no placement for `--items` to measure and `places` is 0. It is a real gap
  in what a Creative hand can do.
- **Only placeable items are offered.** A sword, an ingot and a bucket do nothing this build can
  perform, so the palette does not offer them. They are still in the table, still drawn, and still
  round-trip through a save — a sword the real client left in a world stays a sword.
- **Armour slots are storage.** They load, save and can be swapped into; nothing wears anything.
- **Fire, doors, ladders, rails, stairs, fences, levers, crops, cactus and redstone wire still have
  no mesher emitter**, so a door placed from the palette lands in the world and is invisible. That
  is unchanged by this step and is the largest remaining gap in what the palette can put down.
- **A door places one block, not two.** `ItemDoor.onItemUse` writes the lower half and an upper half
  above it with metadata 8; the edit path writes one block with `placementMetadata`. It did not
  matter while doors were unreachable and it matters now that the palette offers one. The measured
  `places` column only records the block, not the second write, so fixing this is a placement-path
  job rather than a table one.

### 6. Holding jump in water, and a slab a1.1.2 will not let you finish

Two reports from the third play session. One was a real bug with a **wrong oracle standing behind
it**, which is the interesting half; the other was a1.1.2 being a1.1.2.

#### Holding jump in water jumped instead of swimming up

`PlayerBody::tick` ran `if (input.jump && onGround) jump();` and then took the liquid branch. The
class file does not: `ge.j()` -- `EntityLiving.onLivingUpdate` -- reads `isInWater()` and
`handleLavaMovement()` first and branches **three ways**, adding a flat `0.03999999910593033` to the
motion in either liquid and reaching `jump()` only when both are false. So a swimmer got one 0.42
launch off the bottom of the pool and then sank, instead of rising steadily. Against the swim
branch's 0.8 drag and 0.02 sink the real thing settles at 0.06 a tick, a block and a fifth a second.
There is also no `jumpTicks` cooldown in a1.1.2 -- that is a later version's -- so the button fires
again the tick you land.

**The fixture agreed with the bug.** `tests/player_body_vectors.hpp` has had a case called
`swim_up_by_holding_jump` since step 5, run twice as they all are, matching a real `EntityPlayer`
bit for bit -- and in it the player sinks. `tools/genref.java` drove the jar with
`if (jumping && onGround) jump()`: onLivingUpdate's third branch and neither of its first two. The
oracle was a transcription of the same misreading, so fourteen liquid cases agreed to the last bit
on the wrong answer, and a tick-for-tick comparison can never catch that.

The generator reproduces all three branches now. Regenerating moved **240 lines of 13,317** -- the
two `swim_up_by_holding_jump` cases and the two grounded/airborne counters, nothing else -- which is
both the fix and the evidence that the emitter is deterministic under this JVM.

`holding_jump_in_water_rises_instead_of_jumping` is the guard that would have caught it: it asserts
the *shape* of those rows rather than their bits -- the first tick's motion is upward and smaller
than a jump, the feet gain two blocks over sixty ticks, and no step is ever `onGround`. **A fixture
whose name claims a behaviour needs a test that its rows show it**; there is no other kind of test
that can fail when the oracle is wrong in the same direction as the port.

786 tests pass.

#### "Slabs cannot be placed on the top half of a block" -- and in a1.1.2 they cannot

Probed rather than argued, by driving `editBlocks`' exact predicate chain -- ray trace,
`canPlaceAt`, the player-box check -- over a scene world:

| Aim | Result |
|---|---|
| The top face of a slab, standing beside it | places, and `BlockStep.onBlockAdded` merges the two into a double slab |
| The upper half of a wall's side face | places, in the adjacent cell, as a bottom slab |
| The lower half of the same face | places, identically -- the half you click changes nothing |
| The top face of the slab **you are standing on** | refused: the new block would be inside the player |

The last row is the report, and it is what the class files do. `ItemBlock.onItemUse` offsets by the
struck face and writes **one** block; the merge is `onBlockAdded` looking down afterwards. So
finishing a slab means placing into the cell *above* it -- the cell your legs are in -- and
`World.canBlockBePlacedAt` refuses it through `checkIfAABBIsClear`, which fails on any entity in the
box with `preventEntitySpawning` set. That is the player. Step off the slab and click its top face
and it completes.

The modern behaviour the report describes -- clicking the upper half of a face to get an
upside-down slab -- **does not exist in a1.1.2 at all**. `oi` (`BlockStep`) sets its bounds once in
its constructor, `(0, 0, 0, 1, 0.5, 1)`, reads no metadata anywhere, and the placement path has no
notion of which half of a face was struck beyond the face itself. Adding it would be a deliberate
deviation of the same kind as Creative flight, and would need the same argument written down first.

#### A door arrived as half a door, and nothing could be opened

Two more from the same session, and they turned out to be one missing method each.

**`ItemDoor.onItemUse` is the only item in a1.1.2 that places two blocks**, and the placement path
did not know it: it read `places` off the item table, wrote block 64 once, and stopped. A lone
lower half then fails `onNeighborBlockChange`'s "is my other half there" the first time anything
beside it changes and deletes itself -- so the report was "doors disappear when something gets
placed next to them", and the cause was three hundred lines away in the placement. The method also
carries a facing floored out of the player's heading and a **hinge that mirrors** against a door or
a wall already on one side, which is what makes a pair of doors meet in the middle.

**Nothing was asking `Block.blockActivated`.** `PlayerController.onPlayerRightClick` asks the block
before the item, with no sneak override, and a block that answers true has spent the click. Without
that call there is no way to open a door, flick a lever or press a button, and L could only ever
place. `tick::blockActivated` is the dispatcher, on the behaviour column like every other tick
dispatch; the four bodies -- door, lever, button, redstone ore -- sit beside the redstone halves
that were already written and reuse them, so opening a door by hand is `doorSetOpen` with the state
negated rather than a second copy of it.

Two of the four are worth naming: **an iron door consumes the click and does nothing** (its first
line is a material test, which is the whole of why it needs redstone), and **redstone ore lights but
does not consume** -- its override glows and then returns the base class's `false`, so one press
lights the ore and places the block in your hand.

**The right-click moved into core**, as `core/item/use.cpp`. It had been inline in
`platform/ctr/main.cpp`, which was fine while a placement was "offset by the face and write one
cell" and stopped being fine the moment it was a two-block door oriented from a heading: nothing in
`src/platform/` can be tested. `WorldStreamer::rightClick` is what is left on the platform side --
the renderer bracket, held around the whole decision rather than around each write, because one
click is now up to four.

Two rules came back with the transcription, both from `ItemBlock.onItemUse` and its call to
`World.canBlockBePlacedAt`:

- **A snow layer is replaced, not built on.** The method's first line rewrites the side to 0 and
  skips the offset entirely.
- **Water, lava, fire and snow take a block whatever the block's own rule says.**
  `canBlockBePlacedAt` returns true for those four *before* it reaches `canPlaceBlockAt`, so the
  original lets you plant a sapling in water and then kills it on the next tick. This port had
  tightened that into a refusal when the placement rules were first wired up in step 5 -- a
  reasonable-looking mistake, because the two methods have almost the same name and only one of
  them is the one a placement asks.

**Measured, not just derived.** A scratch JVM harness stood a real `EntityPlayer` in a real world,
used a real door item at seven headings, and called `blockActivated` on a door, a lever, a button, an
iron door and redstone ore. Every number agrees with the port: the facing table (yaw 0 -> 1, 90 -> 2,
180 -> 3, 270 -> 0, and -90 -> 0 as well, which is the row that would catch a sign error), the hinge
mirror, the door's `meta ^ 4` on both halves, the lever's 5 -> 13 -> 5, the button's 1 -> 9 with a
second press consumed and ignored, the iron door's refusal, and the ore's `false`. The facing table
is in `docs/physics-a1.1.2.md` and the port's copy of it is in `tests/use_test.cpp`.

`tests/use_test.cpp` is sixteen cases over the whole path, including the regression the report was:
place a door, build three blocks around it, and both halves are still there.

**802 tests pass**, and both targets build.

#### What is not done in step 6

- **Still nothing on hardware.**
- **A chest, a workbench, a furnace and a jukebox do not consume a right-click**, where a1.1.2's do.
  All four answer by opening a screen and there are no screens; eating the click to show nothing
  would be indistinguishable from the placement being broken. It is one deviation with one cause and
  it reverses the day a container screen lands. Named in `core/tick/behaviour.hpp`.
- **A door's two faces are still not mirrored**, which is the 12-byte vertex having no flip bit --
  the same gap step 5 named. *Closed 2026-09-11:* doors draw on the detail stream, which needs no
  flip bit. The mirror was what showed the second door of a pair hinged on its own side. See
  current-work.md.
- **No sounds.** Every activation in the class files plays one -- `random.door_open`,
  `random.door_close`, `random.click` -- and nothing in this port can emit a sound effect yet. Each
  one is named at the line it would go on.
- **A pressure plate still needs an entity to stand on it**, which is the one redstone input left.

### 7. Footsteps, breaking, and sixty-four flecks of dirt

The third play session's second half, and the first three lines of this build that make a noise
about the world rather than about a menu.

#### There is no `dig.*` in a1.1.2, and that is the finding

Every later version splits footsteps from breaking -- `step.grass` against `dig.grass` -- and
this project's own audio doc had a "not yet ported" row for both. a1.1.2 does not: `bb`
(`StepSound`) has two getters and **both return `"step." + name`** on the base class. Breaking a
block plays a footstep. Two of the nine singletons override one getter and are the only rows
where the pair differs: sand breaks like gravel, glass breaks with `random.glass`.

Which getter is which cannot be read from the bytecode -- on the base class they are the same
two lines -- so they are named by their callers, and that fact lives in `extract_blocks.py`'s
`MEMBER_MAP` beside the other four of its kind. Backwards, and glass breaks with a footstep.

**The table is generated and then measured.** `tools/extract_blocks.py` gained about a hundred
lines: it finds the StepSound class from Block's field list (the type with the most static
finals that Block also has exactly one instance field of -- both halves are needed, because
Block has 78 static finals of its own type), reads the nine singletons out of the initialiser
with their names, volumes and pitches, resolves the two subclasses' overrides, and takes each
block's row off the `setStepSound` call in the chained constructor. Six blocks never call it and
both cases are real: water and lava take **Block's constructor default**, and the two staircases
**copy the block they are modelled on**, which is what makes wooden stairs sound like wood
rather than like stone. All seventy rows were then read off a running jar and compared: **zero
mismatches**.

`Block.blockParticleGravity` was measured the same way and is 1.0 for all seventy, so it is a
constant here rather than a column.

#### The positional sound path

`of.b(name, x, y, z, vol, pitch)` is not the interface path scaled. It has **no `0.25f`**, which
is why a footstep at `volume * 0.15` is audible at all; a volume above 1 stretches the fade
distance instead of getting louder; and the falloff is paulscode's `ATTENUATION_LINEAR` over 16
blocks -- a *library* rule, the one part of this that could not be read out of the jar, and
named as such where it is implemented.

What does not cross the seam is **stereo**: panning needs a listener orientation and
`audio::Backend` carries `playSample(id, gain, pitch)`. A sound is attenuated and centred, and
that is the deviation.

**The backend's sample cap moved from 16 to 48**, and the number is the block table's rather than
a guess: six distinct `step.*` keys plus `random.glass` and `random.click`, which is 35 files in
Mojang's own set. Partial loading is not an option -- `playSoundFX` draws its variant *before* it
knows whether the file is resident, so a key with half its variants loaded is a footstep that is
silent half the time. About 1.1 MB of linear memory for a full set, and zero for the common case,
because a1.1.2 shipped no sounds at all.

#### The footstep trigger, and the four ways to get it wrong

It lives in `moveEntity`, not in the player, and every clause of it is audible: it measures the
distance **covered** rather than asked for (so walking into a wall is silent), accumulates that
distance **outside** the test (so a sneaking player banks it and pays out on standing up),
**increments** `nextStepDistance` rather than resetting it, and lets **snow on top win outright**
-- including over a liquid, which the branch below would have silenced.

`PlayerBody::move` decides which block earned the step and leaves it in `stepSoundDue`; the
platform layer, which is the half with a listener, plays it. That split is what lets the whole
trigger be tested with no audio at all, and `tests/step_sound_test.cpp` does exactly that.

#### The particles, and a rounding trap that ate them

`EntityDiggingFX extends EntityFX extends Entity`, so a fleck runs the **same `moveEntity` the
player does** -- it lands on the ground and slides along it. That is what finally pulled the
block sweep out of `PlayerBody` into `core/entity/sweep.hpp`; the move is verbatim and the
bit-exact body fixture is what proves it.

Sixty-four particles per block, cut from a 4x4x4 grid of the cell and thrown along the offset
from its middle, spawned **before** the block is cleared because they need its texture. They
draw through the *existing* detail pipeline and the existing shared index buffer: four
`DetailVertex` per particle, positions relative to the eye's block rather than to a chunk
origin, so the whole pass needed one 32 KB linear allocation and no new shader.

**The trap.** The first version stored the position and rebuilt the box from it each tick. That
is a lossy round trip: the clip lands the box's bottom exactly on the block's top, and a rebuilt
box sits a fraction of an ulp below it -- at which point `calculateYOffset`'s
`mover.minY >= block.maxY` declines to stop it and the fleck falls through the world. The box is
the authority, as it is in the jar, and `particles_fall_land_and_stop` is the test that found it.

**The pool is 512 and lives on the heap.** 512 is eight clouds, which is the most a 5-tick edit
repeat and a 40-tick maximum lifetime can put in the air at once; past it the *newest* spawn is
refused and counted, because a cloud vanishing mid-flight looks like a bug where a missing one
looks like nothing. On the heap because it is 67 KB and a 3DSX main thread has 32 KB of stack --
`-Werror=stack-usage` caught that the moment it was a local, which is the guard doing its job.

#### What is not done in step 7

- **Still nothing on hardware.** Both targets build, **823 host tests pass**.
- **No panning.** Named above; it needs a listener orientation at the backend seam.
- **`Block.onEntityWalking`** -- redstone ore lighting when trodden on -- is the one line of the
  footstep block not ported, because `move()` takes a const world.
  *(Done since, and that reading was short by one override: farmland is trampled there too.
  See section 42.)*
- **The fluid current still does not carry you**, unchanged from step 5.
- **No other sound has an emitter yet.** `random.fizz`, the fire loops and the ambient cave
  counter are all reachable now and all still named in [audio-a1.1.2.md](audio-a1.1.2.md).
- **Particles are only ever block-breaking ones.** a1.1.2 also throws them on landing, under a
  sprinting player, off a torch and out of a fire; every one of those needs a display tick that
  does not exist.

### 8. Four things a play session found: a lever that deleted itself, wire that never connected, footsteps from the sky, and a drop button

The fourth play session, and three of the four turned out to be a *harness* being wrong rather than
a transcription being wrong -- which is the shape of bug this project is meant to be resistant to,
so each one is written up with what the harness did and how it was caught.

#### The lever deleted itself when flicked, and the placement table put it down switched on

Two faults, one block.

**`faceSupported` answered false for orientation 6.** A floor lever is metadata 5 *or* 6 --
`BlockLever.onBlockAdded` writes `5 + rand.nextInt(2)`, which is which way round the handle lies --
and this port's support check had cases for 1 through 5 and `default: return false`. The original's
`no.a(Lcn;IIII)V` is a run of five `if (!isBlockNormalCube(..) && meta == n)` lines that *set* a
drop flag; a metadata the chain never names leaves the flag alone. So 6 is "still supported", not
"unsupported". Flicking notifies the lever's own cell, the notification came straight back to the
support check, and half of all floor levers removed themselves mid-flick.

**And the placement table was measuring a punch.** `tools/genref.java --place` places each block and
then called `ly.b(Lcn;IIILdm;)V` on it, on the reading that a1.1.2's controller runs
`onBlockPlacedBy` after `ItemBlock.onItemUse`. It does not -- **a1.1.2 has no `onBlockPlacedBy` at
all**. `Block` takes an `EntityPlayer` in three methods and that one is `onBlockClicked`, reached
from `PlayerController.clickBlock`, which is the *left* button starting a break. The sweep was
placing every block and then hitting it. Four rows of the shipped table were the hit:

| block | was | is | what the punch did |
|---|---|---|---|
| lever | 13/14 on the top face | 6 | flipped it on |
| stone button | 9…12 | 1…4 | pressed it |
| wooden door | 4 | 0 | opened it |
| redstone ore | -1 everywhere | 0 | turned it into id 74, which the sweep read as "did not survive" |

`onBlockAdded` fills in what a `(id, face)` table cannot: the underside's row, which
`onBlockPlaced` leaves alone and so is 0, and the floor lever's `5 + rand.nextInt(2)`. The second
of those looks cosmetic and is not -- `no.c(Lcn;IIII)Z` names orientations 1 to 5 and **not 6**, so
a floor lever lying the second way round hands no direct power to the block it stands on. It still
powers wire beside it; that goes through the indirect answer, which only reads bit 3.

The same fix settled the long-standing "only the lever consults the player's heading" note in
[physics-a1.1.2.md](physics-a1.1.2.md). It never did. The 13/14 alternation across sixteen headings
was `nextInt(2)` on an unseeded world; the sweep now reseeds `World.rand` per case, and
`kPlacementYawVaryingCount` is **0**. Nothing in a1.1.2 orients from where the player is standing.
`ItemDoor.onItemUse` reads the heading, and it is an item.

**The build did not notice the new table.** `CMAKE_CONFIGURE_DEPENDS` listed `blocks.json` and
nothing else, so `build-host/gen/placement.hpp` kept the old numbers through a rebuild and the
suite failed against a fixture that was right. `items.json`, `placement.json` and `selection.json`
are listed now.

#### Redstone wire drew the crossing tile, always, unlit

The note in step 5 said the tint was missing "because the detail vertex has no per-quad tint". That
was describing a later version's renderer. **a1.1.2's `bc.e` calls `setColorOpaque_F(f, f, f)`** --
brightness on all three channels, no tint anywhere -- and the glow is a *texture*:
`kf.a(II)I` is `blockIndexInTexture + (metadata > 0 ? 16 : 0)`, one row down in terrain.png. Tiles
84/85 are the dark cross and line, 100/101 the lit pair. Verified by decoding terrain.png and
printing the four.

So the wire now does what the class file does: `isPowerProviderOrWire` on all four horizontal
neighbours (one down where the neighbour is not a solid, one up where it is), the line tile for a
straight run and the crossing tile otherwise, unconnected arms of a crossing pulled back 5/16 of a
block with their UV edge, the lit row when the metadata is non-zero, and a vertical sheet up a solid
neighbour that has wire standing on it. `tick::canProvidePower` is public for this: the renderer and
the circuit have to agree about what "connected" means, and two copies of that list is how a wire
gets drawn reaching towards something it does not power.

#### The lever's handle was broken at the top, and it was the UVs

The handle was an upright box through `addBox`, which derives a face's UV from the box's own bounds.
Tile 96 is a two-texel strip in columns 7-8, rows 6-15, and the top six rows of it are transparent
-- so a box asking for the whole tile drew nothing over the top third of the handle.

`bc.c` takes a fixed patch: texels 7…9 across, 6…16 down for the four sides and 6…8 for the two
ends. It also *swings* the handle, through eight corners it rotates -- 0.69813174 radians for the
throw about x, a quarter turn for the second floor orientation, a right angle plus a quarter turn
per wall -- and every one of those goes through `addDetailQuad`, which takes arbitrary corners. So
the lever is a transcription now rather than the file's second honest simplification; fire is the
only one left. `MathHelper::sin`/`cos` do the rotation, not `<cmath>`, for the reason everything
else in this port uses the table.

#### Flying paid out a burst of footsteps over low ground

`Entity.moveEntity` banks `distanceWalkedModified` unconditionally and spends it only where the cell
under the feet is not air. That is a1.1.2 exactly, and it is why a fall across ground pays its steps
out one at a time afterwards -- which the report agreed was right. But a1.1.2 has no flight, so
`moveEntity` never runs on an airborne player there and the question never arises; here it did.
Crossing the sky banked a block of credit per block travelled and cashed the lot in the moment the
player skimmed a floor.

`tickFlying` now saves and restores the two counters around its `move()` and clears `stepSoundDue`.
It is a rule of ours, and it is marked as one in the source. Walking after a flight still steps on
its own schedule, which is the other half and has its own test.

#### A drops one of what is in the hand

`InventoryPlayer.decrStackSize(currentItem, 1)`, which is the half of a drop that is not the
entity. **Nothing landed on the ground at first** -- there were no item entities -- so the item was
spent rather than dropped, and the method said so rather than being called `drop` and quietly
deleting. Step 9 gave it somewhere to land.

A is the button that was going spare: focused, it is the bottom screen's pick, and the drop is on
the unfocused branch. It also overlapped with flight's held sprint, which step 9 removed.

#### What is not done in step 8

- **Still nothing on hardware.** Both targets build, **837 host tests pass**, and the
  ThreadSanitizer build compiles clean -- the mesher runs on the worker and this touched it.
- ~~A floor lever always lands on orientation 6~~ -- the roll is in `tick::leverPlaced` now, which
  is `BlockLever.onBlockAdded`. It had to be: `no.c` names orientations 1 to 5 and **not 6**, so a
  floor lever lying the second way round hands no *direct* power to the block it stands on -- a
  a1.1.2 quirk that a pinned table would have given every lever in the game.
- ~~**Nothing is dropped in the world.**~~ -- item entities landed in step 9. A broken block still
  leaves nothing, and that is now a *table* rather than an entity: `idDropped` and
  `quantityDropped` off every block.
- **Fire still does not flap**, unchanged from step 5.
- **The wire's climb is drawn but the wire itself still has no bottom-face cull**; every sheet in
  `shapes.cpp` is drawn from both sides, which is a quad the original does not emit.

### 9. One flight speed, and something to pick back up

Two asks off the back of step 8's drop button, and the second is the first entity in this project
that is not a particle.

#### The held boost went, and A is only the drop now

Creative flight copied Spectator's held boost onto A, which put a modifier on the one face button
Creative had going spare -- and step 8 had just made that button the drop. The overlap worked (the
boost read `held` and the drop read `down`) and read as an accident, which is what it was.

`kFlightSprintSpeed` is gone and `tickFlying` takes one speed. Spectator keeps its boost on X:
Spectator is a camera with nothing to hit, and Creative flight collides, places and breaks, so a
held speed control is the wrong shape for the second in a way it is not for the first.
`flight_is_stopped_by_a_wall_at_both_speeds` became
`flight_is_stopped_by_a_wall_even_faster_than_a_block_a_tick` and kept its over-speed case: what
that test proves is that the **sweep** is safe for a fast mover, and dropping the fast case with the
fast mode would have thrown that away.

#### `EntityItem`, transcribed

`dx` in the jar. The constructor, `e_()` (onUpdate), `b(dm)` (onCollideWithPlayer) and
`dm.a(Lev;Z)V` (dropPlayerItemWithRandomChoice) are all in `core/entity/item_entity.cpp`, constants
and order included:

| | |
|---|---|
| box | `setSize(0.25, 0.25)`, `yOffset = height / 2` -- so the position is the box's **centre** |
| pull | 0.04 a tick |
| drag | 0.98 on y; on x and z the floor's own slipperiness times 0.98, and `0.58800006` over air |
| bounce | `motionY *= -0.5` on landing |
| lifetime | 6000 ticks, five minutes, a hard `>=` |
| pickup delay | 5 from the constructor, **40** when a player threw it |
| throw | 0.3 along the look vector, `+0.1` on y, then a 0.02 scatter |
| spawn height | `posY - 0.3 + getEyeHeight()`, and `getEyeHeight` is **0.12** on `EntityPlayer` |

**It moves through the shared sweep** in `core/entity/sweep.hpp`, the same one the player and the
digging particles use -- so an item lands on a slab, slides down ice and stops in a corner with no
line here saying so. And the box is the authority with the position derived from it, which is the
trap `Particle` documents at length: rebuild the box from a rounded centre and its bottom sits a
fraction of an ulp below the block it landed on, at which point `calculateYOffset` declines to stop
it and the item falls out of the world. `an_item_does_not_fall_through_the_world_it_lands_on` is
that test.

**a1.1.2 has no drop key.** `dropOneItem` does not exist in this version and the only callers of
`dropPlayerItem` in the jar are the inventory screens spilling the stack on the cursor. So the
button is ours and the throw under it is the game's -- the same split Creative itself is.

**The entity is spawned before the stack is spent.** The pool holds 64 and refuses the newest past
that, the rule the particle pool and the tick scheduler already follow; taking the item off the hand
for an entity that was refused would have destroyed it.

#### Picking one up

`addItemStackToInventory` is `Inventory::addStack`, and it is the one piece of this that had a
surprise in it. The original takes an `ItemStack` and edits its `stackSize`; this takes a count and
returns the leftover, which says the same thing without a caller having to know its argument was
mutated -- and `true` in the original is exactly `leftover == 0` here.

- **`storePartialItemStack` tries one slot**, not the array: the slot already holding this item with
  room in it, or failing that the first empty one. What will not fit comes back and
  `addItemStackToInventory` drops the *whole* remainder into the first gap -- which is why a count
  past an item's own maximum can sit in a slot, exactly as it can in the original.
- **Damaged stacks do not merge.** `if (itemstack.itemDamage == 0)` is the method's first line.
  Nothing wears out in this build yet, so every stack takes the merging path, but the rule is
  written rather than assumed away.
- **All of it or none of it.** The entity is removed only when the stack went in whole; a partial
  fill leaves it holding the remainder and the player walks over it again next tick, which is what
  stops a full inventory eating what it could not carry.

The reach is `EntityPlayer.onUpdate`'s: the player's own box expanded by **one block horizontally
and nothing vertically**, so an item is reached from a step away sideways and never through a floor.
`random.pop` plays per pickup at volume 0.2, pitch `((r - r) * 0.7 + 1) * 2`.

#### Drawing it, and the second texture that needed

`ab.a(Ldx;DDDFF)V` -- `RenderItem.doRenderItem` -- draws two different things, and so does
`core/render/item_entity_mesh.cpp`:

- an item that places a block whose render type `RenderBlocks.renderItemIn3d` answers true for is
  drawn as **that block, quarter size, spinning about its own vertical axis**. It asks
  `block::renderBoxes` rather than carrying a second copy of the render-type list, which is what
  keeps a fence in the hand, a fence in a slot and a fence on the ground the same shape;
- everything else is a **flat sprite, half a block across, turned to face the camera about the
  vertical axis only**. `glRotatef(180 - playerViewY, 0, 1, 0)` and no pitch term, so a sword on
  the ground stays upright when you look down at it.

The bob and the spin are `sin((age + partial) / 10 + hoverStart) * 0.1 + 0.1` and
`((age + partial) / 20 + hoverStart) * 57.295776` degrees, both off the entity's own random phase so
a heap does not pulse in unison. A stack draws as 1, 2, 3 or 4 copies past 1, 5 and 20, and the
jitter between them is seeded `187` **per entity** -- that literal is the point, not an accident: it
is what makes a given pile look the same from frame to frame instead of shimmering.

**gui/items.png went to the GPU for this**, as `Atlas::initItems`. Before it, only the bottom
screen's software rasteriser read that sheet. It goes to ordinary linear memory rather than VRAM --
the block atlas earns VRAM because every fragment of every chunk samples it, and this is a handful
of quads a frame -- and the item pass is two draws with the atlas rebound in between, because the
PICA takes one texture per draw. A pack with no items.png draws no sprites at all rather than
falling back to a terrain tile that would be a lie about what is lying there.

#### What is not done in step 9

- **Still nothing on hardware.** Both targets build, **855 host tests pass**, and the
  ThreadSanitizer build compiles clean.
- **A broken block still drops nothing.** That is `idDropped` and `quantityDropped` off every block
  -- a generated table, and a derivation of its own -- rather than anything the entity is missing.
- ~~**`handleWaterMovement` is skipped**~~ -- **done in step 10.** The flow field moved into
  `core/block/fluid_flow.hpp` and both the player and the item read it.
- **No `random.fizz`** when an item lands in lava. The throw-back is there; the sound is on
  [audio-a1.1.2.md](audio-a1.1.2.md)'s list with the rest of the unported ones.
- **Items are not saved.** They live in the pool for the session and are gone when the world is
  closed, which is a1.1.2's `entities.dat`-shaped gap and needs the chunk entity list rather than a
  pool.
- **They do not merge**, which is a1.1.2's own behaviour. Step 10 transcribed a later version's
  `combineItems` anyway and step 35 took it out again; the version bound is measured there.

### 10. Six things a play session found: plates, currents, heaps, and fire that stood still

Six reports, and they turned out to be five different kinds of missing. Two were a mechanism
transcribed with its *input* left unbuilt (a pressure plate that could not see entities; water that
could not push one). One was a renderer drawing half a block's shapes. One was a texture that ran
once and stopped. Two were smaller: how drops behave, and eight pixels of a bevel.

**899 host tests pass**, and both targets build clean.

#### Pressure plates -- the seam a block behaviour needed and no other has

`al` was already here: its support rule, its `canProvidePower` (straight up and nothing else), its
`indirectlyProvidesPowerTo` (everywhere), its tick rate of 20. What was missing was the only thing
it actually does, `al.h(Lcn;III)V` -- setStateIfMobInteractsWithPlate -- because that method asks
the world a question no other block behaviour asks: *is anything standing here*.

So `TickWorld` grew one hook, and it is deliberately not on `TickAccess`. `TickAccess` is how the
tick reaches **chunks** and is built by `WorldStreamer`, which owns them; nothing owns the player
and the dropped items together except the frame loop, so `setEntityQuery` is set there, once. Unset
means "there are no entities", which is the honest answer for every headless tool in this project
and is exactly what a plate did before this existed.

| | |
|---|---|
| sense box | `(i + 0.125, j, k + 0.125)` to `(i + 1 - 0.125, j + 0.25, k + 1 - 0.125)`, and the arithmetic is the class file's floats, not doubles |
| arm | `al.b(Lcn;IIILkh;)V` from the block-collision scan, and **only when the plate is up** |
| disarm | `al.a(...Random)` on its own scheduled update, and **only when it is down** |
| re-schedule | while something is on it, which is the twenty ticks it stays down after you step off |
| writes | `setBlockMetadata` (which notifies nothing) then `notifyBlocksOfNeighborChange` on itself and on the block *below* |

**The two plates are two tick behaviours now**, `pressure_plate_all` and `pressure_plate_mobs`. `al`
takes a `js` -- EnumMobType -- as its third constructor argument, and the jar passes `js.b` ("mobs")
for block 70 and `js.a` ("everything") for block 72. That is the whole observable difference between
them: **a stack of dirt thrown on to a wooden plate presses it and the same stack on a stone plate
does nothing**, because a dropped item is neither a mob nor a player. Two behaviours rather than one
behaviour and a new generated column, which is the shape the table already uses for a flowing fluid
against a still one.

`tick::entityCollidedWithBlocks` is `moveEntity`'s tail: every block whose cell the entity's box
overlaps gets `onEntityCollidedWithBlock`, with the original's **inclusive** floor bounds rather
than the half-open range every other box walk here uses. It is called from the frame loop and not
from inside `PlayerBody::move`, because that method takes a `const TickWorld&` on purpose -- moving
a body must not be able to write blocks.

#### The flow field had one reader and needed two

`jp.e(nm,III)` -- BlockFluid.getFlowVector -- has been in this tree since the mesher needed it, to
spin a flowing block's top texture. The other caller was missing: `cn.a(cf,gb,kh)Z`
(World.handleMaterialAcceleration) sums that vector over every water cell an entity's box touches,
normalises the total and adds **0.004** of it to the motion every tick. That is the entire mechanism
by which a river carries you, and without it water was scenery.

The transcription moved to `core/block/fluid_flow.hpp`, templated on the accessor rather than
duplicated: the mesher asks a `MeshScratch` in local coordinates and an entity asks a `TickWorld` in
world coordinates, and both answer `blockAt` and `dataAt` and nothing else. Two copies of one method
against two world types is exactly the pair that drifts, and the drift would be silent -- water that
pushes one way and paints its texture the other.

Three details that are the original's and look like bugs:

- **The push is normalised twice.** Each cell contributes a unit vector and the sum is normalised
  again, so a wide box in a big river is pushed no harder than a narrow one. The cells decide the
  *direction*; never the speed.
- **The surface test compares against the loop bound**, `(double)l >= d1`, where `l` is the box's top
  in cells -- so every fluid cell in the box is measured against one height rather than its own.
- **A player holding jump in a current is pushed twice that tick.** `ge.j()` calls
  `handleWaterMovement()` for the jump branch and `ge.b(FF)` calls it again for the movement branch,
  and the method pushes as a side effect of answering. `PlayerBody::inWater` used to be a const
  query asked once; it is `handleWaterMovement` now and is asked exactly where the original asks it.

**Lava carries nothing**, and that is not an omission: `kh.G()` is `isMaterialInBB`, which has no
vector in it at all.

#### Fire had one shape and needed three

`bc.d` branches on what is under and around the cell before it draws a vertex, and the branch is not
cosmetic. `isBlockNormalCube(i, j-1, k) || canBlockCatchFire(i, j-1, k)` gives **floor fire** -- eight
leaning sheets, 1.4 blocks tall, in two sets at ±0.2/±0.3 and ±0.4/the cell wall. Anything else gives
**wall fire**, which is a different set of quads pinned to whichever of the four sides and the
ceiling can burn. This drew an approximation of the first in every case, so a fire eating the side of
a house floated in the air beside it.

`canBlockCatchFire` is `chanceToEncourageFire[id] > 0` -- the `burnEncourage` column
`core/tick/fire.cpp` already spreads by. So the shape a fire draws and the blocks it will spread to
are one fact, and a wall flame is drawn against exactly the faces it is eating.

Three things worth writing down:

- **Fire in mid-air with nothing to burn draws zero quads** -- in the original too. It is never seen,
  because such a fire goes out on the next tick. Two shape tests had to be told to give fire a floor.
- **A flame is 1.4625 blocks tall**, so it legitimately leaves its own cell upwards.
  `no_new_shape_leaves_its_own_cell` now says so instead of passing by accident.
- Two parities, `(i + j + k) & 1` and `(i/2 + j/2 + k/2) & 1`, pick which of the two flame tiles a
  wall face starts on and whether its texture is mirrored. They change no geometry and are the whole
  reason a burning wall does not look like wallpaper.

#### ...and it stood still, because the atlas is in VRAM

`applyAnimatedTiles` settled `TextureFlamesFX` once at pack load and stopped there, for a stated
reason: the animation needs the atlas updated 20 times a second on a console where it lives in VRAM,
which the CPU cannot store into.

The way through is the PICA's own layout. A 16×16 tile is four of the GPU's 8×8 tiles, and the pair
side by side is **adjacent in memory** -- so a tile is two runs of 128 words, 512 bytes each, and
updating one is two small texture copies rather than a 256 KB re-upload. `tileRunsFlipped` and
`tileRunIndex` are in `core/texture/tiled.hpp` **so the host suite can pin them**, which is the same
reason `tiledOffset` is there: `platform/ctr/` is compiled only for the console, and the last atlas
upload defect hid there for exactly that long. Four tests, including "a tile's runs hold only that
tile" -- a run that reached past its tile would corrupt a neighbouring texture every time fire
ticked.

`texture::FlameAnimation` is the two simulations kept running, stepped on the **world's** clock and
uploaded once a frame -- the same split the particles and the music counter use, so a console at 24
fps and one at 60 see the same fire. It starts settled rather than black. Not measured on hardware:
four `C3D_SyncTextureCopy` calls of 512 bytes a frame is the cost, and what that is in microseconds
is a console question.

#### Dropped items: heaps, and a clock that only runs while somebody is there

Two changes, and the first is **openly not a1.1.2's**. `dx.e_()` in this version has no merge step
-- ground merging is a later version's `combineItems`, dated to Beta 1.8 here and **wrong**: step 35
measured it and reverted this change. The paragraph is kept as written so the reversal has something
to point at. It read: so two heaps of dirt thrown side by side stayed two heaps for the five minutes
they lived. That is faithful, and it was asked to change, so the later version's method is
transcribed and marked rather than invented: the bigger stack absorbs the smaller, the survivor
keeps the **younger** age and the **longer** pickup delay, damaged stacks do not merge, and the scan
runs every 25 ticks inside a box expanded 0.5 horizontally.

The second closes a gap that only exists because of how this port is built. a1.1.2's entities live
*in* chunks and an unloaded chunk has none of them to tick; ours are a flat pool that outlives the
columns under it, so a drop left behind while the player walked away quietly spent its five minutes.
**An item whose column is not resident does not tick at all** -- no physics, no light resample, and
above all no ageing.

A death empties a stack rather than removing it, and a sweep at the end of the tick reclaims them:
the walk holds references into the pool and `removeAt` swaps the last entry into the hole.

#### The palette's page arrows

`text` paints its own background across every column it claims, and the arrows asked for two columns
on a sixteen-pixel slot -- so the fill covered the slot edge to edge and painted over both halves of
the bevel. One column is eight pixels and stops four short on each side. A character cell cannot
start half way through the 8-pixel grid, so the *box* is placed around the cell rather than the other
way round, and two `static_assert`s pin it.

#### What is not done in step 10

- **Still nothing on hardware.** Both targets build and 899 host tests pass.
- **No sound from a plate.** `random.click` at 0.3/0.6 arming and 0.3/0.5 disarming; core has no
  sound engine, and it joins the list in [audio-a1.1.2.md](audio-a1.1.2.md) beside the button's.
- **`js.c` ("players") is modelled and unused.** No block in a1.1.2 takes it.
- **The entity query knows about the player and the dropped items only.** There is nothing else to
  know about yet; a mob would be a third row and no more.
- **Fire still has no back-face story of its own.** Every sheet goes through `addSheet`, which draws
  it and its mirror, where the original relies on having eight sheets at four leans. It costs quads
  on a block there are never many of.

### 11. Nine things a play session found, and one of them was two years of Minecraft history

A second report, longer than the last and with a wider spread: two behaviours transcribed and never
wired, one whose data table did not exist, one entity that had a name and no class, a screen whose
two bands were the wrong way round, a catalogue with two thirds of its rows hidden, and **two
complaints that turned out to be a1.1.2 behaving exactly as it should**.

**946 host tests pass**, and both targets build clean.

#### Ladders: three faults on one block

Reported as "ladders aren't climbable, have a full hitbox for placing, z-fight, and I don't think it
was a thin cube in alpha". All three were real and none of them shared a cause.

- **Climbing did not exist.** `ge.b(FF)`'s land branch asks `isOnLadder()` twice -- once before the
  move, where it clears `fallDistance` and clamps a fall to `-0.15`, and once after, where
  `isCollidedHorizontally && isOnLadder()` sets `motionY` to `0.2` outright. Both lines were in
  [physics-a1.1.2.md](physics-a1.1.2.md)'s transcription and neither was written. `ge.A()` reads
  **two** cells, `floor(boundingBox.minY)` and the one above it; without the second, the feet leave
  a ladder's top cell before the chest does and the last block of every climb drops you.
- **Placing asked about the wrong shape.** `canBlockBePlacedAt` tests the new block's collision box
  against the player, and this port asked for that box at **metadata 0** -- which for a ladder is
  outside the 2..5 the game writes and is the one value `collision.cpp` answers with a *full cube*
  for. A full cube in the cell in front of you overlaps the body, so a ladder could not be hung on
  the wall you were standing against. It asks with the metadata `onBlockPlaced` is about to write
  now, which is deterministic and needs no leftover state.
- **It was drawn as its collision box.** `bc.g` writes **four vertices** and stops: one flat quad,
  `0.05F` off the wall. The box has six faces, and its back one sat inside the wall and z-fought
  with it. The only deviation left is that the quad is drawn from both sides -- the original relies
  on the wall behind it being opaque, so a ladder on glass is invisible from outside in a1.1.2.
- And **a ladder whose wall went stayed hanging in mid air**, because `TickBehaviour::Ladder` had no
  `neighbourChanged` entry at all. `br.a(Lcn;IIII)V` checks the face its metadata names and drops.

#### Rails: `mk`, the largest block behaviour in the version

Reported as "rails don't connect or change rotation", and both halves were true for different
reasons.

**The rotation was the renderer's.** `bc.f` keeps one fixed set of texture corners and permutes
which *cell* corner each one lands on -- four permutations across the ten shapes -- and this port
kept one. So every rail in the world was drawn as the north-south one. Worse, the two ascending
shapes on the x axis were the wrong way round: **2 climbs towards +x and 3 towards -x**, and this
had them reversed, so an east-west staircase climbed backwards and met its neighbour in mid air.

**The connecting was missing outright.** `Block.onBlockPlaced` answers 0 for a rail -- which is why
`placement.json` has six zeroes on that row -- because the shape is not a placement decision: it is
worked out afterwards by `mk`, RailLogic, which is 1,300 lines of bytecode and the largest single
behaviour in a1.1.2's block set. It is transcribed whole in `core/tick/rail.{hpp,cpp}`. Four things
in it are worth keeping:

- **A freshly placed rail is written with metadata 15 first.** `if.e` does that before it refreshes,
  and 15 matches none of `setBasicRail`'s ten cases, so the connection list comes out empty. It is
  "I have no previous shape to be biased by", spelled as a metadata value because a1.1.2 has nowhere
  else to put it.
- **Power reorders the curve preferences.** `refreshTrackShape` takes
  `isBlockIndirectlyGettingPowered` and uses it for one thing: to flip the order the four corner
  cases are tried in, so the *last* match wins differently and a T-junction points the other way.
  That is a1.1.2's rail switch, two years before powered rails.
- **`canConnectFrom`'s tail is dead code.** The class file compares two heights and returns true on
  both paths, so the whole method is "yes unless I am already full". Transcribed as what it
  computes.
- Nothing recurses past a second frame, which matters on a 32 KB stack: `refreshTrackShape` builds
  a RailLogic per neighbour and a neighbour's `refreshConnectedTracks` builds its own only to read
  them.

Nine cases in `tests/rail_test.cpp`, and all nine passed the first time the file compiled -- which
is the strongest evidence available that the transcription is right, because none of them was
written by looking at this implementation.

#### What a block leaves behind, and the table that had to be generated for it

`dropBlockAsItem` had **nine call sites and an empty body**, with a comment saying so. Knocking the
support out from under a torch deleted the torch. The method itself is four lines; what was missing
was the two it calls, `Block.idDropped(metadata, Random)` and `Block.quantityDropped(Random)`, which
have **twenty-four and twenty overrides** between them and cannot be read as constants because both
take a Random.

They can be *characterised*, and that is what `tools/genref.java --drops` does: hand each method a
Random that answers every `nextInt(bound)` at the bottom of its range once and the top once, and
write down the bounds it asked for. Every one of a1.1.2's overrides draws at most once, so two calls
pin the whole distribution, and the generator asserts the "at most one draw" property rather than
assuming it. Four shapes come out:

| shape | example |
|---|---|
| a constant | stone drops one cobblestone |
| `min + nextInt(bound)` | redstone ore drops 4 or 5 |
| `nextInt(bound) == 0 ? v : 0` | a leaf block drops a sapling one time in twenty |
| two ids, one roll | gravel drops flint one time in ten |

Three things the table settled that were not obvious:

- **There is no damage column, because there is no `damageDropped` on this path.**
  `dropBlockAsItemWithChance` builds `new ItemStack(id)`, so wool does not keep its colour and a log
  does not keep its kind. That arrives later.
- **A snow layer drops nothing** -- `quantityDropped` is 0 -- and neither do glass, ice, bookshelves,
  TNT or a mob spawner.
- **`BlockCrops.idDropped` prints to stdout.** `System.out.println("Get resource: " + l)` is a debug
  line left in the shipped jar, and it had to be silenced for the length of the probe because stdout
  is where the JSON goes.

The draws come out of the **world's own** Random, so a drop shifts the stream every later random tick
reads. That is why `TickWorld::spawnItem` is a seam and why the draws happen whether or not anything
is listening: a world ticked by a headless tool takes the same random path as one ticked with a pool
behind it, and there is a test that says so.

#### Sand that falls rather than teleports

`fallingTick` moved the block to its resting place in one tick and admitted it: "a placeholder with
a known replacement". `core/entity/falling_block.{hpp,cpp}` is the replacement -- `ff`,
EntityFallingSand, the second entity here that is not a particle.

**The instant path stayed, and it is not a fallback.** `dh.a` is a static boolean on BlockSand,
`fallInstantly`, true while a chunk is populated, and with it set `tryToFall` ticks the entity to a
standstill instead of spawning it -- which is what stops a generated world raining sand as a player
walks into it. This port reaches the same fork from the other side: an unset spawn seam means nobody
is watching, which is true of world generation and of every headless tool here.

Three details of `ff.e_()` that are easy to miss: it **clears its own source cell from inside its
tick** rather than at spawn, so a column empties one cell at a time; the landing writes at
`floor(posY)`, and `posY` is the box's centre because `yOffset` is half the height; and it gives up
after 100 ticks and drops as an item. The landing write can also **fail** -- something in the way --
and then the block becomes an item rather than overwriting, which is what stops sand falling into a
doorway from eating the door.

A full pool is the one place in this project that degrades rather than dropping something: the spawn
is refused, `fallingTick` takes the instant branch, and the block still lands where it belongs.

#### The bottom screen: the two bands changed places

Asked for, and both halves are improvements on what was there. The **hotbar is the top band now** and
runs edge to edge; the **tab strip is the bottom band**.

The hotbar is the one control on that screen read while the player is looking at the *top* screen --
it is what is in your hand -- so the top of the bottom screen is as close to the world as a second
screen can put it. The tab strip is looked at deliberately, so the bottom costs least.

**Nine slots across 320 pixels is 35.55, which does not divide.** The slots are therefore not all
the same width: `hotbarSlotX(i)` is `i * 320 / 9`, so the widths come out 35 or 36 and the row lands
exactly on both edges. Nine 35s centred would have left five pixels of nothing at the edge, and five
pixels of nothing at the edge of a touch target is five pixels a finger can miss. The hit test is a
search rather than a divide for the same reason.

Everything that took its geometry from `kHotbarTop` now takes it from `kTabTop` -- the map, the
palette, the look pad -- and the cursor's d-pad walk **flipped**: off the *top* row of a grid is the
hotbar, and down from the hotbar enters the grid, because that is now where the two things are.

#### The inventory: the whole page, and the armour

The Items page was a 232x112 panel in the middle of a page with the pack's darkened dirt showing on
every side of it, which reads as an unfinished screen. It is the page now: 316x164, 30-pixel slots
(56 % more area than the 24s), 24-pixel icons, and **the four armour slots down the left**, where a
paper doll would be.

Those four have been in `Inventory` since the item table landed and round-trip through `level.dat`;
what was missing was somewhere to draw them. `armorInventory[0]` is the **boots** and `[3]` the
helmet, so the column is drawn in reverse index order -- inverted in the screen rather than in
`Inventory`, because the save file's order is the save file's.

30 rather than 32, because nine columns and an armour column have to share 320 pixels and nine 32s
is 288 with nothing left. The 24-pixel icon is a trade rather than a free win: `drawFlat` is
nearest-neighbour and 24 doubles every other row of a 16-pixel sprite, where 16 was 1:1. What it
buys is that the icon fills the slot. The cube path does not pay it at all -- `drawBox`
inverse-maps each destination pixel and is exact at any size -- and cubes are most of what a slot
holds.

#### The palette offers the whole table now

The rule was "nothing that places no block is offered", on the argument that a sword does nothing
this build can perform. That was the wrong test: a Creative hand is also how a sword, an ingot, a
smelted ore or a piece of armour gets into a chest, into a save, or on to the ground. **84 of its
147 rows were hidden and 63 were offered.** The two exclusions that remain are both about the same thing appearing twice --
engine-only ids, and an ItemBlock whose block already has a carried form -- and neither hides
anything a player could otherwise not reach. a1.1.2's palette is four pages rather than two -- 147 items rather than 63. *(149 since the two music discs joined it -- status.md 26.)*

#### Two reports that were a1.1.2 being itself

Both are worth writing down because both look exactly like bugs.

- **Sponges do not remove water, and have not since Classic.** `ng.e` -- onBlockAdded -- walks a
  5x5x5 box comparing each cell's material against water, and **the body of that comparison is
  empty**: the branch target is the next instruction. The absorption everybody remembers is
  Classic's and then, five years later, 1.8's. The only thing `ng` does in this version is
  `onBlockRemoval`, which notifies the same 5x5x5 -- and *that* was missing here and is wired now.
- **Fire does not burn dropped items.** Nothing in a1.1.2 sets an entity's fire counter except one
  branch of `Entity.onEntityUpdate`, and that branch is `isInLava()`. `og` has no
  `onEntityCollidedWithBlock` at all, and no class outside `kh` writes `kh.aT`. What *was* missing is
  the lava half: `attackEntityFrom(null, 10)` against `EntityItem`'s health of **5**, so one tick in
  lava is the end of the stack. The fizz and the hop were already here, which is why the loss looked
  like a bug -- an item thrown into lava jumped about convincingly and then lay there for five
  minutes.

Lava also **carries nothing**, and that is not an omission either: `kh.G()` is `isMaterialInBB`,
which has no vector in it, and nothing in the jar calls `handleMaterialAcceleration` with anything
but water. Water pushes; lava does not, in this version.

#### A pressure plate that felt unresponsive was a pressure plate with no sound

The mechanism was transcribed and correct -- the sense box, the twenty-tick release, both
notifications, the two `js` filters. What it had no way to do was **say so**: a plate is flush with
the floor and its whole state is one bit of metadata, so the click *is* the feedback. The lever, the
button and the door were in the same position and had the same comment saying the sound was missing.

`TickWorld` grew a third seam for it, `setSoundSink`, on exactly the terms the entity query and the
drop sink have: `core/tick/` has no sound engine and must not grow one -- the tick runs on a worker
and the mixer does not. Six calls now go through it, all with the class file's own volumes and
pitches: the plate at 0.3/0.6 arming and 0.3/0.5 disarming and **at `j + 0.1`, not `j + 0.5`**; the
lever at 0.3, pitched 0.6 on and 0.5 off; the button at 0.6 going in and 0.5 coming out; and the
door's `random.door_open`/`random.door_close` at volume 1 and a pitch drawn from the world's Random
-- which is a draw, so opening a door really does move the stream.

Flint and steel got its own path at the same time. It is `nx`, not `av`: **no clearance test, air
only, and `fire.ignite` rather than the block's place cue.** The first of those is why a fire lit on
a stone wall appears and then goes out on the same call rather than being refused -- which is the
original's sequence and is what "it instantly goes off" is supposed to look like.

#### What is not done in step 11

- **Still nothing on hardware.** Both targets build and 946 host tests pass. The new geometry --
  four falling-block quads and a hotbar that is 30 % wider -- is unmeasured on a console.
- **Nothing wears armour.** The four slots hold, draw and save; there is no damage for a helmet to
  reduce, so a helmet in slot 103 is carried and does nothing. Honest, and still better than a
  helmet that cannot be put anywhere.
- **A falling block is always a cube.** `renderBlockFallingSand` goes through
  `renderBlockByRenderType`, so a version whose falling block is some other shape would need the
  shape table in `falling_block_mesh.cpp`. Sand and gravel are both cubes.
- **Cactus does not hurt anything.** `hy` is the *second* block to override
  `onEntityCollidedWithBlock` -- the note in step 10 said there was only one -- and it wants the
  entity, which the seam deliberately does not carry. A third seam or a wider one, and neither is
  worth it before there is health.
  **Closed later**: the seam is `core/entity/block_contact.hpp`, which is the same shape
  `fire_entry.hpp` has -- the pool runs the loop over its own box and applies the hits through its
  own `attackEntityFrom`, so no seam has to carry an entity. The block table gained a `Contact`
  column for it. See `docs/current-work.md`.
- **Breaking a block by hand still drops nothing.** The drop machinery is wired to the nine tick
  paths that call it; `onPlayerDestroyBlock` -> `harvestBlock` is Survival's and Survival is the
  gamemode drawn disabled.
- **A sign still has no text.** It falls off a wall correctly now and takes its writing with it,
  which is what a1.1.2 does too.

### 12. Six things a play session found: a ladder measured from the wrong method, and dropped items drawn from a buffer written twice

A play-session list of thirteen. Six of them are fixed here; the other seven are classified at the
end, because three of them turn out to be **a1.1.2 behaving exactly as reported** and three are the
entity renderer that step 5 of `docs/todo-m3.md` has been waiting on.

#### The ladder's selection box was a full cube, and the generator was reading the wrong method

`data/<ver>/selection.json` said a ladder is 0,0,0 -> 1,1,1 at every one of its sixteen metadata
values, so the outline round one was a whole block and a ray could not be aimed past one. That was
not a transcription slip: it was the definition of "the selection box" being wrong in
`tools/genref.java`.

The generator took the box a ray is tested against, which it obtained by calling `collisionRayTrace`
for its side effect and reading the `bf..bk` fields it left on the block singleton. That is right
for a torch -- `BlockTorch` overrides `collisionRayTrace` and sets its bounds inline before
delegating -- and it is **wrong for three blocks that set their bounds somewhere else entirely**.
Asking the jar which classes override what:

| class | `collisionRayTrace` | `getSelectedBoundingBoxFromPool` |
|---|---|---|
| `br` ladder | — | **yes** |
| `hy` cactus | — | **yes** |
| `km` stairs | — | yes (delegates to the model block) |
| `fw` door | yes | yes |
| `if` rail, `mj` | yes | — |

`br` sets its two-sixteenths box in `d` (getCollisionBoundingBoxFromPool) and `f`
(getSelectedBoundingBoxFromPool) and touches nothing on the ray's path, so a generator that restores
each block's constructor defaults before every query -- which this one does, deliberately, to make
the fixture reproducible -- records the ladder's *constructor* cube.

**The fix is to ask both, in the order a frame asks them.** `Minecraft` ray-traces and then
`RenderGlobal.drawSelectionBox` calls `f` on whatever was hit, so the generator now runs
`collisionRayTrace` and then reads what `f` answers. The torch keeps its post (`Block.f` falls
through to the bf..bk the torch just set) and the ladder gets its plate. Exactly two blocks moved in
the 1,120-row fixture: **65 at metadata 2..5, and 81 at every metadata** -- the cactus's selection
box is inset a sixteenth on x and z and is **full height**, where its collision box stops at 0.9375.

`tests/ray_trace_vectors.hpp` then disagreed on sixteen rays, and it was right to: its scene is a
world two statements old, where nothing has ever asked the ladder for a box, so the oracle recorded
the constructor cube as well. **That is the leftover-singleton bug this project has already decided
not to reproduce** (the same note is in the collision fixture's header). A running client never sees
it -- `drawSelectionBox` calls `f` every frame and `Entity.moveEntity` calls `d` every tick for every
block the body overlaps, and both leave the real box behind -- so the ray-trace generator now primes
every block in its scene through `f` before the sweep, which is the state a played game is always in.
Sixteen rays that used to stop on the ladder's phantom cube now pass through it or hit the wall
behind, and the suite is green.

#### Many dropped items were invisible, and no icon was missing

`Renderer::drawItemEntities` builds one sheet, draws it, then builds the other **over the same
vertices** and draws that. It reads as correct and it is not: `C3D_DrawElements` records a command
naming an address, and the GPU does not execute it until `C3D_FrameEnd`. Both draws therefore ran
against whatever the *second* `buildItemEntities` left in `itemVerts_`, so the terrain-sheet draw
rendered the item-sheet geometry with the block atlas on it.

The visible symptom is exactly what was reported: **a dropped cobblestone is simply not there
whenever anything off `gui/items.png` is on the ground beside it** -- and it is there when nothing
is, because a pass that writes nothing never overwrites the buffer. Both sheets are now built into
disjoint halves of the one buffer before either is drawn, and each draw gets its own base pointer.
The two spans cannot overflow between them: every entity belongs to exactly one sheet, so their sum
is bounded by what one pass over all of them could produce, which is what `kMaxItemVertices` already
sizes.

**Not seen on hardware.** This is a GPU-ordering fault and the host has no GPU, so what is checked
is the reasoning and the arithmetic, not the picture.

#### Any armour piece went in any armour slot

There was no rule at all -- `Inventory::swap` moved any stack anywhere -- so a helmet could be worn
on the feet. a1.1.2 has one, in `lj` (SlotArmor), and it is one line:
`stack.getItem() instanceof ItemArmor && ((ItemArmor) item).armorType == this.slotType`.

So `items.json` grew an **`armour` column measured out of the jar** (`ItemArmor.armorType`, `mr.aX`),
`Inventory::accepts` is that comparison, and `swap` refuses when either end would not hold what the
other is carrying -- refused whole, because a swap is one move and a slot that keeps its contents
while the other loses them would destroy a stack. The touch handler keeps the stack on the cursor
when a slot refuses it, which is what the original does too.

**armorType counts from the head and `armorInventory` counts from the feet.** `ContainerPlayer`
builds its four slots as `getSizeInventory() - 1 - i` against armorType `i`, so `armorInventory[3]`
is the helmet. The flip lives in `Inventory::armourSlotFor` and nowhere else.

#### The bucket, which is the first item to go down `Item.onItemRightClick`

Buckets did nothing, and they could not have: every right-click in this build went through
`ItemBlock.onItemUse`, and `ac` (ItemBucket) does not implement it. `Minecraft.clickMouse` has **two**
entry points -- the block's, which is handed the crosshair's hit, and the item's, which gets no hit
at all and casts its own ray -- and only the first existed here. `item::useItem` is the second.

Three measured things it needed, all now generated rather than assumed:

- **`ItemBucket.isFull`**, as a `bucket` column: `0` empty, a **flowing** block id when full -- 8 and
  10, not the still 9 and 11, which is why poured water spreads instead of standing in its cell --
  and `-1` for milk. `-2` is "not a bucket", the one value the field cannot hold.
- **`canCollideCheck(metadata, true)`**, as a second flags column in the collision fixture and a
  sixteen-bit mask per block in `selection.json`. `hitLiquids` is the third argument of
  `rayTraceBlocks_do` and exactly one class reads it -- `jp`, BlockFluid, whose whole override is
  `hitLiquids && metadata == 0`. **That is why a bucket fills from a source and not from a stream**,
  and it is the only column in the table that varies inside a block, which is why it is a mask and
  not a bool.
- **Reach 5.0, not 4.0.** `ItemBucket` builds its own ray with its own literal and never asks the
  controller, so a bucket genuinely reaches a block further than a block can be broken.

Filling matches on **material**, as the original does, so either water block fills the same bucket.
Pouring refuses a solid cell -- a branch that can only fire with the eye inside a block, because the
pour goes into the cell the ray came *from* and `rayTrace` never tests the cell it starts in. Without
it, a player with their head in stone pours water into the block they are standing in.

#### Jump climbs a ladder

**Ours, and not in the jar.** a1.1.2 climbs only on `collidedHorizontally && isOnLadder()`, which on
a mouse is free because the hand on W is not the hand that aims. On a 3DS it is the same thumb: the
circle pad steers *and* looks, so turning your head to see where you are going stops the stick
pressing the wall and you slide back down. Jump is otherwise dead on a ladder, it means "up"
everywhere else, and it climbs at `kLadderClimb` -- the same 0.2 a tick the wall-press gives, so
neither route is a shortcut. It is asked **before** `onGround`, so standing at the foot of a ladder
climbs rather than jumps.

#### The tab strip's labels sat on the top edge of their buttons

`kTabTop / kCell + 1` names the strip's *first* row, not its middle: `text` counts rows from one, so
the `+ 1` that converts the zero-based row index is already spent. The band is 216..240, which is
rows 28, 29 and 30, and 29 is the one whose glyphs share a centre with the button.

#### The other seven, classified

**Three are a1.1.2 doing what it does**, and the finding is worth more than a fix would be:

- **Eggs do nothing.** `Item.itemsList[344]` is a plain `di` -- no `onItemUse`, no
  `onItemRightClick`, no subclass. `EntityEgg` is Beta's. There is nothing here to implement.
- **The compass is an icon and nothing else.** Item 345 is a plain `di` too; the whole of a compass
  in a1.1.2 is `aa` (TextureCompassFX), which rewrites the 16 x 16 icon every tick with a needle
  aimed at `spawnX/spawnZ`. It needs an animated-tile path on the **items** sheet, which this build
  has only for terrain (`texture_fx.hpp`'s `FlameAnimation`). Transcribed but not written.
- **A sign has no text**, which the previous step already recorded.

**Four need the entity renderer**, which is `docs/todo-m3.md` step 5's open box and is a design note
with a measured budget before it is any code:

- **The bow** (`jg`) fires an `EntityArrow` -- there is no charge in a1.1.2, one click is one arrow.
- **Boats** (`me`) and **minecarts** (`jo`) place an entity, not a block, which is why neither can
  even be put down: there is nothing to put.
- **Paintings** (`od`) place an `EntityPainting`, and additionally need the art table and a
  wall-fitting search.
- **Signs** need a **tile-entity** slot as well -- `Entities` and `TileEntities` are still opaque
  preserved NBT -- plus the console's software keyboard on placement. The block is render type -1 in
  a1.1.2 and is drawn by `TileEntitySignRenderer`, so "signs do not render" is the same missing
  renderer.

**And one is a feature, not a fix**: the held item in the corner of the top screen. It is
`ItemRenderer.renderItemInFirstPerson`, it is self-contained -- the geometry `item_entity_mesh`
already builds, placed in camera space instead of world space -- and it is the one thing on this list
that would put geometry on the top screen that is not the world. It has not been written.

### 13. The compass was a texture, and the entity renderer was already half-built

§12 left six things queued behind two blockers. This is the pass at those blockers, and the first
finding is that **one of the six was not queued behind anything and the other blocker was smaller
than it had been written down as.**

#### The compass is not an item

"The compass doesn't work" sends you to `core/item/`, and there is nothing there: item 345 is a
plain `di` with no `onItemUse`, no `onItemRightClick` and no subclass. Read the item table and the
honest conclusion is that a1.1.2 has no compass behaviour, which is true and is not the point.

`Minecraft`'s startup registers an `aa` -- **TextureCompassFX** -- against tile 54 of
`gui/items.png`, and `RenderEngine.updateDynamicTextures` overwrites those 256 texels every frame
with a needle aimed at the world's spawn. **The compass is a texture.** There is no behaviour to
port, and a build looking for one finds nothing.

`core/texture/compass_fx.{hpp,cpp}` is `aa.a()` transcribed: the pack's own dial re-blitted each
step, a target angle from the spawn point and the player's yaw, a **damped spring** chasing it
(`velocity += error * 0.1; velocity *= 0.8; angle += velocity`, with the error clamped to one
radian), and then a nine-texel grey crossbar and a twenty-five-texel needle drawn over the face with
the front half red. The two loops are not each other with the arguments swapped -- one uses cosine
for x and *subtracts* the sine term from y, the other does the opposite and adds -- and reading them
as symmetric puts the bar on the wrong diagonal.

**Which tile, asked of the jar.** `ItemDef` carries an `animatedIcon` flag now, set by
`tools/genref.java --items` from the FX class's own `b` (the tile it owns) and `f` (the sheet).
Nothing in the engine names item 345 or tile 54. A useful by-product of doing it that way: the same
sweep proves **a1.1.2 has no clock.** There are six `z` subclasses in this jar -- water, flowing
water, lava, flowing lava, two flames and the compass -- and the compass is the only one with
`f == 1`.

Two stated deviations. It **ticks at 20 Hz rather than once a frame**, so the needle settles in
about half a second instead of about a sixth; the original's damping is frame-rate dependent and
this project has already made that call for fire and for particles. And **anaglyph is skipped**, as
it is in the flames, because the 3DS's stereo is real parallax.

Two consumers, because the tile is read two ways: the bottom screen rasterises icons out of the
pack's decoded pixels and a dropped compass is a sprite the GPU samples. Both are pushed.
`Atlas::updateTile` was generalised to `updateTileOf` so the items sheet gets the same two-run
treatment the block atlas already had -- and the bottom screen gets an **override** rather than a
write into the sheet, because that buffer belongs to the Menu and outlives the world: a compass that
overwrote it would leave its last needle baked into the pack.

#### The entity renderer, which was mostly already there

`docs/todo-m3.md` step 5 said the expensive part was that **nothing in the renderer can draw an
entity** -- a different vertex format, a different draw cadence, a different budget. Half of that was
already disproved by §9: the 16-byte `DetailVertex` takes arbitrary corners with arbitrary UVs and
rides the world shader, which is how a dropped item and then a falling block got drawn.

`docs/entity-render-a1.1.2.md` is the design note that box asked for. Its finding is that the
pipeline question was answered by the dropped item and what was left was a **texture-management**
question wearing its clothes: the detail pass samples one texture per draw, and a1.1.2 draws these
entities out of five more files.

- **`core/texture/entity_skins.{hpp,cpp}`** packs `item/boat.png`, `item/cart.png`,
  `item/sign.png` and `item/arrows.png` into one sheet at fixed offsets, and carries `art/kz.png`
  as a 256 x 256 plane of its own. **Never empty**: generated stand-ins go down first and the pack
  is painted over them, so a pack carrying three of the four files does not get a black boat.
  *(§15 added `char.png` as a fifth page and grew the sheet from 128 x 64 to 256 x 64; the four
  offsets above did not move.)*
- **`core/render/box_model.{hpp,cpp}`** is `ip` -- ModelRenderer -- transcribed, and is the one
  genuinely new piece of geometry. Two facts out of the class file that a reimplementation would
  get wrong: **a `ModelRenderer` in this version holds exactly one box**, because `addBox` assigns
  its arrays rather than appending; and **the texture space is 64 x 32 with no field that says so**,
  because `ll` divides every UV by hard-coded literals. The 0.1-texel inset is the original's too.
- **Winding does not matter**, and that is worth writing down: the opaque detail pass runs with
  `GPU_CULL_NONE` because a crossed square has two sides, so a box whose faces came out inside-out
  is still drawn. A mistake there is a shading question, not an invisible boat.

`tests/box_model_test.cpp` is where this is pinned, because its failure mode is not a crash: a box
whose depth and width are swapped still fits its page and still draws, and no screenshot says which
of the six faces is wrong.

#### Paintings

`od.a(...)` -- ItemPainting.onItemUse -- **runs to completion in this build and always did.** It
builds an entity, asks it whether the wall will hold it, and hands it to the world. With nowhere to
put an entity the click performed every step and produced nothing, silently. That is the shape of
the bug for all six entity-spawning items, and it is why `places` could not express it: `places` is
0 for all six, correctly, and 0 also means "does nothing".

So there is a **`spawns` column** now -- none, painting, boat, minecart, arrow -- read as
`instanceof` against the item classes the way `armour` and `bucket` already are, plus
`spawnVariant` for `ItemMinecart`'s own 0/1/2. It finds exactly six items, including all three
minecarts.

`core/entity/painting.{hpp,cpp}` is `jc`: `setDirection` and `onValidSurface`, both transcribed, and
the constructor's art draw -- **every art in declaration order, the ones that fit kept, one picked
at random**, which is why a one-block gap gives one of the seven small pictures. Three details that
a plausible reimplementation gets wrong:

- **`offs` is not `size % 32 == 0`.** The class file tests `== 32` and `== 64` explicitly, so a
  48-texel painting -- the Skeleton and the Donkey Kong -- gets no half-block slide where a modulo
  reading would give it one.
- **The bounding box is shrunk, not grown**, by `0.00625` on every face. Without that a painting
  flush against a wall collides with the block behind it and refuses its own placement.
- **The canvas is twice as thick as its hitbox**: the box is `0.5/32` of a block either side and the
  geometry is `0.5/16`. That is the original's arithmetic and not a mismatch here.

`core/render/painting_mesh.{hpp,cpp}` is `bw.a(Ljc;IIII)V`. It does **not** go through the box model,
and that is the class file's decision: `RenderPainting` emits its own quads because it subdivides
the canvas into 16 x 16 cells and **lights each one separately**, which a single box cannot do. A
four-block picture across a doorway is bright at one end and dark at the other. The `Painting`
carries sixteen light bytes for that -- sixteen because the largest art in this version is four
blocks by four -- resampled every tick while the wall check stays on its hundred-tick counter.

#### What is left, and what it now needs

The bow, the boat, the minecart and the sign are **no longer blocked on a design decision**; they
are blocked on their own work, and the shared pieces are in.

- **The bow** wants an `EntityArrow` pool and `gk`'s geometry, which is a custom tessellator shape
  rather than a box model. `jg.a(...)` is transcribed in this file already: it consumes an arrow
  through `InventoryPlayer.consumeInventoryItem`, plays `random.bow` at `1/(rand*0.4+0.8)`, and
  spawns one `kg`. **There is no charge in this version** -- one click, one arrow.
- **The boat and the minecart** want `cl` and `hj` -- five and seven boxes -- which the box model
  can now draw, plus their physics, plus **riding**, which is the first thing in this project that
  takes the camera off the player body and is a `PlayerBody` question rather than a renderer one.
  `me.a(...)` does its own ray at reach **5.0 with liquids on** -- the same shape as the bucket --
  and spawns at `(x + 0.5, y + 1.5, z + 0.5)`. `jo.a(...)` places **only on rails** and reads its
  cart type off its own field.
- **Signs** still want the tile-entity half plus the console keyboard, and they are the only one of
  the four that needs something the `entitydata` box does not already cover.

**And the pools are still session pools.** A painting placed and a world reloaded is a painting
gone, exactly as a dropped item is. `docs/entity-render-a1.1.2.md` says so explicitly; the
`entitydata` slot is still `none` and this pass does not close it.

### 14. The other four: an arrow, a boat, a minecart and a sign

§13 removed the two blockers and left four features standing on them. This is those four, and three
findings in it are worth more than the features.

#### `yOffset` is computed in float, and a minecart written with a double simply does not go

The cart's `setSize(0.98F, 0.7F)` gives `yOffset = height / 2.0F`. `Entity.setPosition` builds the
box at `posY - yOffset`, and the rail branch sets `posY` to `j + yOffset` -- so the box's bottom is
`j + yOffset - yOffset`, and whether that is exactly `j` depends on which `yOffset` it is:

```
float:  64.0 + 0.34999999403953552 - 0.34999999403953552 == 64.0
double: 64.0 + 0.34999999999999998 - 0.34999999999999998 == 63.99999999999999
```

A box bottom a fraction of an ulp *below* the block it is standing on overlaps that block, and
`calculateOffset` then refuses the move. **The first minecart travelled 0.01 blocks and stopped**,
with everything else about it correct. This is the same class of thing `physics-a1.1.2.md` warns
about -- "several constants are not the round numbers they look like" -- but it is worse than a
wrong constant, because the two numbers are equal to fifteen digits and differ only in what a
round trip does to them. Both the boat and the cart carry the float form now.

#### `EntityLiving` has no idea it is riding anything

There is no reference to `ridingEntity` anywhere in `ge`. So a player in a boat keeps running
`moveEntityWithHeading` exactly as they would on foot -- the stick still becomes `motionX`/`motionZ`
under the 0.02 air acceleration and the 0.91 friction -- and the *only* thing riding changes is that
the position is overwritten afterwards. A boat's entire steering is
`motionX += riddenByEntity.motionX * 0.2`.

That is why riding cost so little: `PlayerBody::tickRiding` applies the heading, applies the
friction, and puts the body where the vehicle's seat says. `core/entity/rider.hpp` is the seam, and
it exists so `boat.hpp` never includes `player_body.hpp`.

#### A head-on boat does not break

`EntityBoat`'s wreck test is `isCollidedHorizontally && speed > 0.15`, and `speed` is measured
**after** `moveEntity` -- which zeroes the motion on whichever axis was clipped. So a boat driven
square into a wall measures a speed of nothing and survives; what breaks one is hitting at an angle,
where the un-collided axis is still above the threshold. That is why boats break on shores and not
on flat walls, and the first version of the test asserted the opposite.

#### The four features

- **The bow.** `jg.a(...)` has **no charge in this version** -- drawing a bow arrives with Beta 1.8,
  the same release Creative does -- so one click is one arrow at a fixed 1.5 velocity.
  `core/entity/arrow.{hpp,cpp}` is `kg`, and it is the cheapest moving entity here because it does
  not use `moveEntity` at all: it ray-traces from where it is to where it would be, sticks in
  whatever it hits, and otherwise adds its motion. **Gravity is 0.03, not the 0.05 of later
  Minecraft** -- an arrow ported with 0.05 drops short and reads as bad aim.
  `core/render/arrow_mesh.{hpp,cpp}` is `gk`: a tail cap drawn twice and four fins, on the one
  square page of the entity sheet.

  One stated deviation: **it costs no arrow.** The original consumes one and refuses to fire
  without; this build has no stack depletion anywhere, and requiring ammunition would be the one
  place a Creative hand was a stock rather than a catalogue.

- **The boat.** `me.a(...)` is an `onItemRightClick` that casts **its own ray at reach 5.0 with
  liquids on** -- the bucket's shape exactly -- which is why a boat could never have gone down the
  block-placement path: the crosshair's ray is not allowed to see water, and water is the only place
  a boat is any use. Buoyancy is five horizontal slices of the hull asked whether they are wet, the
  fraction doubled and less one scaling a 0.04 push; there is no water level in it anywhere.

- **The minecart.** `jo.a(...)` places **only on rails**, so half of "not even placeable" is the
  game. The physics is the largest single entity method in a1.1.2 and its shape is worth stating:
  the cart is **snapped onto the rail's centreline every tick** (assigned, not steered, so it cannot
  leave sideways), its speed is **re-pointed rather than re-computed** (which is why it takes a
  corner at full speed), a slope pushes with a constant `0.0078125` rather than with any component
  of gravity, and its height is a **lookup**, not a simulation. The connection matrix `oc.j` turned
  out to be exactly the ten rail shapes `core/tick/rail.hpp` already derives.

  Two things are not ported and are named rather than hidden: a chest or furnace cart is placeable
  and drawable but opens nothing, and the cart's *contents* are not drawn inside it.

- **Signs.** Three halves, and the first is why the other two were invisible: **a sign is render
  type -1**, so the mesher answers with nothing and the whole of a sign -- board, post and text --
  comes from a tile entity that did not exist. `core/world/sign_store.hpp` is `ob`,
  `core/render/sign_mesh.{hpp,cpp}` is `jk` through `in`, and `Overlay::editSignViaKeyboard` is
  `GuiEditSign` as a console applet.

  Two deviations. The editor is **one multi-line keyboard rather than four**, because opening the
  system applet four times to write one sign would be worse than the thing it replaces. And **there
  is no generated font**, so a sign on Dev Art shows a blank board -- the one place in this project
  where Dev Art is less than a real pack, and the obvious next thing if it matters.

#### A shared-sheet consequence worth knowing

The minecart's underside plate is 18 x 14 x 1 at texture offset (44, 10), and the cuboid unwrap
wants 38 texels of width from u 44 -- eighteen past the edge of a 64-wide page. The original does
not care: `cart.png` is a texture of its own, OpenGL wraps, and the faces involved are inside the
cart and never seen. **Here the pages share a sheet**, so unclamped those UVs would sample the
arrow's page beside them. `buildBox` clamps into the page now; the difference from the original is
confined to faces the original also draws with off-page UVs.

### 15. The held item, and three things about a screen that is not a 4:3 window

`docs/todo-m3.md`'s last open box before mobs: **the item in the player's hand, in the bottom right
of the top screen.** It is `jh` -- `ItemRenderer` -- and the estimate that it was self-contained held
up: `core/render/held_item.{hpp,cpp}` is the whole of it in core, `Renderer::drawHeldItem` is one
extra pass, and no existing geometry changed. What was *not* in the estimate is that three of its
decisions are about this console rather than about a1.1.2, and each of them has a number behind it.

#### The class file, in two halves

`jh.a(F)V` -- `renderItemInFirstPerson` -- is a transform and two branches. The transform is nine
`glTranslatef`/`glRotatef`/`glScalef` calls composed in camera space, and the second branch of it is
the arm swing read **twice with different curves**: `sin(swing * PI)` for the dip and
`sin(swing * swing * PI)` for the turn. The item branch ends with `glScalef(0.4)` and hands over to
`jh.a(Lev;)V` -- `renderItem` -- which splits again:

- **A block** whose render type `RenderBlocks.renderItemIn3d` accepts -- 0, 13, 10 and 11, checked
  in the class file rather than remembered -- goes through `renderBlockOnInventory`, which is
  `glTranslatef(-0.5, -0.5, -0.5)` and the six faces at the block's own bounds. `block::renderBoxes`
  already answers with boxes for exactly those four types, so a fence in the hand is a fence post,
  the same shape it is in a slot and on the ground.
- **Everything else** is the flat icon **given thickness**: a front face, a back face a sixteenth of
  a unit behind it, and four runs of sixteen one-texel strips joining their edges. 66 quads, and the
  strips are what make a held sword read as a sword rather than as a decal. Each strip's texture
  coordinate is the tile edge minus `0.001953125F`, which is half a texel of a 256-wide sheet -- so
  they land on texel centres by themselves and need none of this project's own tile inset.

The per-tick half is `jh.a()V` plus `EntityPlayer.b_`/`w`: `equippedProgress` chases 1 while the
selected item is the drawn one and 0 while it is not, at 0.4 a tick, and adopts the new item once it
is under 0.1 -- which is what makes switching hotbar slots *lower one item and raise the next*. The
swing is eight ticks of `swingProgressInt / 8`, and `getSwingProgress` adds one when the difference
is negative so the last eighth interpolates 7/8 -> 1 instead of snapping backwards.

Two calls in `Minecraft.clickMouse` decide when the arm swings, and they are not symmetric: button 0
swings **before it looks at what is under the crosshair**, button 1 swings only if
`onPlayerRightClick` returned true, and the `Item.onItemRightClick` path swings **not at all** --
it calls `resetEquippedProgress` instead, so a bucket becoming a water bucket is shown as a re-raise.
`editBlocks` in `platform/ctr/main.cpp` reproduces all three.

#### The empty hand is the arm, and the skin is a pack file

The `else` branch draws the player's own right arm, and it was nearly written off as unportable on
the grounds that this project may not ship `char.png`. **That was the wrong reading of the
constraint.** `char.png` is a *texture pack's* file exactly as `terrain.png` is -- it sits at the
root of the jar rather than under `item/`, which is the only thing unusual about it -- so the rule
is the one every other page already follows: read it from the pack, ship nothing.

So it is ported. `core/texture/entity_skins.{hpp,cpp}` gained a fifth page and the sheet grew from
**128 x 64 to 256 x 64**: a fifth 64 x 32 page does not fit the old sheet, 96 is not a power of two
and a PICA texture dimension has to be one, and 256 x 64 is the next size that is one in both axes.
64 KB rather than 32, eight slots of which five are used, and **the four existing pages kept their
offsets**, so no model's UVs moved.

**A pack with no skin gets a black arm.** Every other page's stand-in is a coloured grid, which is
right for them -- a boat with a stand-in still reads as a boat. An arm does not work that way: a
forearm in Dev Art orange reads as a bug, where a solid silhouette is honest about being a shape
with no skin on it. It is also the one page whose texels are entirely covered by its model, so a
grid would tell a reader nothing the silhouette does not.

The geometry is one box, and the only thing in it worth deriving was what
`setRotationAngles(0, 0, 0, 0, 0, 0.0625F)` leaves the arm at. **Two of the three angles are zero
and the third is not**: the method's last four statements add an idle sway to both arms, and the
right arm's Z term is `cos(age * 0.09) * 0.05 + 0.05`, which at age zero is **0.1** rather than 0.
The swing block above it does run -- `onGround` is set to 0 and the gate is `> -9990` -- but every
term in it is a sine or a square of zero, so it puts the rotation point back exactly where the
constructor had it. The box itself is `ip(40, 16)` with `addBox(-3, -2, -2, 4, 12, 4, 0)` and
`setRotationPoint(-5, 2, 0)`, and it goes through the same `buildBox` a boat does -- page clamp,
0.1-texel inset and all.

**The two branches share their shape and none of their numbers.** The swing offset is 0.3/0.4/0.4
for the arm against 0.4/0.2/0.2 for an item; the rest position is 0.8 and -0.75 against 0.7 and
-0.65; and the arm turns **+70 degrees about Y** where the item turns -20, with no X rotation at
all. The five transforms between the swing and the model are in **blocks and not model units** --
`render(0.0625F)` scales only what is inside it -- so `glTranslatef(-1, 3.6, 3.5)` looks like it
puts the arm behind the player and the three rotations after it bring the box back to a centroid of
(0.78, -0.64, -0.81), which is the bottom right corner with its lower end off the screen.

**Spectator gets no hand at all**, which is now a distinct state from an empty one: `setHeldItem(0,
...)` is an empty hand and draws the arm, and `clearHeldItem()` is a camera with no body.

#### Three numbers that are the screen's and not the game's

**The framing.** a1.1.2's constants place the hand at x = 0.56 in camera space and the horizontal
half-extent is `tan(fov/2) * aspect * d`, so a 5:3 screen divides by 25 % more than the 4:3 window
those constants were chosen against: untouched, the item sits at 0.67 of the half-width instead of
0.83 and reads as floating near the middle rather than sitting in the corner. `buildHeldItem` takes
the aspect and **shifts** the item sideways by `0.56 * (aspect / (4/3) - 1)` -- 0.14 of a block.
A shift and not a scale, because scaling camera-space x would stretch the item as well as move it.

**The near plane.** `kNearPlane` was 0.2 here against the original's 0.05, traded for depth precision
a 16-bit buffer needed. Measured against the built geometry, the nearest corner of a held sprite
reaches **0.193 of a block** in front of the eye -- inside 0.2, so the world's projection would have
clipped the tip off it, and `drawHeldItem` built its own projection at a1.1.2's own 0.05. It still
does, and the two constants are now equal: the world's came down to 0.05 as well once the 24-bit
depth buffer made the trade pointless. See *The near plane was three bugs* below.

**The depth clear.** `iq.c(F)V` calls `glClear(GL_DEPTH_BUFFER_BIT)` at offset 704, immediately
before `renderHand`, so the hand can never be clipped by a wall the player is standing against.
citro3d has no mid-frame depth clear -- `C3D_RenderTargetClear` picks what `C3D_FrameDrawOn` clears
and nothing more -- and a full-screen quad to do it by hand is 96,000 fragments of overdraw on a
fill-bound device. So the hand is put where the world cannot reach instead: `C3D_DepthMap` compresses
this pass into the **top 5 %** of the reversed depth range. Window depth is `near/d` to a far plane's
worth of rounding, so a world fragment reaches 0.95 only within `kNearPlane / 0.95` of the eye and
the near plane clips all but the last 5 % of that -- a shell no block face can occupy without filling
the screen. It was one centimetre deep at `kNearPlane` 0.2 and is five millimetres at 0.05: the
argument gets stronger as the near plane comes down, not weaker. 5 % and not 1 % because the item still sorts against *itself* and an extruded
icon is a sixteenth of a unit thick: a twentieth of a 16-bit buffer leaves about a dozen levels
across that thickness, a hundredth leaves two.

#### And a stereo number, which is the one that wants hardware

**The hand is nearer than the eyes are far apart.** The separation this renderer uses is derived
rather than picked -- 7 px of infinity disparity at a focal distance of 8 comes out as I/2 = 0.33 of
a block -- and the held item sits between 0.19 and 1.37 blocks away, where the `1/d` term of

```
disparity_px(d) = 200 * (I/2) * [ 1/(F*t*a) - 1/d ]
```

reaches 5.2. Put those numbers in and the nearest corner lands **86 pixels** out of the screen. The
3DS convention is about 13 px and titles run to 20. No focal distance fixes it, because the object is
closer to the eye than the two eyes are to each other.

So the pass gets its own two: the focal distance is the item's own depth, 0.72, which puts it *on*
the screen plane rather than in front of it, and the separation is a **sixteenth** of the world's,
which holds the whole item inside the same 7 px the world's infinity is allowed
(`200 * (I/2) * 1.67 = 7` solves to I/2 = 0.021 against 0.327). The slider still scales it, so
turning 3D down still flattens the hand. **That sixteenth is arithmetic and not a measurement** --
what it feels like is the kind of thing only a console can say.

#### The hand was invisible, and the crosshair is why

**It drew nothing on hardware.** Not clipped, not behind the world, not mis-transformed -- drawn,
correctly, at an alpha of zero.

`drawSelection` and `drawCrosshair` each say they restore no state "on purpose", and the argument is
sound: `applyWorldState` runs before every eye and states all of it rather than inheriting it, so a
pass that is *last* may leave the GPU however it likes. `drawHeldItem` was put after them, which
made that argument false. What they leave behind is a **one-stage combiner replacing both colour and
alpha with the vertex's own**, the alpha test off, and src-alpha blending -- and the detail shader
puts the **fog amount** in the vertex alpha, which for something 0.8 of a block from the eye is
zero. Every fragment of the hand was computed and then multiplied by nothing.

The fix is one call to `applyWorldState()` at the top of the pass rather than undoing the
crosshair's three settings by hand: a fourth thing either of those passes changes later would
otherwise be the same bug again. **The lesson is about the comment, not the code** -- "restores
nothing on purpose" was true of the last pass in the eye and was read as true of the renderer.

#### The skin is a pack file, and there is a screen for it

The first cut read `char.png` out of the *active* pack and stopped there. What was asked for, and is
now there, is **Options → Skin**: Default, every texture pack carrying a `char.png`, and every
`.png` in `sdmc:/3dalpha/skins` -- the folder created the first time the screen is opened, because a
console help line naming a path that does not exist is a worse instruction than one that does.

Four decisions in it are worth keeping:

- **The saved value is a name, not a path or an index.** `pack:<name>` or `file:<name.png>`, which
  is the rule `texture_pack` already follows, and it is what lets `ensureAtlas` apply the choice at
  boot by reading the one file it names. An index would have meant listing the packs folder --
  a full read of every zip on the card -- for a setting.
- **A row is decoded before it is offered.** A PNG that will not decode, is not a multiple of 64
  across, or is neither of the two skin heights is left off the list rather than offered and then
  failing under the player's thumb. That is `listPacks`' own rule for a zip that will not open.
- **Picking a skin rebuilds the atlas and then overrides the page.** Patching alone cannot go
  *back* to Default: Default is the active pack's own file and only `buildEntitySkins` reads it, so
  there would be nothing to restore it from.
- **`PackEntry::hasSkin` comes off the central directory**, beside the file count, so a pack with no
  skin costs the skin screen nothing at all and one with a skin is opened a second time only to read
  it.

#### The narrow body belongs to 1.8, and the build says so

`detectSkinModel` recognises a slim skin -- the test the format implies, since a narrow arm's unwrap
is `2 * (3 + 4) = 14` texels where a wide one is 16, so the last two columns of the right arm's page
are texels no narrow skin ever fills -- and **only a 64 x 64 can be one**, because that format and
the narrow body arrived together. The row says "slim" so a player whose Alex skin looks a texel wide
is told why on the screen that offered it.

It is then **drawn on the wide arm anyway**, because a1.1.2 has no other. `versions/<id>.json`
gained `hasSlimSkins`, false here, and `core/render/held_item.cpp` carries a `static_assert` on it
rather than a branch:

> the narrow arm's ModelBiped box has not been derived -- open the jar of the version that added it
> before turning hasSlimSkins on

**That is the point of it.** Flipping the flag has to fail the build until somebody derives that
version's box the way every other number in the file was derived from a1.1.2's; shipping a
remembered `addBox(-2, -2, -2, 3, 12, 4)` would be exactly the kind of thing this project's rule
about the jar exists to stop.

#### Coverage

`tests/held_item_test.cpp`, 14 cases; `tests/skin_list_test.cpp`, 10; plus three added to
`tests/entity_skins_test.cpp` for the new page and two to `tests/settings_file_test.cpp` for the new
key. The animation is checked against the class file's own numbers; the geometry has no oracle, so
what is checked is the property the feature exists for -- the centroid is right of the eye, below it
and in front of it, in **all three** branches and mid-swing -- plus the structural claims: one
texture per hand, whole boxes or a full 66-quad sprite or one arm box, every arm UV inside the
player's page and no other's, nothing written past the end of the buffer, and an item this build
does not know drawing neither itself nor a bare arm. The skin list is checked on real files in a
temporary directory -- Default always first, a pack without a skin never offered, a broken or
wrongly-shaped PNG never offered, the key rebuilding a path with nothing listed, a 64 x 64 keeping
its top half rather than being squashed, and a chosen skin changing the player's page and **no
other page on the shared sheet**. Host suite 1096/1096; the 3DSX build passed.

**Still unrun on hardware**, and the invisible-hand bug above is the reason that matters: it was a
GPU state interaction, and nothing on the host could have caught it.

### 16. Nothing could be hit, and one hook was never called

A play session reported nine things that would not drop or would not break. Checked one at a time
against the client jar, they turned out to be **two faults and five things that were already
right** -- and the two are worth separating, because they are at opposite ends of the engine.

**Fault one: there was no way to attack anything.** The break button rays for a block and only for a
block. `Minecraft.clickMouse` asks `objectMouseOver` for what is under the crosshair and **an entity
there is hit instead of the block behind it**, through
`PlayerController.attackEntity` -> `EntityPlayer.attackTargetEntityWithCurrentItem` -> the entity's
own `attackEntityFrom`. With that branch missing, a boat, a minecart and a painting could be placed,
ridden and looked at, and nothing a player did could damage one -- so *of course* none of them ever
left anything on the ground. The report read as three missing drop tables and was one missing call.

`item::attackEntity` (core/item/use.hpp) is that call. It picks over the painting, boat and cart
pools at `kBlockReach` and returns whether it hit something, which is the caller's cue not to break
the block behind it. What each of the three then does is transcribed:

- `dc.a(Lkh;I)Z` and `oc.a(Lkh;I)Z` are **the same four lines with a different tail**:
  `forwardDirection` flips, `timeSinceHit = 10`, `damage += amount * 10`, and past 40 the vehicle
  breaks. Those three numbers now live in `core/entity/rider.hpp`, which was already the seam
  between a vehicle and its rider and is the only header both include. **Five bare-handed hits**
  break either one -- `InventoryPlayer.getDamageVsEntity` answers 1 for an empty hand and `onUpdate`
  sheds a point of damage a tick in between.
- The boat leaves **three planks and two sticks**; the cart leaves **item 328 plus the chest or the
  furnace it was carrying** -- not items 342 and 343, which is the one place a cart's type reaches
  the ground.
- `jc.a(Lkh;I)Z` -- the painting -- **never reads its damage argument**. One hit takes it down.

Two things fell out of writing it. `MinecartSystem::hitByArrow` was already the same method with
`amount` fixed at 4 and no drop, so it is now `attack(world, index, 4)` and an arrow-broken cart
leaves a cart. And the boat's `Wreck` list -- a wall-broken boat recorded its position for
`main.cpp` to drop planks at -- **turned out to be unnecessary**: `TickWorld::spawnItem` is `const`,
so the pools can drop through the world's own sink exactly as `dropBlockAsItem` does. The wall and
the hand now go through one `dropAndRemove` per pool, and fifteen lines came out of `runGame`.

**A painting whose wall is mined also drops now**, which is not the attack path at all: `jc.e_()`'s
hundred-tick `onValidSurface` check spawns an `EntityItem` as it removes the picture. The removal
was here and the spawn was not, and the header said the item "belongs with Survival" -- it does not.
It is the entity's own method and a1.1.2 has no Survival to gate it on.

**Fault two: `onBlockDestroyedByPlayer` was never called.** `hq.b(IIII)Z` is four steps -- particles,
read the metadata, `setBlockWithNotify(0)`, then sound **and `ly.b(Lcn;IIII)V`** -- and
`item::destroyBlock` had the first three. use.hpp had quoted all four since it was written.

It is empty on `Block` and has exactly three overrides in the whole version, which is why the gap
was invisible: `km` (stairs) forwards to the model block, whose own is the empty one; `q` (TNT)
primes it, and there is no primed-TNT entity here -- the same deviation `core/tick/fire.cpp` already
names at the other call site. The one that shows is `hd` (crops): **three rolls of
`nextInt(15) <= metadata`, each worth a seed**. A crop's `idDropped` answers only at growth stage 7,
so before this a crop broken at any other stage left *nothing at all* -- which is exactly what was
reported. `tick::blockDestroyedByPlayer` (core/tick/drop.hpp) is the hook, dispatched on the
behaviour like everything else in that file. Its draws come out of the world's random, so a world
with no item pool still walks the same stream.

Two details of it are the class file's and neither is `dropBlockAsItem`'s: the offsets are computed
**entirely in float**, coordinate included, where `dropBlockAsItem` widens each term before adding;
and the seeds are **the player's break only**. Knocking the farmland out from under a crop is the
neighbour path -- `mq.h`, which is `dropBlockAsItem` alone -- and leaves wheat at stage 7 and nothing
below it. Conflating the two would make a crop worth farming by mining the dirt under it.

**The other five were already right, and that is a result too.** A door, an iron door, a redstone
torch, a pressure plate and a sign whose support is taken away all break and all drop, in
`core/tick/redstone.cpp` and `core/tick/behaviour.cpp`, through the real player-break entry point
(`item::destroyBlock`) and not just through a synthetic write. Measured, not assumed: a scratch
fixture broke the supporting block under each in turn and read the drop sink -- door 324, iron door
330, redstone torch 76, plates 70 and 72, sign post and wall sign 323, both halves of a door gone,
the sign's cell emptied. Whatever was seen on the console was not these behaviours; the two faults
above are what the session found, and the difference is worth the note so the next reader does not
go looking for a third.

`AABB`'s ray test moved with this work: `rayHitsBox` was **duplicated verbatim** in `boat.cpp` and
`minecart.cpp` and a third copy was about to appear in `painting.cpp`, so it is in
`core/util/aabb.hpp` now.

Coverage: `tests/drop_catcher.hpp` is a shared sink so a test can ask what something left behind
without building an item pool, and `tests/drop_test.cpp` was moved onto it. Eleven cases added --
four on the crops hook (including that the neighbour path deliberately leaves no seeds, and that the
random stream is unchanged with no sink), three on the painting, four on the boat and three on the
cart, plus two on the `item::attackEntity` seam itself. Host suite **1112/1112**; the 3DSX build
passed. **No hardware run.**

### 17. Arrows hit every collidable entity, and a full item pool stops eating drops

Reported as "arrows don't hit boats, and minecarts drop nothing when killed by arrows". Two faults.

**The arrow only swept minecarts.** `kg.e_()` gathers every entity near its path and keeps those
whose `canBeCollidedWith` (`c_()`) is true. From the class files, that is the painting (`jc`,
`return true`), the boat (`dc`) and the cart (`oc`, both `!isDead`). `dx` (EntityItem) and the base
`kh` say false. The nearest intercept against `box.expand(0.3F)` wins (`d < best || best == 0`),
clipped to the block hit, and takes `attackEntityFrom(shooter, 4)`. `ArrowSystem::tick` now takes
an `ArrowTargets` of all three pools and runs one nearest-target sweep over them. A painting falls
to one arrow (its attack ignores the amount). A boat, like a cart, breaks to a second arrow inside
two seconds. The player is also a target once the arrow is 5 ticks old; with no health yet, that is
left out.

**The cart did drop, into a pool that refused it.** A host repro of the reported chain passed:
`runGame`'s tick order, a standing eye 6 blocks off, a real `ArrowSystem` and `MinecartSystem`, and
the drop sink. The second arrow breaks the cart and item 328 reaches the sink. Past the sink,
`ItemEntitySystem::spawn` can only fail by returning null from `allocate()`, which happens when all
64 slots are full. That ceiling was sized for player throws only, and its header said to revisit it
"when block drops land". Block drops have landed. In Creative every broken block drops an item that
lives 5 minutes, so the pool fills quickly and the newest drop, the cart's, was refused. This is
**inferred, not seen on hardware**; it is the only path in the code that fits the report.

Now a drop into a full pool **replaces the oldest item**, the one nearest its own despawn, and the
`evicted()` counter records it. `dropFromPlayer` still refuses, because a refused throw stays in the
hand and loses nothing. Capacity stays at 64.

Coverage: `tests/arrow_test.cpp` has four new cases. Two carts are broken by arrows from a standing
player and item 328 is caught. A boat breaks to planks and sticks. A painting is knocked down. A
boat in front of a cart takes the arrow alone. A mutation check (boat and painting strikes turned
off) fails three of them. `tests/item_entity_test.cpp` replaces the refuse-the-newest case with
eviction of the oldest and with a throw still being refused. Host suite **1117/1117**; the 3DSX build
passed. **No hardware run.**

### 18. The entity pools have no cap; the heap decides, and the draw is nearest-first

Reported as "arrows, minecarts (and probably more) have a limit, the real game doesn't". The report
is right. a1.1.2 caps none of them: `spawnEntityInWorld` adds to a list, and so does
`EffectRenderer.addEffect`. Its bytecode (`bq.a(Lnq;)V`) is `List.add` then `return`, read off the
jar for this change. The old arrays (arrows 128, carts 32, boats 8, paintings 32, items 64, falling
blocks 16, particles 512, signs 64) were limits this port had chosen.

**Storage: `core/util/segmented_pool.hpp`.** Each pool is a list of fixed-size segments.
- **The first segment is the old capacity and is taken at construction** (`kInitialCapacity` on each
  system). Ordinary play therefore allocates exactly as often as before, which is never.
- **Growth copies nothing and moves nothing.** A reference into a pool survives a push into the same
  pool. `FallingBlockSystem::tick` relies on this, because it can spawn while it walks. A
  `std::vector` would break that silently on its first reallocation.
- **Growth goes through `malloc`, never `operator new`,** which on the console ends the process
  (`platform/ctr/heap.hpp`).
- **Growth first asks `poolGrowthAllowed`** (`core/util/memory.hpp`). It is allowed only while the
  heap left after it still holds an 8 MB reserve plus three copies of every pool byte. Those three
  copies are a save's snapshot, its NBT and the compressed file, so the save after a spike can still
  be written.
- **Refusal lands on the old full-array path.** The shot is not fired, the placement fails (the item
  is kept), a drop evicts the oldest item, a throw is refused, and sand takes the instant path.
- **Memory comes back.** `trim` runs at the end of each tick and frees trailing segments, keeping one
  spare as hysteresis. Removal never frees, so no segment goes under a live reference mid-walk.

**Save: `SavedPool<T>`** is a `malloc`'d array sized at capture.
- A capture the heap refuses keeps the previous snapshot. An older save is a save; a partial one is
  a loss.
- `readPool` no longer rejects a list longer than a pool. That old check failed the whole level.dat.
- The count in the file is not trusted for an allocation. Each element is read before it is stored.
- On restore, a pool that the heap refuses keeps what fits and counts the rest in `refused()`.

**Drawing: `core/render/draw_budget.hpp`.** The vertex buffers stay fixed-size, because the GPU reads
them after submit. Previously a pass filled its buffer in pool order, which dropped whichever entities
sat late in the pool, and swap-removal reshuffled that order from frame to frame. Now a pass that
would overflow works as follows:
- It sorts entities into half-block rings and draws every ring that fits in full.
- The first ring that doesn't fit gets exactly the room left, in pool order, so a pile bigger than
  the budget still shows part of itself.
- Items share one `DrawCutoff` across the two sheet passes. Signs share one across boards and text,
  and the text pass rewinds it so it draws the same signs.
- There is no allocation and no sort: 128 counters on the stack, plus a first pass that is skipped
  when everything fits.
- Draw budgets were raised for boats (8 → 32) and falling blocks (16 → 64). The rest are unchanged.
  That is about 64 KB more linear memory.

**Performance at large counts is uncapped, not fixed.** These costs grow with the count:
- The arrow sweep is arrows × targets.
- Cart-on-cart pushing is O(n²) per tick.
- ~~The item merge scan is O(n²)~~ -- **gone in step 35**, with the merge itself. The item tick is
  linear again, and the pool ceiling is the only thing bounding a pile of drops: nothing shrinks one
  now, which is a1.1.2's behaviour and the reason the ceiling matters.

`TickTimer` still clamps at ten ticks a frame, so a slow tick costs frame rate, not a death spiral.
That is the trade asked for ("degrade performance rather than crash"). Nothing about it is measured on
hardware.

**Not touched, and the same kind of limit:** `TickScheduler` (4096, drops and counts),
`LightUpdateQueue` (2048), and `TickWorld`'s torch-toggle (64) and deferred-notify (512) tables.
Those are block-update structures rather than entities.

Coverage: `tests/segmented_pool_test.cpp` has 10 cases. They cover growth without moving, fresh
slots, swap-remove, trim hysteresis, byte accounting, heap refusal, the growth rule at its exact edge,
and three draw-cutoff cases. Every per-system "refuses rather than overflowing" test became two tests:
one showing no limit past the first segment, and one where a `test::LowHeap` (`tests/low_heap.hpp`)
takes the old full path. The persistence tests add a 389-arrow round trip and a low-heap restore.
`past_the_buffer_the_nearest_items_are_drawn` spawns far items first, so a pool-order build would fail
it. Host suite **1134/1134** under ASan/UBSan. TSan is clean on `entity_persistence`,
`housekeeping`, `save_progress` and `pool`. The 3DSX build passed. **No hardware run.**

#### The refusal is said on the top screen

A refused spawn used to look like a button that did nothing. It now puts Legacy Console Edition's
sentence in the bottom left of the top screen: "The maximum number of Minecarts in a world has been
reached." Boats and Paintings use the same wording (LCE's own). Signs, Arrows and Dropped Items were
made to match.

**The look is a1.1.2's own chat overlay**, which LCE kept. It was read out of `lu` (`GuiIngame`):
- `lu.a(String)` wraps at 320 GUI pixels by `kd.a(String)`, the longest prefix that fits, one
  character at a time. Pieces go in at index 0, and the list is trimmed to 50.
- `lu.a()` ages each line by one a tick.
- The draw shows the first ten lines and skips any aged 200 or more. Opacity is
  `t = clamp((1 - age/200) * 10, 0, 1)`, and `alpha = 255 * t * t`.
- Line `i` sits at `height - 48 - 9i`, over `drawRect(2, y-1, 322, y+8)` in black at `alpha/2`.
  The text is `kd.a(String,III)`, which is drawStringWithShadow: the shadow at (+1, +1) in
  `(c & 0xFCFCFC) >> 2`, then the text.

Where the pieces live:
- `core/gui/chat_log.{hpp,cpp}` is the list.
- `core/render/chat_mesh.{hpp,cpp}` builds the glyph quads, in screen pixels.
- `Renderer::drawChat` draws last in each eye:
  - an ortho matrix at the screen plane;
  - strips through the outline program, with the alpha in its tint;
  - text through the detail program off the pack font, with the alpha in the combiner constant,
    because that shader spends vertex alpha on fog.
- `buildChat` runs once a frame, before the first eye.

Two stated differences:
- Only the ten showable lines are kept, since there is no chat screen.
- Posting the message that is already newest, while it still shows, restarts its fade. The
  placement buttons repeat every five ticks, and ten copies of one sentence would fill the list.

Detection needs no signature change. `item::markRefusals` snapshots every pool's `refused()` before
a click, and `refusedSince` names whichever rose. A throw compares the item pool's count the same way.

**No pack font, no message:** the only in-game font is the pack's `default.png`, which is the same
limit sign text has.

**A bug fixed on the way.** `useSign` wrote the sign block before asking the store for the tile
entity. A refused store left an invisible block, since a sign is render type -1. The tile entity is
now asked first, and a refusal places nothing.

Coverage: `tests/chat_log_test.cpp` has 14 cases:
- the class file's numbers (opacity is 254 at age 180 in doubles, as Java computes it);
- wrap order, the ring, ageing, and repeat restarts;
- code-point-safe truncation;
- shadow/text quads, positions and UVs;
- no font, and the buffer edge;
- the sentences, and refusal detection.

`a_sign_the_store_will_not_hold_leaves_no_block_behind` is in `sign_test.cpp`.

#### What an entity costs in memory, and what a mob farm will

Asked while doing the above: can many entities be made cheaper in RAM by not holding the same
things repeatedly? Measured with `sizeof` on the host and through devkitARM with the console's
flags; the two agree:

| | Bytes |
|---|---|
| Arrow | 176 |
| Minecart | 176 |
| Boat | 152 |
| ItemEntity | 152 |
| Particle | 144 |
| FallingBlock | 136 |
| Painting | 120 |
| SignText | 80 |

**Nothing per-type is held per instance already.** Textures are one sheet each, models are
compiled-in box tables, and item and block definitions are generated tables. An instance holds only
its own state, and most of that is a1.1.2's doubles (position, previous position, motion and box).
Narrowing those would break the float behaviour the physics was transcribed for. A thousand
entities is about 150 KB, against a newlib heap of about 21 MB on an old 3DS and 40 MB on a New 3DS
(`docs/3ds-performance.md`).

**Mobs are bounded by the game, not by RAM.** `ia` (PlayerControllerSP) builds two spawners:
- `new k(this, 200, co.class, ...)`: 200 monsters (`co` is IMob);
- `new az(15, ag.class, ...)`: 15 animals.

Each checks `cn.b(Class)`, the world's count, before spawning. That is a rule of the game rather than
a technical limit, so it stays when mobs are ported. A farm's population is at most about 215 mobs,
well under a megabyte even at 1 KB each.

**What will actually cost is per-frame model building.** Every entity pass rebuilds its vertices on
the CPU every frame. At 200 bipeds of six boxes that is 28,800 vertices (460 KB written) per frame.
The saving worth making for mobs:
- build each model type's boxes **once**, into a static buffer;
- per mob, upload only its part matrices and light;
- draw each mob with a skinning-style vertex shader that picks the matrix by a part index.

That turns "rebuild the model" into "set some uniforms". It belongs in the mob work
(`docs/todo-m3.md`) rather than here, because no pass that exists today draws enough animated
entities to need it. Pathfinding has the same shape of cost and wants a per-tick budget.

### 19. A minecart stack overflowed to NaN, and took its rider and the save with it

Reported as "stacking too many minecarts turns them flat or makes them disappear; riding one
teleports you to 0 0 0 into a ghost world that doesn't render or collide and has no map arrow; a
world saved after that won't open". One fault and one bad failure mode.

**The fault is a1.1.2's own arithmetic.** `oc.f(kh)` (applyEntityCollision), cart against cart,
from the class file: `sum = m1 + m2`, then `m1 = m1 * 0.2 + sum / 2 - push` and the mirror for the
other cart. The pair ends with **1.2 times** the motion it had, and `oc` bounds only the *move*
(`kMinecartSpeedCap`, 0.4), never motion. `oc.e_()` calls it for every nearby cart, so carts that
stay in contact compound it every tick. Host repro: eight carts placed on one rail of a 21×21
loop, one ridden and pushed. The largest per-axis motion is 3.5e125 at tick 250. Past about 1e154
the squared speed overflows, and at **tick 320** (16 s) motion goes `inf` then NaN. The NaN spreads
through the rail snap into position: the cart is drawn nowhere or degenerate, the rider's seat
is NaN, and so is the player.

**The fix bounds motion where it compounds.** `collideCarts` clamps each axis to
`kMinecartMotionLimit` (10). It is the port's bound, not the game's: past 0.4 more motion does not
move a cart faster, it only sets how long the cart keeps top speed once it is clear. From 10, a
ridden cart (drag 0.997, move ×0.75) coasts at the cap for about 976 ticks; an empty one about 79.
The unbounded original, if the double could hold it, would coast for longer and not otherwise
differ. Rail re-pointing can carry a clamped pair's combined speed onto one axis, so per-axis
motion peaks slightly over 10 (11.5 in the repro) until the next collision.

**The save failure was the decoder, not the writer.** Each entity reader rejected a non-finite
field by returning false, which failed `readPool`, then `readPersistentEntities`, then the whole
`decodeLevelDat`. Now `read` fails only when the stream is broken, and an entity holding an
impossible value (non-finite, or out of the existing range checks) is **dropped**. `decodeData`
also repairs a present player whose `Pos` is non-finite: they go to the spawn block (`Pos[1]` is
eye height, so `spawnY + kEyeHeight`) with fall distance reset. NaN motion and rotation go to zero.
The inventory and everything else are kept. Worlds already saved in that state should now open,
without the NaN carts and with the player at spawn.

**Not changed:** the port still resolves each cart pair once per tick where `oc.e_()` resolves it
from both carts (see `collideCarts`). The push is 0.1 against the class file's
`0.1 × (1 − aO) × 0.5`, applied from both sides. That is an existing, deliberate difference and is
unrelated to the overflow.

Coverage: `a_stack_of_carts_stays_finite_however_long_it_runs` (minecart_test) fails with the clamp
disabled, at tick 320. `entity_persistence_rejects_truncation_and_drops_nonfinite_entities` replaces
the case that pinned the old "a NaN entity fails the file" behaviour.
`entity_persistence_puts_a_nonfinite_player_back_at_spawn` is new. Host suite **1151/1151**
(ASan/UBSan); the 3DSX build passed. **No hardware run**, and no real broken save was reopened.

### 20. "Placed blocks don't face the way I'm looking": furnaces and stairs face their neighbours

Reported as "the rotation of placed blocks doesn't follow view direction for logs, furnaces etc".
**In a1.1.2 nothing follows the view direction.** Re-checked in the jar: `ly` has no method taking
an `EntityLiving`, so there is no `onBlockPlacedBy` (it arrives with pumpkins in a1.2). A log (`mg`)
has no orientation at all, just top/bottom rings from `a(I)I`. The user chose faithful a1.1.2
over a Beta-style heading rule. Two real faults were under the report:

- **Every furnace drew its mouth on +Z.** The cube stream draws `faces`, the inventory answer, and
  never read the metadata the placement wrote. `ku.a(Lnm;IIII)I` puts the mouth on side ==
  metadata, and a lit furnace's mouth in the world is tile 61 (`bb + 16`), not 44. The fix:
  `metadataFaces` in blocks.json (extractor, jar-verified), `block::worldFaces`, and furnaces off
  the fast path.
- **Furnace and stair orientation came from a face table that the jar does not have.** Neither
  class overrides `onBlockPlaced`. Both turn from their neighbours in `onBlockAdded` (`ku.h`,
  `km.h`), and the one-stone sweep made that look face-driven. Ported as
  `TickBehaviour::Furnace` and `::Stairs`, including the staircase turning into its model block
  under anything solid. The sweep now lists the six classes that do override `onBlockPlaced`, and
  `gen_selection.py` tables only those. Details: [physics-a1.1.2.md](physics-a1.1.2.md), *Which
  way a placed block faces*.

The generated `placement_vectors.hpp` is otherwise byte-identical to the checked-in one. Coverage in
`placement_test.cpp`: the port's own write-then-`blockAdded` against every case of the sweep, plus
walls, corners, flights and a block on top. `mesher_test.cpp` covers the mouth. Host suite
**1163/1163**; the 3DSX build passed; `extract_blocks.py --verify` and `gen_selection.py --check`
agree. **No hardware run.** The chest, whose world texture reads its neighbours, was the one thing
left out of this and is done in section 50.

### 21. Entity hitboxes were the arrow's, and the outline ignored what the click would hit

Reported as "entity hitboxes feel too big, so it becomes impossible to place a rail below a
minecart". The report also asked that the block outline stop when an entity would be hit first.
Four differences from `iq.a(F)` (EntityRenderer.getMouseOver), each checked in the jar:

- **The border was 0.3; the original's is 0.1.** The boat's pick said the 0.3 was the entity
  pick's own. It is `EntityArrow`'s. A grounded cart's box tops out at 64.7. Under 0.3 the grown
  box reached the top of its cell, so every shot at that cell entered the cart first. With 0.1 it
  stops at 64.8, which leaves a strip at the top of the neighbour's side face clickable.
- **No distance limit to three blocks.** Entities were picked out to the block reach of four.
- **No clip to the block hit.** A cart behind a wall took the click.
- **Pool order, not distance.** Paintings, then boats, then carts: a boat behind a cart won.
  Distances were also measured to the centre, not the intercept.

`item::pickEntity` now transcribes it over all three pools (core/item/use). It returns an
`EntityTarget`, and `attackEntity` and the new `interactWithEntity` act on that target. The
per-pool `pick` methods and `rayHitsBox` are gone. `editBlocks` computes the target once per
click:
- R attacks the target.
- L interacts with it. A refused `interact` (a chest cart, an occupied vehicle) no longer falls
  through to the block behind it, but the item's own `useItem` still runs, as in `clickMouse`.

The outline is cleared on frames where an entity takes the crosshair.

Coverage in `tests/minecart_test.cpp` (six cases), with `boat_test`, `painting_test` and
`use_test` moved to the new API. The reported case is `a_rail_goes_back_under_a_cart_that_has_lost_its_track`.
A mutation back to 0.3 fails it, and fails the border cases for the cart and the painting.
Dropping the block clip fails the wall case. Host suite **1169/1169**; the 3DSX build passed.
**No hardware run.**

### 22. Greedy meshing: a cube atlas to repeat tiles in, and a seam for the rasteriser

Equal neighbouring cube faces are now one quad. That means the same plane, face direction, tile and
light byte. Both cube formats carry it. It is on by default, with a debug-page row to turn it off.
The blocker recorded in §2 was the atlas, and three hardware facts decided the shape of the fix.

**1. The PICA cannot repeat one tile of a shared atlas.** Wrap mode belongs to the whole texture,
and the fragment stage has no `fract`. So every tile a cube face can show is stored pre-repeated
in a 64×64 slot of a second texture, the **cube atlas** (`core/mesh/cube_atlas.hpp`). A slot is an
8-texel gutter, three copies of the tile, and another 8-texel gutter. The gutters repeat the tile's
own first and last texel. A merged quad samples `[0, w] × [0, h]` copies from the end of the near
gutter, which caps a run at 3 along either edge.
- **The gutter came from hardware (first run, below).** The first layout was 4×4 copies edge to
  edge, and every block showed a few texels of the neighbouring slot: red on the corners of grass
  tops, which is TNT's slot 8, directly below grass's slot 0. The ⅛-texel inset is half as large in
  texture coordinates at 512 texels as at 256, and a run spans four tiles, not one. Together that
  was more error than the inset absorbs. Now a stray sample has to be 8 texels out to reach
  another tile; anything less reads the same tile's edge texel.
- **What the gutter does not cover:** the far edge of a run shorter than three, which ends against
  the next copy of the *same* tile. The inset still stands there.
- **The fourth copy paid for the gutter.** On the real world, runs of 3 cost 5.8 % more quads than
  runs of 4 (1,057,821 against 999,579). Keeping 4 with a gutter would have needed a 1024×512
  atlas, 2 MB of VRAM.
- **The size is set by VRAM.** A slot for all 256 tiles would be 1024², 4 MB at RGBA8, the figure
  `atlas_image.hpp` already refused. Cube blocks can reach only 54 tiles in a1.1.2, so slots go to
  those alone: 8×8 slots in 512×512, **1 MB**.
- The layout is computed at compile time from the generated block table. It covers every cube-type
  block's faces, the furnace's metadata rows, the unknown block and tile 0. A `static_assert`
  fails the build if a version ever needs more than 64 slots; a 2×2 repeat would hold all 256.
- **VRAM order:** the cube atlas is allocated first, then the ordinary atlas. The upload goes in
  eight 128 KB bands through the existing 256 KB staging buffer, so a texture-pack reload with a
  full pool never asks the linear heap for 1 MB.
- `updateTile` also rewrites the tile's whole slot, copies and gutters. No animated a1.1.2 tile is
  a cube tile, so that path is dormant.
- The detail and translucent passes keep terrain.png. Fluid reads across tile edges on purpose, and
  nothing but cubes merges.

**2. Merged quads create T-junctions, and the PICA rasteriser cracks at them.** It snaps each
vertex to 12.4 fixed point (1/16 px, as Azahar models it). A corner lying mid-edge of a merged quad
lands up to ~0.044 px off that edge. The sliver between belongs to neither quad and shows the sky
colour. Unmerged faces never do this, because they share corners exactly.
- The fix is a **seam**: each merged quad grows by `k = seam.x·max(w,0) + seam.y` blocks on every
  side, where `w` is the view distance.
- `seam.x` is ⅛ px over the focal length (`120/tan(fov/2)`), so the growth is ⅛ px at any
  distance. `seam.y` is a 1/1024-block floor. The clamp on `w` stops corners behind the near plane
  from shrinking a quad.
- Single faces get zero growth, so an unmerged mesh draws exactly as before.
- **12-byte format:** `WorldVertex::face` became `seam = face + 6·code`. Code 0 means a single
  face; code 1+corner is a merged corner. `world.v.pica` indexes a 30-entry `seamDir` table
  (`mesh::seamDirection`) with that byte directly.
- **Quad format:** the old `ao` byte is the extent, stored as `w + 16h`. `quad.v.pica` unpacks it,
  lengthens the edges to `(w+2k)` and `(h+2k)`, and moves corner 0 by `−k(e1+e2)`. It sizes `k`
  from corner 0's `w` for all four corners; the floor covers the corners that come out short.
- The texture is not stretched to follow the growth. That costs ⅛ px of texel misalignment at a
  merged edge.

**3. The merge must be cheap on the main thread.** Faces go into per-face, per-layer bitmask rows,
51 KB on the heap. The merge clears each bit as it covers it, so there is no per-section clear.

Host measurements on a copy of the real 1119-column world (`--mesh`, with `flat` for the old
mesh):

| | cube quads | mesh bytes, 12-byte | mesh bytes, geoshader | face emit (O3) |
|---|---|---|---|---|
| one quad per face | 2,135,496 | 107.45 MB | — | 26.3 µs/section |
| greedy, runs of 4 (first layout) | 999,579 (2.14× fewer) | 55.46 MB | 17.33 MB | 26.9 µs/section |
| greedy, runs of 3 (current) | **1,057,821** (2.02× fewer) | 58.12 MB | 17.78 MB | 26.2 µs/section |

- Resident at distance 8 with runs of 3: 11.16 MB → 8.78 MB (12-byte) and 2.24 MB (geoshader).
- Vertex-shader length: `world.v.pica` 22 → 28 instructions per vertex; `quad.v.pica` 44 → 69 per
  quad; the geometry shader is unchanged at 29. That is ~18–27 % more shader work per quad, against
  2× fewer quads.

**First hardware run** (runs of 4, geoshader default, as reported by the user). Greedy on: **22 ms
GPU / 25 ms CPU**. Greedy off: **34 ms GPU / 44 ms CPU**. It also showed the cross-slot texel bleed
that the gutter above fixes. The runs-of-3 layout has not been on a console yet.

**Verified without a console:**
- `tests/greedy_test.cpp` takes 48 terrain and noise sections apart into unit faces. Each greedy
  mesh equals the flat mesh as a multiset of (cell, face, slot, light). The tests also cover
  winding, the run cap, outward seams, a clean builder after a flush, and the cube atlas layout
  and gutters.
- `quad_format_test` compares the two formats with merged quads included.
- A scratch PICA interpreter ran the **assembled** `.shbin`s over 62 quads (46 merged). The paths
  agree to 1e-15 in position and UV under affine matrices, with and without the seam. The 12-byte
  shader matches `M·(corner + k·dir)` exactly.

**What only hardware can say:**
- Whether the gutter clears every wrong texel. If a few remain at the far edge of short runs, they
  are the same tile's opposite edge, and the next lever is the inset (`kCubeUvInset`, and the
  constants in `quad.v.pica`).
- Whether ⅛ px is enough seam on the real rasteriser (look for sparkles along merged edges with the
  seam uniform at zero, then at ⅛).
- The wireframe now marks each quad's two start edges in white over the grey block grid, which
  shows the runs.

Host suite **1182/1182** (ASan/UBSan); the 3DSX build passed. **No hardware run.**

### 23. The four peaceful animals: a mob that is a table, a pathfinder with a dead loop, and nothing that is not a1.1.2's

The pig, the sheep, the cow and the chicken are in: they spawn, wander, path around walls, make
noises, take hits, drop what they drop, are milked, saddled, ridden, sheared, lay eggs, despawn,
save and reload -- and do **nothing this version does not do**. The whole derivation is
`docs/mobs-a1.1.2.md`; this is what was decided and what it cost.

**One struct and a table, not four classes.** `mv`, `bo`, `am` and `mz` differ in ten numbers and
four one-line methods, three of which are "which sound do I make". `core/entity/mob.hpp` is
`MobDef` plus one `Mob`, which is the shape the rest of this project already uses for render types,
tick behaviours and block shapes, and it keeps the pool flat.

**`PlayerBody` is `EntityLiving`'s body now, and that is a rename rather than a change.** A pig
runs the same `moveEntity` and the same `moveEntityWithHeading` a player does -- the jar has one
copy and `EntityPlayer` is a subclass of the thing that holds it. What differs is four numbers, so
`width`, `height`, `yOffset` and `stepHeight` became fields with the player's values as defaults,
and `LivingBody` is an alias so a mob does not have to read "player" to find its own physics. The
22-case bit-exact oracle still passes unchanged, which is what says the defaults did not move.
**`yOffset` is 0 for a mob**: only `EntityPlayer` sets 1.62, so an animal's `posY` is its feet.

**Two bugs in a1.1.2 were found and treated differently.**
- `cz`'s `getVerticalOffset` loops over the entity's size and then reads the *parameters* instead
  of the loop variables, so the loop is dead and a1.1.2 paths a cow as a point. **Kept** (and named
  `kSizeIsIgnored`): it is not a limit the hardware imposed, it is what the search explores, and
  fixing it would change every path and add a volume test per node.
- `a`'s `equals` compares a packed `x | y<<10 | z<<20` hash, so cells 1024 apart and any negative
  coordinate collide. **Fixed**, because that is a hash written as an identity and this project
  tests negative coordinates on purpose.
- A third, cosmetic: `ew` gives sheep, cow and chicken the same numeric id (91). Nothing here keys
  an animal by number; the save uses the string.

**The spawner is the only source of animals and is deliberately slow.** `az` is referenced by `ia`
and by nothing else -- there is no world-generation spawning in this version at all, which is why
a fresh world is empty. Three quirks shape the rate and all three are kept: a chunk whose first
draw lands in solid ground ends the *whole pass* (`return 0`, and the draw is a flat `rand(128)` in
y, so most passes stop at the first chunk they look at); `rand(1) - rand(1)` is always zero, so a
group never scatters vertically; and the cap of 15 is checked once before three passes, so a tick
can finish over it.

**The renderer needed no new pipeline and no new bind.** Eleven pages now live in one 256 x 128
entity sheet (it was 256 x 64; every existing page kept its origin), so fifteen animals of up to
twelve boxes each are **one draw call and one bind** on the existing detail pass -- the same one
the boat and the cart ride. The budget is 16 animals in range at 288 vertices each: 72 KB of
linear memory, nearest-first past it.
- A sheep with a fleece and a saddled pig are **two models each**, which is `ns`/`gm`'s
  `shouldRenderPass`; shorn and unsaddled they are one.
- The hurt flash is a second pass in the original (`glColor4f(brightness, 0, 0, 0.4)` at depth
  equal). A `DetailVertex` carries its own colour, so this pulls green and blue down instead: no
  extra pass, no blending, and a white sheep flashes pink rather than red.
- `RenderLiving`'s `glTranslatef(0, -24 * 0.0625 - 0.0078125, 0)` runs *inside* `glScalef(-1, -1,
  1)`, so it lifts rather than sinks. Getting that sign wrong buries every animal to the ears; it
  was wrong once and `a_drawn_animal_stands_on_its_own_feet` is what caught it.

**Breeding was built and then removed, on instruction, and this is the record of it.** The first
pass of this section shipped 1.1's rules read out of that jar's `ba` -- 600 ticks of love on being
fed wheat, a mate of the same kind within 8 blocks, 60 ticks within 3.5 of it, a baby at `-24000`
and both parents at `6000` of one `growingAge` field, half size while small -- plus an invented
`Mob::persistent` that exempted a fed or bred animal from the despawn. It was labelled an
extension throughout and was still **the wrong call**: the user asked for parity with a1.1.2, and
a1.1.2 has no breeding, no babies, no wheat use beyond bread and no way to keep an animal alive.

All of it is gone: `inLove`, `growingAge`, `breedTicks`, `persistent`, `findTarget`, `tryBreed`,
the nine breeding constants, `Mob::baby()`/`scale()`, `MobPlayer::holdingWheat`, the `Age`/`InLove`/
`Persistent` NBT tags and the four tests that covered them. **The lesson is the one this project
already records**: an extension that is easy to argue for is still an extension, and "labelled
clearly" is not the same as "asked for". A later version's behaviour belongs in a later version's
port, not behind a comment saying it is not this one's.

Two things the removal simplified rather than merely deleted. `ek.b_()`'s target branch is gone
with it, which is honest -- `ag` never overrides `findPlayerToAttack`, so no animal in this version
ever holds an `entityToAttack` and that half of the method was dead code kept alive by breeding
alone; `updateActionState` is now the wander and nothing else. And the size fields all left with
it, so a mob's box and model are its type's, full stop.

**Animals despawn**, which surprises people who learned Minecraft after Beta 1.8: further than 128
blocks from the player is immediate, and past 600 ticks of age it is 1-in-800 a tick unless the
player is within 32. Being hit resets the age.

**What it costs, and none of it is a hardware number.**

| | |
|---|---|
| `sizeof(Mob)` | **680 bytes** measured, of which 388 are the path and 176 the body |
| Fifteen animals | **10.0 KB**; the pool holds 32 from construction, so 21.3 KB |
| Path arena | 1,024 nodes = **32 KB**, plus a 4 KB heap and an 8 KB table, taken once and reused |
| Path budget | one search per tick across all animals; fifteen animals ask about 0.4 times a tick |
| Draw | one call, one bind, at most 4,608 vertices (72 KB) |
| Vertices per animal | 144 (pig, shorn sheep), 192 (chicken), 216 (cow), 288 (fleeced sheep, saddled pig) |

**Two things the tests found, both real.**
- **Knockback pushed the wrong way.** `attackEntityFrom` passes `entity.posX - posX` and
  `knockBack` *subtracts* it, so the vector runs from the mob to its attacker. Built the other way
  round, a punched cow walks into the fist.
- **Dying in lava dropped nothing.** Fire, lava, drowning, suffocation and the void are all
  `attackEntityFrom(null, n)` in the jar; subtracting the health directly instead skipped the
  drops and the death sound. They all go through `attack` now, which is also what gives them the
  invulnerability window they are supposed to have.

**Coverage.** `tests/mob_test.cpp`, **25 cases**: the table against the class files, ten punches
and the drops, the invulnerability window's difference rule, the sheep's one fleece and who may
take it, a death in lava that still leaves leather, the bucket, the saddle and the two clicks it
takes, the egg clock, the chicken's damped fall, a wander that stays on the ground, a pen nothing
escapes, two animals shoving each other apart, a wall the pathfinder goes round through its gap, a
sealed room it refuses, despawn by distance, **that nothing a player holds prevents it**, the spawn
rules, the cap, darkness, the crosshair and the click, the four models and their part counts, an
animal drawn standing on its own feet, the hurt tint, and the sheet the six new pages sit in.
`tests/entity_persistence_test.cpp` carries four animals -- a saddled pig, a shorn sheep, a hurt
cow with a despawn clock on it, and a chicken mid-egg-clock.

Suite **1210/1210** (ASan/UBSan); `entity_persistence` (6), `housekeeping` (2) and the animals
(13) pass under TSan; the 3DSX build passed. **No hardware run** -- nothing in this section has
been seen on a console, and the draw budget above is a vertex count rather than a frame time.

### 24. The entity sounds, and the boot list that was silently swallowing half of them

`docs/audio-a1.1.2.md` §*What an entity plays* is the derivation and the tables; this is what
landed and what it cost.

**Three of a1.1.2's entity sounds had no emitter and two more had one that could never be
heard.** The second half is the one worth writing down, because nothing about it looked wrong:

- **A key that was never preloaded is silence, deliberately and without an error.** There is no
  filesystem and no Vorbis in the per-frame path, so `playSoundFX` is a handle and two floats and
  can be nothing else -- see `core/audio/sound_engine.hpp`, which has said so since the menu
  click landed.
- The boot list lived in `platform/ctr/main.cpp` as six literals plus `preloadBlockSounds`. So
  every key added anywhere else in the tree had to be *remembered* into a file three layers away
  from it, and twice it was not: **`random.bow` has been mute since the bow landed**, and so has
  **every one of the four animals' eight `mob.*` keys**. They were transcribed correctly, played
  through the right seam at the right volume and pitch, and produced nothing.
- The list is `audio::preloadEffects` now (`core/audio/effect_preload.{hpp,cpp}`), and half of it
  is *derived*: footsteps and break sounds off the generated block table as before, and the
  animals' three-apiece off `MobDef` through `entity::preloadMobSounds`. What is left listed is
  what no table knows -- a door, a lever, a plate, and the sounds `Entity` itself plays.
- `tests/entity_sound_test.cpp` →
  `a_key_that_is_played_is_a_key_that_is_preloaded` walks both tables and checks every key
  against the boot set. It cannot decode anything, which is the point: it compares the two lists,
  which is exactly the comparison nobody was making.

**The cap moved because it could finally be measured.** `ctr::kMaxSamples` was **48**, chosen off
the block table alone and against Mojang's own a1.1.2-era resources folder -- 35 files. That was
already fiction on two counts: the animals and `Entity` name eleven keys no table lists, and a
player does not have the a1.1.2-era folder, because that S3 bucket has been gone for years. They
copy whatever tree they have, and a modern one carries **eight** variants of `step.grass` where
the old one carried six.

`--audio-list <resources>` now decodes the whole boot set on the host exactly as the console
decodes it at boot. Against the real folder on this machine:

| | |
|---|---|
| samples | **72** |
| frames | 1,780,061 |
| PCM | **3,500.8 KB** |
| per sample | 48.6 KB |

So `kMaxSamples` is **80** -- the measurement plus room for a pack with a few more variants --
which is ~3.9 MB of linear memory against the 16 MB `__system_allocateHeaps` reserves for
everything that is not the mesh pool. Partial loading is still the failure to avoid rather than a
thing the loader enforces: `playSoundFX` draws its variant *before* it knows whether that file is
resident, so a key with four of its eight loaded is a footstep that is silent half the time.

**What now has an emitter.** All of it goes through `TickWorld::playSoundAt`, the seam the
pressure plate's click already used, so no pool grew a sound engine:

- **`random.splash`** -- `kh.y()`'s water entry, in `core/entity/water_entry.hpp` as a shared
  edge detector because five pools need the same eight lines. **Which pools is derived**:
  `kh.e_()` is one line, `y()`, so an entity splashes exactly when its own `onUpdate` calls
  `super.onUpdate()` -- the player, the four animals, dropped items, arrows and boats do;
  **the falling block, the painting and the minecart do not**, and enter water in silence. That
  is a1.1.2's.
- **`random.fizz`** -- a stack landing in lava (`dx.e_()`, 0.4 and `2.0 + r·0.4`), which
  `item_entity.cpp` had a comment apologising for; and a burning animal reaching water
  (`kh.c()`'s tail, 0.7 and `1.6 + (r−r)·0.4`).
- **`random.drr`** -- an arrow strike, and this was a *bug fix* as much as a sound. It was a
  count the frame loop read back through `ArrowSystem::struckLastTick()` and played **at the
  player's own ears**, so an arrow that landed forty blocks away sounded like one at your feet.
  It plays from `core/entity/arrow.cpp` at the arrow now and that accessor is gone.

Three findings, each checked rather than assumed:

- **The splash is an edge and needs two bits, not one.** `inWater` alone makes a swimmer splash
  twenty times a second; `firstUpdate` alone is what stops every boat afloat announcing itself
  on the tick a save is reloaded. Both are `kh`'s (`aV` and `c`).
- **A floating boat flickers, and it is faithful that it does.** `dc` does not override `g_()`,
  which insets the box 0.4 top and bottom -- and a hull is 0.6 tall, so the probe is inverted and
  its answer changes as buoyancy rocks it across a cell boundary. Measured: **3 splashes in 240
  ticks at volume ~0.03**, two orders below a dive, before distance attenuation. It is inaudible
  because of the volume expression rather than by luck, so it is kept.
- **The splash volume is the previous tick's motion**, because `y()` runs before anything moves,
  and the horizontal terms are weighted 0.2. That is the whole character of the sound and it is
  checked against the expression rather than against a constant.

**One stated deviation.** `kh.c()`'s tail asks the *mutating* `handleWaterMovement`, so in the
jar an entity in a current is carried a fourth time per tick. `PlayerBody::move` does not run
that tail for anybody, player included, so the mob fizz uses `isMaterialInBox` with the same
inset -- the test without the push. Taking the push for animals alone would drift a cow down a
river four thousandths of a block a tick faster than the player swimming beside it.

**Still no emitter** *(as at §24; §25 closed four of these)*: `random.hurt` (`ge.d()`/`ge.e()` --
the player's own, which is Survival and is step 4), `random.explode` and `random.fuse` (nothing
lights TNT), the monsters' `mob.*`, the ambient cave counter, and `random.fizz` from flowing water
meeting lava (`core/tick/fluid.cpp`, the block half). `kg.b(dm)`'s `random.pop` is **not** missing
so much as unreachable: arrows cannot be picked up here, on the same no-depletion rule that makes
the bow cost nothing.

**Coverage.** `tests/entity_sound_test.cpp`, 8 cases, plus `tests/sound_catcher.hpp` -- the
mirror of `drop_catcher.hpp`, and the first test helper to wire `TickWorld`'s sound sink. Nothing
in it decodes anything or needs a device.

Suite **1219/1219**; the new cases plus `entity_persistence`, `housekeeping` and `sound` pass
under TSan; the 3DSX build passed. **No hardware run** -- and this section owes one more than
most, because the whole of it is a claim about what a console makes a noise doing.

### 25. The five hostiles: the dead half of the creature AI, an explosion, and a difficulty

`docs/mobs-a1.1.2.md` §*The five hostiles* is the derivation and the tables; this is what landed,
what it cost and what it is still short of.

**The zombie, the skeleton, the creeper, the spider and the slime.** They spawn where the light
does not reach, walk a real path to the player, hit, shoot, explode and split; they burn at dawn,
despawn twice as fast in the open, save and reload, and `k`'s one-in-a-hundred spider arrives with
a skeleton on its back.

**Nine rows, not five plus four.** `MobDef` gained six columns -- `ai`, `hostile`,
`attackStrength`, `moveSpeed`, `burnsInSunlight`, `talkInterval` -- and the monsters are five more
rows of the same table. The tick that was `ge.e_()` once is still `ge.e_()` once. What genuinely
branches is `MobAi`, which is the jar's five overrides of `ek.a(Lkh;F)V` plus the slime's refusal
to be an `ek` at all.

**`hostile` is a column because `co` is an interface.** `new k(200, co.class, ...)` counts the
slime, which is a `ge` and not an `ek`, so `type >= Zombie` would have been right by accident and
wrong in principle. The same column selects which `getBlockPathWeight` runs -- and those two are
the same method with the sign reversed (`0.5 - brightness` against grass-or-brightness), which is
the whole of "monsters keep to the dark" *and*, through `ek.a()`'s `weight >= 0`, the whole of
"and cannot spawn in the light".

**`ek`'s attack branch was dead and is now live.** It was left out when the animals landed with the
reason written down: `ag` never holds an `entityToAttack`, so porting it would have been porting
the monsters' behaviour with nothing to test it against. It is thirty lines and two of them are the
interesting ones -- the target and wander branches are *exclusive*, and `hasAttacked` turns the
walk into a sidestep about the target's heading, which is why a skeleton circles.

**What was new rather than ported:**

- **`core/entity/explosion.{hpp,cpp}`** -- `je`, all three phases. It is in `core/entity/` and not
  in `core/tick/` because it needs `dropBlockAsItem` from one and `rayTraceBlocks` from the other,
  and `entity` already depends on `tick`. The cell record is a **616-byte bitset** over a cube of
  radius 8 rather than a `HashSet` -- on the caller's stack, no allocation, and enough for any
  strength up to 8.4 against a creeper's 3.0, which reaches 3.7 blocks.
- **`getExplosionResistance` is `max(resistance * 3, hardness * 5) / 5`**, derived rather than
  generated: `setResistance` stores `arg * 3` and `setHardness` raises it to `arg * 5`, and every
  block in this version calls `setHardness` first. Checked against all seventy rows.
- **`dropBlockAsItem` took a `chance`**, which is not a generalisation invented here -- the jar's
  method has always had it and `dropBlockAsItem` has always been the call with 1.0. The explosion
  is the one caller that passes anything else.
- **A difficulty**, in `<world>/3dalpha.ini` beside the gamemode, with a World Settings row. It is
  a1.1.2's own setting (`cn.l`) and three things read it. **Peaceful is a removal and not a
  suppression**: `dq.e_()` kills the monster at the end of the tick it notices, so the 200-cap
  spawner still runs and still costs what it costs.
- **A player-hurt seam.** There is no player health (Survival is step 4), so
  `MobSurroundings::hurtPlayer` carries the hit out and `platform/ctr/main.cpp` takes the two parts
  that exist -- `ge.a(Lkh;IDD)V`'s knockback and `random.hurt` -- behind `ge`'s ten-tick
  invulnerability window, and counts the damage rather than pretending it landed.
- **Arrows reach mobs and the player.** `ArrowTargets` gained both, which is the other half of the
  bow landing. An arrow's `fromPlayer` is true whoever fired it, because that flag means "an
  `EntityLiving` did the hitting" and is what `bo.a(Lkh;I)Z` tests before it sheds wool.

**Three things that are only findable in the bytecode:**

- **`World.getBlockDensity` is not integer division.** It decompiles as `return i2 / i3` with both
  operands `int`, which would make explosion cover a truth value instead of a fraction; the class
  file has `i2f` on both before the `fdiv`. Checked before it was written, not after it looked
  wrong.
- **A slime splits at `health == 0` exactly**, not at `<= 0`. A size-4 slime taking 20 damage in one
  blow ends on -4 and does not split. Reproduced.
- **`cu.a(J)`'s slime-chunk hash wraps at 32 bits three times out of four**, and only
  `chunkZ * chunkZ` is widened before its multiply. A version that promoted everything to 64 bits
  picks different chunks.

**What a creeper leaves, and the one Easter egg.** A creeper that *explodes* drops nothing and
makes no death sound -- `setEntityDead` never reaches `onDeath`, so gunpowder is only ever from one
you killed. And `dd.b(Lkh;)V` drops `record13 + rand(2)` when the killer is a **skeleton**, which is
the only source of a music disc in a1.1.2: nothing crafts one and no chest generates one. The arrow
carries who fired it (`ArrowShooter`) so that the drop survives; the jar does not save
`shootingEntity` and this port does, because forgetting would lose the disc across a world close.

**Rendering.** Four new models -- `cr`/`cb`/`fv` (biped, and the skeleton is the zombie's pose with
thinner limbs), `em` (creeper, six boxes drawn out of seven built), `jy` (spider, eleven), `hh`
(slime, five across its two passes). `Placement` gained a per-axis scale, because `dn`'s
`preRenderCallback` is the hook the creeper's swell and the slime's size both use and it runs
*between* the `-1,-1,1` flip and the model's lift -- so the scale multiplies the lift too, and a
swollen creeper rises off the ground rather than sinking into it.

**The entity sheet grew 256 x 128 -> 256 x 256.** Four rows of four 64 x 32 pages held sixteen and
seventeen were needed; the GPU wants both dimensions a power of two. Every page above the monsters
kept its origin, it is still one draw call and one bind, and fifteen pages are now free. It costs
128 KB of sheet and the same again of texture memory.

**The spider's eyes are one part rather than a second model**, and it is a stated deviation: `ok`
draws all eleven boxes of `jy` again from `mob/spider_eyes.png` and blends at
`(1 - brightness) * 0.5`, but that page is transparent everywhere except the head, so the other ten
would write 240 vertices for the alpha test to throw away. The blend becomes an alpha cut, so the
eyes do not dim in daylight.

**`ctr::kMaxSamples` 80 -> 128, measured again rather than extrapolated.** `--audio-list` against
the real resources folder on this machine:

```
 72 samples, 1,780,061 frames, 3,500.8 KB   -- the animals  (§24)
108 samples, 3,386,798 frames, 6,638.9 KB   -- and the monsters
```

The monsters cost half as much again as everything before them put together: eleven distinct
`getSound` keys with two or three variants each, plus `random.fuse`, `random.explode`,
`mob.slimeattack` and `random.hurt`. At the measured 61.5 KB a sample the cap is now ~7.9 MB of
linear memory against the 16 MB reserve -- **half of it rather than a quarter**, and the number to
re-measure before anything else large goes in.

**Still not ported:** `hl` (Giant -- registered at id 53, drawn at six times scale, constructed by
nothing); the player's health, and with it `dm.a(Lkh;I)Z`'s difficulty and armour scaling; the
`explode`, `smoke` and `slime` particles and their draws; TNT, which is the explosion's other
caller.

**Coverage.** `tests/monster_test.cpp`, 24 cases: the table against the class files, the fist's
vertical test, the spider's daylight forgetting, a zombie burning under open sky and not under a
roof, a creeper lighting and un-lighting and taking the floor with it, a skeleton's cooldown and
its not shooting itself, the record, the slime's sizes and split and the overshoot that refuses to,
slime chunks against two seeds, the spawner filling a dark cavern and refusing a lit one, Peaceful,
the jockey, the blast against obsidian and through a wall, and the four new models. Plus three
monsters through `entity_persistence`.

Suite **1248/1248** (up from 1219); the new cases plus `entity_persistence`, `housekeeping` and
`sound` clean under TSan; the 3DSX build passed. **No hardware run**, and this is the section that
owes one most: two hundred models and a 1,352-ray explosion in front of a 268 MHz ARM11 is the
first thing in this project whose cost is a real question rather than a rounding.

### 26. The bug that refused every monster, and the discs the item table could not reach

Three things the first pass of the hostile mobs got wrong or left short, all
found by a player rather than by the suite.

**No monster ever spawned on the console, and every host test passed.**
`getCanSpawnHere` runs *after* the entity is built -- it has to, because
`ma.a()Z` reads the size its own constructor drew -- so by the time `ge.a()Z`
asks the world for colliding boxes, the candidate is in the pool.
`getCollidingBoundingBoxes(this, box)` excludes `this`; the port's box query is
`TickWorld::anyEntityIn`, a predicate with no identity in it, and the frame
loop's implementation of that predicate walks the same mob pool. **So every
candidate found its own box and refused itself**, one hundred per cent of the
time.

It passed 1,248 tests because **a bare `SceneWorld` sets no entity query at
all**, so `anyEntityIn` answered false for everything and the self-collision
could not happen. That is the lesson worth keeping: a seam that is unset in
every test is a seam with no coverage, and this one had been unset since it was
written -- the pressure plate that first needed it is driven directly rather
than through a world.

The fix is `NotYetInTheWorld`, a scope guard inside `canMonsterSpawnAt` that
clears `alive` for the length of the call. It is not bookkeeping bolted on: in
the jar the entity is a Java object that is not in `World.loadedEntityList`
until `spawnEntityInWorld`, and `getCanSpawnHere` runs in between -- the pool
*is* that list here and `alive` is what every reader of it means. It cannot be
an index test, because the frame loop's query has never heard of an index. It
was the caller's job for one commit and is the callee's now, because a contract
a caller can forget is a contract that gets forgotten.

`a_spawn_check_does_not_trip_over_the_candidate_itself` is the test, and the
fixture it needed is a `Cave::watchEntities()` that wires a real entity query.
Verified both ways: with the guard removed the test fails on `total > 0`.

**The two music discs were outside the item table.** Ids 2256 and 2257, which
is 1,910 past the end of a1.1.2's item run, so a contiguous array covering them
would be 2,258 rows to carry two. `registry.hpp` said they were rows "no a1.1.2
player can obtain" -- true until §25, when `dd.b(Lkh;)V` landed and a skeleton
killing a creeper became the version's only source of a record. So the feature
shipped dropping an item that had no icon, no name and no palette row, and drew
as an empty slot.

They get a **two-entry side table** (`kOutsideItemIds` / `kOutsideItems`,
emitted from the `outside` list `tools/configure.py` already computed), and
`item::def` falls back to a scan over it. The scan costs nothing for an id the
array reaches: the bounds check that used to answer "unknown" answers "look in
the side table" now. The palette is over every row rather than only the ones the
array reaches, so it is **149 items, not 147**.

They were also called `item_2256` and `item_2257`, because `getItemName` is null
for both -- `lg`'s second constructor argument is the *record* name that
`World.playRecord` takes, not an item name. `tools/genref.java` names them
`record_13` and `record_cat` now, out of a small map for ids above the run
rather than 1,910 nulls of array; the data was brought in line with what the
generator would emit, so a regeneration is a no-op.

**And `lg.a(...)` is a real method**: a record used on a jukebox (`ly.aZ`, id 84)
at metadata 0 sets the metadata to `shiftedIndex - record13.shiftedIndex + 1`
and plays the track. There is no jukebox behaviour in this port and `.mus` is
undecoded, so a disc is still an item you can hold and nothing more -- named
here so it is a gap rather than an oversight.

**The Creative tab is called Items and the inventory is called Inventory.** The
palette was named "Blocks" when it was blocks-only; it has been the whole item
table since M3 step 3 -- swords, ingots, armour -- and now the discs too, so the
label described a third of the page. The `PlayerPage` members are still `Items`
and `Blocks`; a page's name in code is not what a player reads. "Inventory" is
nine characters against ten cells of an 80-pixel tab, which is the width that
had to be checked.

Suite **1251/1251**; `monster`, `entity_persistence`, `sound` and the palette
tests clean under TSan; the 3DSX build passed. **No hardware run** -- and the
first of these three was precisely a thing that only a console showed.

### 27. Where the monsters actually go, and the chunk order that decided it

§26 fixed a spawner that refused every candidate. The next hardware report was
narrower: *"I still did not see any enemy spawn at night on the surface."*

**A fixture world cannot answer that.** A `SceneWorld` is a floor and some air,
so every drawn point that is not the floor is air, `az` never takes its early
return, and a probe over one reports a spawn rate no real world has -- the
earlier §26 probe measured 128 surface spawns in 6,000 ticks that way. So the
first thing built here was `--spawns`, a host harness that opens a **generated**
world through the real streamer, ticks it, and runs the same
`spawnMonsters`/`spawnAnimals` pair out of one random with the mob pool wired to
`setEntityQuery` -- the console's loop, with counters on it.

**`SpawnCounters` is now part of the spawner** (null by default, one predictable
branch per counted event) and is on the Info page, because "nothing spawns" and
"you have not walked into one" look identical from the top screen: `chunk`
climbing at all means passes are running and columns are resident, `floor` is
how many drawn positions had anywhere to stand, and `spawn` is what survived the
light and the box.

**What it found.** Over 18,000 passes of a real world, **17,986 ended on their
first or second chunk** -- `az` returns from the pass the moment a drawn point is
not air, and `k`'s `rand(rand(120) + 8)` lands inside rock better than nineteen
times in twenty. That makes the iteration order of the eligible-chunk set the
whole of *where a monster may appear*, and the port iterated it row-major. Every
monster came out of the northern rows: 52 spawns in a measured run, **none at
all in the southern third of the square**. The header comment said the set was
"the same set either way", which was the wrong reading of a `HashSet` sitting in
front of an early `return`.

**The order is derivable, and it was derived rather than invented:**

- `ol.hashCode()` is `(x << 8) | z`, which is a terrible hash and that is the
  point -- for any negative z the sign bits fill everything above bit 7, the x
  half is swallowed whole, and all nine columns of a row share one hash.
- `HashMap` files a key under `(h ^ (h >>> 16)) & (n - 1)` and iterates bucket by
  bucket. Eighty one entries resize the table to **128** and stop there, and
  `clear()` keeps the table it grew, so that is the table every pass iterates.
- Insertion order is `az`'s own: x outer, z inner, each -4 to 4.

`entity::eligibleOrder` is that, as a counting sort over 128 buckets -- ~350
bytes of stack, computed once per spawner call. `tests/monster_test.cpp` pins it
against **output from a real JVM**, for a square of all-positive coordinates, one
of negative x (where the rows come out upside down, which is the sign bits
reaching the bucket through `h ^ (h >>> 16)`), and one straddling the origin
(where four buckets hold fourteen chunks each).

**The one stated deviation**: a bucket of eight or more becomes a red-black tree,
and `moveRootToFront` puts the tree's root at the head of the chain -- so Java
hoists exactly one of each nine. Which one is the outcome of balancing nine
insertions, and it is also JVM-version-dependent (Java 7 spread hashes
differently and had no treeified bins at all). The bucket partition and the
bucket order -- the half that decides where monsters appear -- are exact; the
hoist is not reproduced, and the tests compare those runs as sets.

**And the surface is quiet because `az` is.** Held at midnight over 30,000 ticks
of a real world: **806 monsters, 14 of them above y 64**. The reason is one line
of the group loop -- `gy += rand(1) - rand(1)`, which is zero every time. The y
never moves off the draw, so a surface spawn needs `rand(rand(120) + 8)` to land
on the local ground height exactly, while a draw that lands in a cave is already
standing in one. **98.9% of drawn positions have no floor.** Every step of that
was checked against `az.a(Lcn;ILnu;)I` and `k.a(Lcn;II)Lmt;`: three passes, the
one-in-ten chunk, the radius of four, three groups of two tries, the `b1 = 6`
scatter, and the two `return 0`s.

Measured, same world, 6,000 ticks: **53 monsters row-major, 142 in hash order.**

Suite **1253/1253**, `monster` clean under TSan; the 3DSX build passed. **No hardware run** -- and
the Info page counters are there so the next one answers this without another round trip.

### 28. A Creative player is not something a monster hunts

`MobPlayer::targetable`, and it is the first rule here that **a1.1.2 cannot be
asked about**: the version has no gamemode, so it has no player a monster is
supposed to ignore. The behaviour copied is the one later versions settled on,
which is derived rather than invented -- targeting runs through
`TargetingConditions.forCombat`, that refuses anybody who `isInvulnerable()`,
and a Creative player is invulnerable. So the rule is stated as "invulnerable is
not prey", not as "Creative is special".

**Where the refusal goes, which is four places and not one:**

- `findTarget` -- `dq.i()`/`ax.i()`, so zombie, skeleton, creeper and spider
  never acquire the player at all. A creeper that cannot acquire one cannot
  light, and a skeleton that cannot acquire one never nocks an arrow; neither
  needed its own test in the code.
- The `else` branch that maintains an existing target, so **switching to
  Creative mid-chase drops the chase on that tick** rather than letting the walk
  finish. It arrives at the same `entityToAttack = null` a dead player does.
- `hopAbout`. The slime is not an `ek` and never reaches `findPlayerToAttack`:
  `ma.b_()` asks for the player itself, and `ma.b(Ldm;)V` -- damage by standing
  on you -- asks a second time. Both had to be told. Without it the slime would
  have been the one mob that still hurried towards somebody it cannot touch.
- `MobSystem::attack`'s new `provokes`, which is the swinging half:
  `dq.a(Lkh;I)Z` turns a monster on whoever hit it, and in the later versions
  the same invulnerable test gates `HurtByTargetGoal`, so it does not. A false
  `provokes` **lands the hit in full** -- the sheep is still shorn, the knockback
  still applies, the mob still dies -- and leaves no grudge. It defaults to true,
  so an arrow and a Survival fist are unchanged. Reached through
  `item::Attacker::provokes`.

**What deliberately did not change.** `present` stays true for a Creative
player, so a herd still despawns around them, a cow still turns its head to
watch them go past (the jar's look-at equivalent asks nothing about combat), and
walking into a pig still shoves it. "Invisible" here means invisible to the
target search, not absent from the world. **Spectator is left targetable** as
before: it has no body, so a mob that walks over to it reaches nothing and hurts
nobody, which is a different bargain and is the one already written down in
`platform/ctr/main.cpp`.

Six cases in `tests/monster_test.cpp`, including the both-ways slime check --
the same slime at the same distance does land on a targetable player, without
which "no hits" would also pass on a slime that had simply hopped away.

Suite **1259/1259**; the 3DSX build passed. **No hardware run.**

### 29. All twelve particles, and the thousand darts that throw most of them

Before this the pool held one kind: `iw`, the fleck a broken block throws. That
is not the particle a player sees most. Almost every particle in a quiet hour of
a1.1.2 comes out of `cn.m(III)V` -- **a thousand darts a tick at a 33-block cube
round the player**, each asking whatever it hits whether it would like a puff.
Nothing was breaking, so nothing was drawing anything.

**Twelve kinds, ten of which have a name.** `cn.a(String, DDDDDD)` is a chain of
ten string compares and the name is the whole API; `ParticleKind` is that list in
the jar's compare order. The two without a name are the two `EffectRenderer` and
`EntityRenderer` build themselves -- and **neither of those is what it looks
like**:

- `iw` (Digging) is the one that was already here.
- `nf` (Rain) is spawned **by nothing, in the jar too**.
  `EntityRenderer.addRainParticles` is gated on `Minecraft.J`, a field
  initialised to `false` and *assigned nowhere in the class files*. It is here
  because `kq` (Splash) is its subclass and inherits its whole tick.

`addBlockHit` -- `bq.a(IIII)`, the chip a pick throws on the ticks a block has
not broken yet -- is written and has no caller for the same kind of reason:
this build has no break progress, so there are no such ticks.

**Three sheets, because `EffectRenderer` keeps three lists.** `nq.c()` answers a
layer and one texture is bound per layer: `particles.png` for the eight sprites,
`terrain.png` for the digging fleck, `gui/items.png` for the two `ig` kinds.
`particles.png` is a **new 128 x 128 texture in linear memory, 64 KB** -- not a
page of the entity sheet, which has fifteen 64 x 32 slots spare and no 128 x 128
hole, and growing it to 512 tall would cost 256 KB to save one bind. The three
spans are built into disjoint parts of the one 32 KB buffer and drawn in three,
because a draw call names an address the GPU does not read until frame end --
the same trap `drawItemEntities` was written up for.

**A pack with no `particles.png` gets a generated stand-in**, the way one with no
`gui/items.png` gets terrain tiles. Every sprite on that sheet is white or grey
in the original -- the colour is `particleRed/Green/Blue` at the quad -- so white
discs through the real tints give grey smoke, red dust and an orange flame. The
smoke row shrinks from frame 7 to frame 0, which is the direction
`tile = 7 - age * 8 / maxAge` walks it.

**What each kind actually does was transcribed, not generalised**, and the
differences are not cosmetic: three kinds replace the base constructor's
normalised velocity outright; **six of the ten named kinds throw the caller's
motion away entirely**, because `e.a` passes only the position to their
constructors; `ba` and `nf` never touch `particleAge` at all and die on a
countdown; `jb` sets `noClip`, so a torch flame is not stopped by its torch;
`cq` is the only kind that spawns another, and the chance is `1 - elapsed`, which
is the trail behind a lava spit. Two override brightness -- a lava pop is full
bright for ever and a flame fades from full onto the world's light. Four rewrite
their scale every *frame* rather than every tick, so those ramps live in
`particle_mesh` with `partialTicks`.

**Two jar faults copied rather than corrected**, both named in the code:

- `jb`'s constructor jitters its position into three locals and never calls
  `setPosition` with them. Six `nextFloat` draws that move nothing; the draws are
  made here and the result is discarded, because a flame really does sit dead
  centre on its torch.
- `ai.i` tests each of the glowing ore's six motes with `py < 0.0` where every
  other axis is tested against the block's own coordinate. So the mote pushed a
  sixteenth *below* the floor is outside the block but not below zero, and is
  refused: **an exposed redstone ore glitters on five faces, not six**, and only
  one at the bottom of the world would sparkle downward.

**One fault of ours, found while rewriting the mesh.** `buildParticles` paired
the *high* u with the `-right` corners, where `renderParticle` pairs the low one
-- every sprite was mirrored horizontally. Invisible on a four-pixel chip of
stone, which is why it survived; not invisible on a flame.

**Where the particles are thrown from.** `World.spawnParticle` is a seam on
`TickWorld`, exactly as it is a `World` method in the jar, so a block behaviour
and an entity tick reach it the same way and a harness that installs no sink
runs all of the logic and draws none of it. Seven blocks answer the display tick
(torch, redstone torch, lit furnace, wire, lit ore, fire, the two fluids) and the
lit/unlit pairs were read off `ly`'s static initialiser -- the flag is a
constructor argument, so 62 smokes and 61 does not. On top of that: the water
entry spray (a row of each scaled by the entity's width, so a dropped item makes
six and a boat thirty-one), a mob's death puff and drowning bubbles, a slime's
landing ring, the explosion's `explode`+`smoke` per recorded cell, an arrow's
wake, a boat's bow wave, the furnace cart's chimney, and the steam when lava
turns to stone.

**The display loop draws its offsets from the world's own generator**, which
shifts the block-tick stream by 6,000 draws a tick. That is the jar's: `cn.m`
reads `this.n`, the same `Random` the random block ticks use. Copied rather than
given a stream of its own, because a stream that diverges from the jar's in a way
the jar's own client does not is not the more faithful of the two.

Twenty-nine new cases across `tests/particle_test.cpp`,
`tests/display_tick_test.cpp` and `tests/particle_mesh_test.cpp`. Suite
**1289/1289**; the 3DSX build passed. **No hardware run**, so the cost of a
thousand block reads a tick on an ARM11 is stated as a shape and not a
measurement: it is 20,000 reads a second through the column cache, against the
million-odd the mesher already does.

**Two sounds came with it**, because two of the seven blocks that answer the
display tick make a noise as well as a particle: `jp.b`'s `liquid.water` over
flowing water and `og.b`'s `fire.fire` over any fire. Both joined
`audio::preloadEffects`, and `--audio-list` against a real resources folder now
decodes **110 samples / 3,580,795 frames / 7,017.8 KB**, against `kMaxSamples`
128. `jp.i`'s `random.fizz` was already on the list and only needed the site that
plays it, which is the steam this section added.

### 30. Wrong pixels on rails: one staging buffer, two readers

**Reported as "rails have a few wrong pixels, I think from the greedy meshing".** It was
not the greedy mesher -- merging only runs over cube faces
(`MeshBuilder::addFace`), and a rail is a detail-stream sheet that samples the
ordinary 256x256 atlas, so no merged quad and no cube-atlas gutter is anywhere
near that fragment. `addRail` itself was checked against `bc.f` in the jar
instruction by instruction -- UV origin (`tex & 15 << 4`, `tex & 240`), the
corner->UV permutation, all four entries of `kTurns`, and which metadata raises
which corner -- and matches.

**The fault was in `Atlas::init`, and it was a race.** The function filled one
256 KB linear staging buffer, handed it to `uploadTiled`, and then passed the
same buffer straight to `initCube` to be overwritten with cube-atlas bands. With
the atlas in VRAM that upload is not a memcpy: `C3D_TexUpload` ->
`C3D_TexLoadImage` range-checks the destination against [0x1F000000, +0x600000)
and routes VRAM through `C3D_SyncTextureCopy`, which enqueues a GPU texture
copy. So the CPU went back to writing the buffer the copy engine was reading.

**Which tile loses was not luck.** Band 0 writes
`staging[tiledOffsetFlipped(x, y, 512, 512) - base]` over the cube atlas's top
64 image rows. Worked back through the *ordinary* atlas's own tiled-and-flipped
map, the first words it touches are x = 0..15 of image row 136 -- atlas row 8,
column 0, which is **tile 128, the rail**. The tiles it reaches after that are
129, 130, 131 and up: the rail is not one of the casualties, it is the first
one. Curved rails draw from tile 112 and were never touched, which is why only
the straight and sloped ones showed it.

**And the colours identify the source exactly.** The layout hands slots out in
ascending tile order, so cube-atlas slot row 0 is tiles 0..7: slot 0 is grass
top (97,161,55) and slot 4 is wooden planks (188,152,98). Bright green and
bright brown, which is how it was reported, and a race between CPU stores and
copy-engine reads is why the affected texels moved from one world load to the
next.

**`C3D_SyncTextureCopy` is not the guarantee its name suggests**, which is what
the old code leaned on for the band-to-band case as well. Read out of
`libcitro3d.a`: with a frame open it splits the frame and tail-calls
`GX_TextureCopy` with no wait at all; with no frame open it reaches
`gspWaitForEvent`, but on `GSPGPU_EVENT_PPF` with `nextEvent = false`, which
libctru documents as returning immediately when an unconsumed event of that kind
is already pending. A stale PPF from any earlier transfer satisfies it without
this copy having moved a byte.

**The fix is a second band-sized staging buffer, alternated.** Band 0 -- the one
that lands on the rail -- goes into a buffer the ordinary upload never touched,
and the bands alternate from there, so a band's copy always has a full band of
CPU work behind it before its buffer comes round again. 128 KB rather than a
second full atlas, because `Renderer::setAtlas` re-enters this on a texture-pack
change with the chunk pool already full.

Suite **1289/1289**; the 3DSX build passed. **No hardware run**, so the visible
confirmation is still owed -- the diagnosis rests on the tiling arithmetic above
and on the two colours matching, both of which are derived rather than observed
on a console.

**The tiling map itself was cleared on the way past**: `mortonInterleave` and
`tiledOffset` were checked exhaustively and are bijective over 0..63 and over
every word of both a 256x256 and a 512x512 atlas, so the upload has never been
shredding texels. And the `kUvInset` question the first pass raised is a dead
end worth keeping: at an eighth of a texel it never drops or duplicates a texel
column at any quad size from 8 to 256 px, and shifts a sample by at most 0.125
texel, so it cannot by itself produce a wrong-coloured pixel.

### 31. TNT: the four ways a1.1.2 lights it, and the one entity that was missing

The block, the drop table, the flammability, the explosion and both sound keys were all already
here. What was missing was `jd` -- EntityTNTPrimed -- and without it every ignition path in the
build ended in a comment saying so: `core/tick/drop.cpp`'s `blockDestroyedByPlayer`,
`core/tick/fire.cpp`'s `tryToCatch` and `core/entity/explosion.cpp`'s third phase each named the
gap rather than hiding it. A block of TNT could be placed and broken and did nothing at all.

**`core/entity/primed_tnt.{hpp,cpp}` is `jd`**, transcribed from the class file, and it is short:
one counter, `ff`'s physics with one difference, a smoke particle a tick, and `createExplosion` at
strength 4 when the counter runs out.

**Four ignition paths, and all four are ported.**

| Path | `q`'s method | Fuse | Sound |
|---|---|--:|---|
| A pickaxe | `b(Lcn;IIII)V` | 80 | `random.fuse` at 1.0 / 1.0 |
| A fire spreading in | the same, from `og.a` | 80 | the same |
| A blast | `c(Lcn;III)V` | `nextInt(20) + 10` | **none** |
| A neighbour going live | `a(Lcn;IIIII)V` | 80 | `random.fuse` |

The fourth is the one that had been waiting on a subsystem, and the subsystem turned out to be
finished: `TickWorld::isIndirectlyPowered` and `canProvidePower` are both there for the door and
the rail, so `tntNeighbourChanged` is six lines in `core/tick/redstone.cpp` gated exactly as
`doorNeighbourChanged` is.

**Breaking TNT in a1.1.2 lights it and hands you nothing.** `q.a(Ljava/util/Random;)I` --
quantityDropped -- returns a hard 0, and the `onBlockDestroyedByPlayer` override has no metadata
guard in this version; the `if (metadata == 1)` that makes a mined block drop itself arrives later.
So crafting is the only way to obtain a block of it, and `data/a1.1.2/drops.json` already said so
(`countMin: 0`). This is the kind of thing that reads as a bug and is not.

**Three numbers worth keeping**, each easy to get wrong by one:

- **`if (fuse-- <= 0)` reads the value before the decrement** (`dup_x1` in the class file), so a
  fuse of 80 is eighty smoking ticks and an eighty-first that explodes -- 81 ticks, not 80.
- **The constructor converts an angle that is already radians.**
  `f = Math.random() * PI * 2`, then `MathHelper.sin(f * PI / 180)`. That shrinks a full turn to a
  0.11-radian arc, so every primed block in a1.1.2 hops very slightly towards -Z and almost not at
  all on x. It reads as a straight hop and not a scatter. Transcribed rather than corrected.
- **The landing factors are applied on every tick it is on the ground**, not once on the tick it
  arrives -- which is where it differs from `ff`, whose entity dies on the same line. TNT dropped
  from a height hops and then settles.

**A blast re-primes rather than detonating**, which is the whole character of a chain reaction: a
recorded cell holding TNT gets `nextInt(20) + 10` ticks and no fuse sound, so a wall ripples over a
second and a half instead of going off as one bang. `je`'s phase three runs the hook **after** the
cell is written to air (offsets 1126-1151 of `je.a()`), so the entity is never standing inside the
block it came from. The draw is on the world's generator and is made whether or not there is a
pool to put the entity in, for the same reason `dropBlockAsItem`'s draws are.

**An entity spawned mid-tick is ticked in that same tick**, and that is a1.1.2's and not an
artefact: `World.updateEntities` walks `loadedEntityList` by index and `spawnEntityInWorld` appends
to it. The pool here behaves the same way, so a chain's second blast is one tick earlier than a
naive reading would predict; `tests/primed_tnt_test.cpp` pins it. Nothing cascades inside one tick
because the shortest re-primed fuse is ten.

**The white flash could not be a vertex colour.** `hw` (RenderTNTPrimed) draws the cube, then on
`fuse / 5 % 2 == 0` draws it again with the texture and lighting off, blended white at
`(1 - (fuse - partial + 1) / 100) * 0.8`. The detail pipeline modulates the atlas by the vertex
colour and modulation can only darken, so there is no vertex colour that makes a TNT texture white.
The flash goes through the **outline pipeline** instead -- the texture-free, blended,
uniform-tinted program the selection box and the crosshair already use, which is the closest thing
the PICA has to the fixed-function state `hw` sets. Because the tint is a uniform rather than a
vertex attribute, one flashing entity is one draw call; the budget is sixteen.

The one state not reproduced is the blend function. `hw` asks for `GL_SRC_ALPHA, GL_DST_ALPHA`,
which on a framebuffer with no destination alpha is not what it reads as; the ordinary over-blend
is what the effect looks like on a real client and is what this uses.

**The bug that came out of that, and the rule it cost.** The first version of the flash pass left
the GPU where it found it and restored three pieces of state by hand on the way out. It is not the
last pass in the eye -- four more entity passes and then the **translucent terrain pass** follow it
-- so the texture combiner it had torn down to one primary-colour REPLACE stage was inherited by
all of them. The visible symptom was the sea: the largest surface in the frame, drawn with no
texture, came out as a flat white sheet whose opacity was the fog amount the vertex shader parks in
primary alpha. The three values restored by hand were wrong as well -- `GPU_GEQUAL` where the world
uses `GPU_GREATER`, an alpha reference of 0 where it uses `kAlphaTestRef`, and a src-alpha blend
where it uses `ONE/ZERO`.

The fix is to call `applyWorldState()` and then re-clear the cull mode, and the general rule is
worth keeping: **`applyWorldState` is the single description of what a world pass runs under, so a
mid-eye pass that changes state restates all of it rather than undoing its own edits.** Undoing
edits by hand is a list that has to be kept in step with a function somewhere else, and this is
what it looks like when it drifts. `drawSelection` and `drawCrosshair` restore nothing and say so,
and they are correct because they are the *last* two passes in the eye -- which is the distinction
that was missed. `drawPrimedTnt` was the only pass clobbering the combiner before the translucent
terrain, so nothing else in the frame had the same fault hiding in it.

The depth comparison is the one piece deliberately kept off `applyWorldState`'s: the flash redraws
the same vertices through the same matrix, so `GPU_GREATER` would reject every fragment and the
flash would never appear. `GEQUAL` with writes off is what makes it an overlay. `hw` has no
equivalent of the selection box's 0.002 expansion, so neither does this.

**The swell is a fourth power.** `f = clamp(1 - (fuse - partial + 1) / 10, 0, 1)`, squared twice,
times 0.3. Four ticks from the end that is under three hundredths of the full 1.3, which is why
TNT looks its own size until the last half second and then visibly inflates. Both passes read the
same `primedTntScale`, because a disagreement between them would draw a white rind around the block.

**Both sound keys were already preloaded** for the creeper (`random.fuse`) and its blast
(`random.explode`), so TNT's sounds arrived as call sites and cost nothing: `ctr::kMaxSamples` is
unchanged. The block-side fuse is at pitch **1.0** against the creeper's flat 0.5, which is the one
way to tell by ear which of the two is about to go off, and it is played at the entity's position
-- `posY - yOffset`, so `y + 0.01` and not the cell's centre.

**Cost.** A `PrimedTnt` is **128 bytes** (measured on the host; six doubles of box, nine of
position and motion, the fuse and three bytes of state) and the pool holds sixteen from
construction, so 2 KB. The
renderer's two buffers are 36 KB (96 cubes) and 6.8 KB (16 flash cubes) of linear memory, once. The
expensive part is the blast itself, which is `je`'s 1,352 rays at strength 4 -- the same cost a
creeper already pays, and the note in `current-work.md` about that arithmetic on a 268 MHz ARM11
now has a second caller. **Not measured on hardware.**

Covered by `tests/primed_tnt_test.cpp` (twelve cases: the four ignition paths and the neighbour
that should be ignored, the fuse arithmetic, the physics, the chain, the generator-ordering
guarantee for a headless caller, and the three rendering functions) and by
`tests/entity_persistence_test.cpp`, which round-trips a fuse through level.dat. Suite
**1301/1301**.

### 32. A burning mob was invisible fire: `doRenderShadowAndFire`'s missing half

Reported as "the fire on mobs is invisible when they burn", and that is exactly what it was. The
tick half had been right since the mobs landed -- `kh.y()`'s counter, a heart every twenty ticks,
`mb.j()`'s catch at dawn, the fizz on hitting water -- and nothing in the frame path had ever drawn
it. `ak.b(Lkh;DDDFF)V` is `doRenderShadowAndFire` and it is two private calls: the shadow, which is
still not ported, and `ak.a(Lkh;DDDF)V`, the fire, which is now `core/render/entity_fire_mesh.cpp`.

**It is a stack of yaw-billboarded sheets and the whole of it is eight numbers.**

| The class file | What it means |
|---|---|
| `glDisable(GL_LIGHTING)`, `glColor4f(1,1,1,1)` | `light = 0xFF`, white vertices |
| `int i = Block.fire.blockIndexInTexture` | **one** tile for the whole stack |
| `float f5 = entity.width * 1.4F` | every number below is in units of this |
| `float f9 = entity.height / entity.width` | `ceil` of it is the sheet count |
| `glRotatef(-playerViewY, 0, 1, 0)` | yaw only, no pitch term |
| `glTranslatef(0, 0, -0.4F + (int)f9 * 0.02F)` | the stack leans toward the camera |
| quad x `-0.5 .. f6 - 0.5`, y `-f8 .. 1.4 - f8` | 1.4 tall, stepped 1 -- they overlap by 40 % |
| `f6 *= 0.9F`, `glTranslatef(0, 0, -0.04F)` | narrower and nearer as it rises |

**Four of those look like bugs and none of them is.** The quad is asymmetric -- only its +x edge
moves as `f6` shrinks, so the stack leans left rather than tapering. The sheets are a unit apart
and 1.4 units tall, which is what makes three of them read as one column of flame. The lean is
*toward* the camera, because fire drawn inside a mob is hidden by it. And **`(int)(1.8F / 0.6F)`
is 2**: the depth term truncates the ratio where the loop takes its ceiling, and the float
quotient is 2.99999976 rather than 3, so a zombie gets three sheets placed as though it had two.
Java divides the same two floats and gets the same quotient, so that is the original's own depth
and not an artefact of the port. It is pinned by test, which is the only thing that stops it being
tidied away later.

**One tile is the version-specific part.** Later versions alternate `fire_0` and `fire_1` down the
stack; a1.1.2 reads `blockIndexInTexture` once, above the loop. Block fire uses both tiles
(`core/mesh/shapes.cpp`) and entity fire uses the first. Which tile that is comes from
`texture::flameTile(0)` -- asked of the block that *renders as fire*, never of an id, the same rule
the mesher follows -- so a version whose table has no fire draws no entity fire either, and the
tile is the one `FlameAnimation` is already rewriting 20 times a second. The sheets animate for
nothing.

**A pass of its own, and it had to be.** The sheets are tiles of the block atlas and the mob is
the entity sheet, so no arrangement of buffers could have made them one draw. `drawMobFire` runs
straight after `drawMobs`, which rebinds the block atlas on its way out, so the pass costs one
draw call, no bind, and 12 KB of linear memory -- sixteen animals, the frame's mob budget, at four
sheets each, which is one more than anything in a1.1.2 can need. The cutoff is `draw_budget.hpp`'s,
nearest first, as every entity pass is.

**Mobs only.** `Mob::fire` is the only fire counter in the tree: a player body has none until
Survival gives it damage, and nothing else in this build burns. The hurt flash `buildMobs`
multiplies into a mob's vertices deliberately does not reach the flames, which is also the
original's arrangement -- the fire is a separate pass, after the flash.

Covered by `tests/entity_fire_mesh_test.cpp` (six cases: the sheet count including the degenerate
dimensions the original's loop would hang on, the geometry of the stack at yaw zero, the tile and
the full-bright white, the turn with the camera, and that only a mob with the counter running gets
any, plus the sheet a position near the edge of the 32-block detail window cannot express). Suite
**1309/1309**. **Not seen on hardware.**

### 33. Fire did not burn anything: `moveEntity`'s tail, and a conclusion that was wrong twice

Reported as "fire doesn't burn entities like dropped items or animals", and the port had it
written down as a *finding*: both `core/entity/item_entity.cpp` and `core/entity/mob.cpp` said that
nothing in a1.1.2 can set an entity's fire counter except the lava branch of `kh.y()`, on the
evidence that `og` -- BlockFire -- has no `onEntityCollidedWithBlock` and that no class outside
`kh` writes `kh.aT`. A test, `fire_does_not_burn_a_dropped_item`, pinned it. Every piece of that
evidence is true and **the conclusion is wrong**: `kh` writes its own counter, in `moveEntity`,
four times.

```java
// kh.c(DDD)V, after the sweep and the footstep
ySize *= 0.4F;
boolean flag = g_();
if (worldObj.isBoundingBoxBurning(boundingBox)) {
    dealFireDamage(1);
    if (!flag) { fire++; if (fire == 0) fire = 300; }
} else if (fire <= 0) {
    fire = -fireResistance;
}
if (flag && fire > 0) { playSoundAtEntity(this, "random.fizz", 0.7F, ...); fire = -fireResistance; }
```

The ignition is a **box test against the world**, not a callback from the block, which is why
looking for an entity hook in `og` found nothing and concluded nothing burns. `core/entity/
fire_entry.hpp` is the transcription and carries the correction.

**The counter is a fuse and rests below zero.** `fire` sits at `-fireResistance`; a tick in fire
increments it, and catching light is the tick it reaches **zero**, which is then set to 300.
`fireResistance` is 1 for everything the game constructs except `dm`, which sets 20 -- so an animal
is alight on its first tick in a flame and a player gets a second of grace. It is also why a
`level.dat` for a player who has never burned reads `Fire: -20`.

**A counter sitting at exactly zero can never catch**, because `fire++` runs before the test. Zero
is where the constructor leaves it and where `kh.y()`'s water branch puts it, so an entity dropped
straight into a flame, or one stepping out of a river into one, burns at 1 instead of at 300 -- it
still takes the damage and still counts as alight everywhere, but loses the fifteen seconds it
would have kept burning afterwards. Pinned by test rather than smoothed over.

**What a tick in fire costs depends on what the entity does with a hit**, and `dealFireDamage(1)`
is `attackEntityFrom(null, 1)`:

| Entity | `attackEntityFrom` | A tick in fire |
|---|---|---|
| `ge` -- the nine mobs | hurts, with a ten-tick window | about one point a second; a pig dies in ten |
| `dx` -- a dropped stack | five health, no window | **gone in five ticks** |
| `dc`, `oc` -- boat, minecart | `amount * 10` of a 40-point counter | broken after four seconds |
| `kg`, `ff`, `jd` -- arrow, falling block, TNT | `return false` | nothing; it just chars |

**What burns is exactly what moves.** The ignition lives in `moveEntity`, so the entities that
never call it never catch fire: `kg` -- the arrow -- and `jc` -- the painting -- have no call to
`kh.c(DDD)V` anywhere. That is derived rather than assumed; `ge` has three calls and they are the
three mutually exclusive branches of `moveEntityWithHeading`, so a living entity still runs the
tail once a tick.

**`isBoundingBoxBurning` is generous.** It walks `floor(min)` to `floor(max + 1)` on each axis
against three ids -- fire, still lava, flowing lava -- with none of the water probe's inset and
none of its surface arithmetic. An entity whose box merely touches the plane of a fire cell is
burning, which is why fire catches things that look like they are standing beside it. Asked here
as "the block that ticks as fire, or anything whose material is lava": the same set, without a
renderer or a tick naming an id.

**One deviation, inherited rather than introduced.** The jar's tail asks the *mutating* `g_()`, so
it carries an entity in a current one more time; this asks `g_()`'s inset box without the push, for
the reason `core/entity/mob.cpp` gave when it wrote the hiss half -- `PlayerBody::move` runs no
tail for the player, and taking the current for animals alone would drift a cow downstream faster
than the player swimming beside it.

**The renderer follows the same rule.** `buildEntityFires` now walks every pool that can be alight
-- mobs, stacks, boats, carts, falling blocks, primed TNT -- into one buffer and one draw, because
every sheet is the same tile of the same atlas. The stack stands on the entity's `posY`, which is
its feet for a mob and the middle of the box for the five whose `yOffset` is half their height.

**Not done: the player.** `dm` has the twenty-tick fuse and nothing to spend it on -- there is no
player health until Survival, which is M3 step 4 -- so `PlayerBody::move` still runs no tail. The
first-person fire overlay is the same gap seen from the other side.

Covered by `tests/fire_entry_test.cpp` (six cases: the fuse and the player's longer one, damage
through wet and the douse, what counts as a burning box including the touching cell, an animal
catching and dying, the zero-counter quirk, and a boat charring to planks), plus the rewritten
`fire_burns_up_a_dropped_item` and two new item cases in `tests/item_entity_test.cpp`. Three tests
changed because they had encoded the old reading: the item one above, the zombie-at-dawn pair in
`tests/monster_test.cpp` (which tested `fire == 0` for "not burning", and the resting value is now
-1), and the lava fizz count in `tests/entity_sound_test.cpp` (a stack over lava no longer lives
long enough to hop twice, because the tail has been burning it since its box touched the lava).
Suite **1318/1318**. **Not seen on hardware.**

### 34. The cactus: `onEntityCollidedWithBlock` at last, and an item shape read from the wrong table

Two reports from one play session -- "dropped cacti have a bit of empty space between the cube
edges" and "dropping an item onto a cactus takes very long to destroy it" -- and they are the two
tables a cactus is read out of, each giving the wrong one of its two shapes.

**The damage.** Step 11 recorded that `hy` is the second block to override
`onEntityCollidedWithBlock` and left it, "because it wants the entity, which the seam deliberately
does not carry". `fire_entry.hpp` had since shown the shape that does not need such a seam, and
this is the same one: the pool runs the loop over its own box and applies the hits through its own
`attackEntityFrom`. The loop is the head of `moveEntity`'s tail, immediately before the fire test
that section 33 ported:

```java
// kh.c(DDD)V, after the footstep and before the burn
int i = MathHelper.floor_double(boundingBox.minX);   // ... maxX, and the other two axes
for (int x = i; x <= l; ++x)
  for (int y = j; y <= m; ++y)
    for (int z = k; z <= n; ++z) {
      int id = worldObj.getBlockId(x, y, z);
      if (id > 0) Block.blocksList[id].onEntityCollidedWithBlock(worldObj, x, y, z, this);
    }
```

**Plain floors, inclusive bounds, no inset.** Later versions add a thousandth of a block at each
end; a1.1.2 does not, and that is what makes the whole thing work. A stack settles on a cactus's
collision top at 0.9375 of the cell, so its box bottom floors to the cactus's own y; an entity
pushed against a cactus's side is likewise inside the cell, because the collision box is inset a
sixteenth on x and z. That second one is why walking into a cactus hurts in the original at all.

`hy.b(Lcn;IIILkh;)V` is `attackEntityFrom(null, 1)` and nothing else -- no metadata test, no
cooldown, no sound -- so what a touch costs is entirely the entity's own `attackEntityFrom`, as
with fire: **a dropped stack is destroyed in five ticks** (five health, no invulnerability window),
an animal loses about a heart a second through its ten-tick window, a boat or a cart takes ten of
its forty points, and an arrow, a falling block and a block of TNT take nothing. Wired into the
item, mob, boat and cart pools; the player has no health yet. A box over two cactus cells is hit
twice in the tick, because the loop makes one call per cell.

The block table gained a **`Contact` column** for it, one row per class that overrides the method,
which is TickBehaviour's rule. Two classes, three blocks: the cactus, and both pressure plates --
and the plate's row is **dispatched by nothing**, because this port arms a plate from its own
sense pass in `core/tick/redstone.cpp`, and two paths for one effect would press it twice. New
code is `core/entity/block_contact.hpp`, a template over the world access like `fire_entry.hpp`.

**The shape.** `block::renderBoxes` answered the cactus with its collision box, so the slot icon,
the held cactus and the dropped one were a cube inset a sixteenth all round. Every face in this
project is textured over its own extent, so that mapped the side tile's *outer texel columns* --
which are transparent except for the spike pixels -- onto the narrowed face, and the block showed
daylight between its own edges. `bc.a(Lly;)V`'s render-type-13 branch does what `bc.b(ly,IIIFFF)`
does in the world instead: reset the bounds to the whole cell (`ly.e()`), draw the caps there, and
draw each side as a full-cell face under `Tessellator.addTranslation(±0.0625F)`, so those columns
hang past the cell and the alpha test cuts away the rest of them.

So `renderBoxes` now answers the cactus with three boxes of one cell and a **face mask per box** --
caps, x sides, z sides, two faces each -- and `mesh::addCactus`, `render/item_entity_mesh.cpp`,
`render/held_item.cpp` and `gui/item_icon.cpp` all read the mask. The four had three copies of the
construction between them and now share one. The mask defaults to every face, so no other shape
changed, and the dropped item's vertex budget counts the faces it will actually write rather than
six per box.

**The icon had to sort faces rather than boxes.** Three boxes in one cell have no useful order
between them: the painter's key was `minX - maxZ`, which put the cap box down first and then laid
a sixteenth-wide strip of the side tile over the front edge of the top. The key is now the face
centre's `x + y - z`, which is the cross product of the projection's two screen axes -- the view
direction written as one number. A fence's post and rails sort identically either way.

Covered by four new cases in `tests/item_entity_test.cpp` (a stack destroyed by a cactus it lands
on, one point a tick against five health, the resting-on-top cell, and a stack two cells away that
is untouched) and two in `tests/item_icon_test.cpp` (the three boxes and their masks, and an icon
whose top face survives whole). Suite **1324/1324**. **Not seen on hardware**, and the dropped and
held cactus in particular are geometry nobody has looked at on a console.

### 35. Dropped items do not stack on their own, and the version that claim was dated to was wrong

Step 10 added ground merging on request, transcribed from a later version and marked as a deviation.
It is taken out again: **two stacks lying in the same block stay two stacks**, which is what a1.1.2
does. The entity tick is `dx.e_()` with nothing added to it now, and the pool ceiling is the only
thing bounding a pile of drops.

**The version the deviation was dated to was wrong**, and that is the part worth keeping. Step 10
said "ground merging arrives with Beta 1.8's `combineItems`". Disassembled from the jars on this
machine, plus 1.2.5, 1.3.1, 1.3.2 and 1.4.7 fetched for the bisect:

| Version | `EntityItem` | Merge in `onUpdate`? |
|---|---|---|
| a1.1.2_01 | `dx` | no |
| a1.2.6 | `eo` | no |
| b1.2 | `fh` | no |
| b1.6.2 | `hj` | no |
| b1.8.1 | `ee` | **no** -- the whole of `w_()` is gravity, the water branch, `moveEntity`, drag, bounce, `age++`, 6000 |
| 1.1 | `fj` | no |
| 1.2.5 | `fq` | no |
| 1.3.1 | `ni` | **yes** -- `getEntitiesWithinAABB(EntityItem, boundingBox.expand(0.5, 0, 0.5))`, every tick, server side only |
| 1.3.2, 1.4.7 | `ni`, `px` | yes |
| 1.8.9 | `uz` | yes, and now behind `age % 25` |

So ground merging is **1.3.1**, not Beta 1.8 -- a year and a half later than the comment claimed --
and the `age % 25` throttle the port copied is later still. It is a *release* bound: the manifest
used carries no 2012 snapshots, so which snapshot between 1.2.5 and 1.3.1 added the method is not
settled here, only that 1.2.5 is without it and 1.3.1 has it.

The bisect was mechanical: `javap -p -c`
over every root class, find the one carrying both `6000` and `0.03999999910593033` (that is
`EntityItem`, and only `EntityLiving` shares the pair), then look for a method taking its own type.
That is `combineItems`. Two ways of missing it are ruled out: b1.8.1's `w_()` was read end to end,
and **every `EntityItem` up to and including 1.2.5 has zero `java.util.List` or `Iterator` call
sites in the whole class** -- so there is no scan of any shape in it, inlined or otherwise.

The code: `combine()`, `kItemMergeInterval` and `kItemMergeReach` are gone, and so is the scan in
`ItemEntitySystem::tick`. The sweep at the end of the tick stays -- a death still empties a stack
rather than removing it, because the walk holds a reference into the pool. Five merge cases in
`tests/item_entity_test.cpp` are replaced by three that pin the absence: two like stacks in one
block after 120 ticks (past every multiple of 25 the old scan woke on), eight of them still eight,
and an old stack whose clock is not reset by a fresh one landing on it. Suite 1324/1324.

### 36. Three shapes, not two: `setBlockBoundsForItemRender`, and the arrow that was eaten by its archer

Two reports from play, and neither was the thing it looked like.

**"The stone button has the wrong texture in the inventory and hand (normal stone block)."** The
texture was right -- `new hu(77, 1)` is the stone tile, because that is what a button is made of --
and the *shape* was a full cube. There are three shape questions in a1.1.2 and this port had two of
them:

| Question | Jar | Here |
|---|---|---|
| What do I walk into? | `addCollisionBoxesToList` | `block::collisionBoxes` |
| What does a ray hit, and what does the world draw? | `collisionRayTrace` / `getSelectedBoundingBoxFromPool` | `block::selectionBox` |
| What is drawn when it is **not in the world**? | `setBlockBoundsForItemRender`, called by `RenderBlocks.renderBlockAsItem` | `block::itemRenderBox` -- **new** |

`ly.e()V` is empty, so for 253 of 256 ids the third answer is the constructor's bounds and agrees
with `selectionBox(id, 0)`. Exactly two classes override it, and they are the two whose world bounds
are written only in `setBlockBoundsBasedOnState`:

| Block | `e()V` sets | `selectionBox(id, 0)` |
|---|---|---|
| `hu` -- stone button (77) | 0.3125, 0.375, 0.375 -> 0.6875, 0.625, 0.625 | the unit cube |
| `al` -- both pressure plates (70, 72) | 0.0, 0.375, 0.0 -> 1.0, 0.625, 1.0 | the wafer on the floor |

So a button in the hand, in a slot or lying on the ground was a stone cube, and a plate was a
sliver. `hu.a()I` is **20 and is not `getRenderType`** -- `ly.a()I` is `tickRate`, `ly.f()I` is
`getRenderType` and `hu` does not override it -- so a button really is render type 0 and really is
drawn by the standard renderer from its bounds. That is the trap this cost an hour to: the number
looked like the button's own render type in a later version.

Measured rather than transcribed: `tools/genref.java --collision` restores each block's constructor
bounds, calls `e()`, and emits `kItemRenderBoxes[256]` beside the selection sweep; the rest of
`tests/collision_box_vectors.hpp` regenerated byte for byte, which is also a check on the tool.
`gen_selection.py` folds the column into the shared shape list (45 -> 47 boxes) and `configure.py`
emits `kItemRenderIndex`. `block::itemRenderBoxes` is the item path's entry point and
`core/gui/item_icon.cpp`, `core/render/held_item.cpp` and `core/render/item_entity_mesh.cpp` ask it
instead of `renderBoxes`. The world mesher never went through `renderBoxes` for a cube and is
untouched.

**Placement was checked and is not broken**, which the same report doubted. `hu.a(Lcn;III)Z` is four
`isBlockNormalCube` calls on the horizontal neighbours, and a1.1.2's `ItemBlock.onItemUse` calls
`cn.a(IIIIZ)Z` -- `canBlockBePlacedAt` with **no side argument**; `canPlaceBlockOnSide` does not
exist in this version. `tick::canPlaceAt` already says exactly that, and a button goes onto any of
the four sides of a wall through `item::rightClick` end to end. A top or bottom face places nothing,
as in the jar. What differs: a button that lands at metadata 0 -- which a1.1.2 allows, since the
side is not checked -- is drawn here as a full cube, where the jar draws whichever bounds its
singleton was last left holding. That is the same leftover-state question `collision.cpp` already
answers deterministically, and it is left alone.

**"An arrow should hit paintings and cause them to pop off."** It already did, and
`tests/arrow_test.cpp` had a case for it. What it did not do was leave the bow: the self-grace --
`entity != shootingEntity || ticksInAir >= 5` -- was ported as a by-place proxy, "any candidate
standing over `(shooterX, shooterZ)`", which is only as good as the shooter's staying inside its own
box. `kFlightSpeed` is 0.6 a tick against a player half-width of 0.3, so **one tick of Creative
flight leaves the footprint**, the exclusion misses, and the arrow -- born inside the player, muzzle
0.16 back, targets grown by 0.3 -- strikes its own archer at distance zero and is spent. Every shot
fired while flying died on the tick it was loosed; on the ground, at 0.215 a tick, none did, which
is why it read as "arrows are broken in Creative" rather than as a shot going astray.

The exclusion now splits on `ArrowShooter`: the player is skipped **by identity**, because
`ArrowTargets::playerPresent` is a stable handle and the jar's comparison is a reference one; the
footprint proxy is kept for mobs under a skeleton's arrow, where there is no handle to use. A
consequence worth stating: a player's arrow can now strike a cart, a boat or a mob it was fired from
inside of, which is what the jar does. Suite 1330/1330; 3DSX built; no hardware run.

### 37. A sword that hit like a fist, and the hoe that was a column short

Two items that did nothing, and in both cases the missing piece was the same kind of thing: a
fact about the item that the generated table did not carry. Neither needed new engine
machinery; both needed `tools/genref.java --items` to be asked one more question.

**The sword.** `item::attackEntity` has passed a hard-coded 1 since §16, and use.hpp said so --
"there is no `damageVsEntity` column in the generated item table yet, so a sword hits like a
fist here". The column exists now. `di.a(Lkh;)I` is `getDamageVsEntity(Entity)` and **no
override in this version reads the entity**, which is the whole reason the number can be a
column at all: the generator calls it with null on every constructed item and writes the answer
down. Two classes override it and both answer from a field their constructor set:

- `bs` (ItemTool) -- `weaponDamage = material + kind`, where kind is 0 for a shovel, 1 for a
  pickaxe and 2 for an axe. A wooden shovel is 1, a diamond axe 6.
- `iu` (ItemSword) -- `weaponDamage = 4 + material * 2`. Wood 4, stone 6, iron 8, diamond 10.

**Gold is material 0, beside wood.** A golden sword hits for 4 and wears out in 32 uses, which a
table written from names would get wrong in both columns; this one was measured. The hoes
override neither method, so all five hit for 1 -- as does everything else, because `Item`'s own
body is `iconst_1; ireturn`.

The unknown row carries 1 as well, and that is not filler: `InventoryPlayer.getDamageVsEntity`
returns 1 when the slot holds nothing, so `def(held).damageVsEntity` answers for the bare hand
out of the same lookup and `attackEntity` needs no `held == 0` branch. The `if (i > 0)` guard is
kept because the jar has it. **Durability is still not spent** -- the jar follows the hit with
`stack.hitEntity(living)`, which is `damageItem(1)` on a sword and `(2)` on a tool, and nothing
in this build wears out yet.

**The hoe.** `fu.a(Lev;Ldm;Lcn;IIII)Z` is the entire class -- there is no other method on it --
and it was unreachable because the dispatch in `rightClick` routes on *the block an item
places*. A hoe places none: `ItemDef::places` is measured by using each item on each face and
reading the cell the face offsets into, and a hoe writes its farmland into the cell that was
**struck**. So all five hoes measure 0, correctly, and 0 also means "does nothing". That is the
same hole `spawns` was added for, and `tills` is the same answer -- `instanceof fu`, asked of
the jar by the generator.

Four things in the method are not what a reader would guess, and all four are visible in play:

- **The face parameter is never read.** A hoe tills from underneath and from the side, so hoeing
  the face of a dirt cliff turns that cell into farmland nothing can grow on.
- **`isSolid` guards grass and not dirt.** The condition is
  `(above.isSolid() || id != grass) && id != dirt`, so dirt under a stone slab still tills and
  grass under it does not. A torch or a flower overhead is not solid and stops neither.
- **The seed roll is paid on dirt too.** `world.rand.nextInt(8)` is drawn before the block is
  compared against grass, so tilling dirt spends a draw that can never yield anything. Folding
  it away would leave the world's random one step behind for every later random tick -- the
  same argument `dropBlockAsItem` makes for its own dead `nextFloat`.
- **The sound is `ItemBlock.onItemUse`'s row exactly** -- `stepSound.getStepSound()` at
  `(volume + 1) / 2` and `pitch * 0.8` -- so it is `audio::placeCue` and not a second
  transcription of the same arithmetic.

The seed itself is one `EntityItem` with `delayBeforeCanPickup = 10`, which is
`kBlockDropPickupDelay` and what the drop sink already passes. Its rise is a constant `1.2F`
rather than a third draw, so a winning roll costs two `nextFloat`s and not three, and the
offsets are computed **entirely in float** as the crop's seeds in `core/tick/drop.cpp` are.

The farmland block is found by its tick behaviour, like the sign and the door before it; grass
and dirt are named through the generated `mcver::Block` enum, as `behaviour.cpp`'s
`flowerGround` already names the same three.

Coverage: eight cases in `tests/use_test.cpp` -- all five hoes on both grounds, the refusal on
stone and sand, the solid-cover asymmetry between grass and dirt, the struck cell from all six
faces, the one-in-eight roll driven from both sides with the drop's height and scatter checked,
the dead draw on dirt verified by replaying the stream, the whole damage column against the
jar's numbers, and a boat taken down in one hit by a diamond sword where a fist needs five.
Host suite **1338/1338**; the 3DSX build passed. **No hardware run.**

### 38. The mob spawner: the first tile entity that ticks, and the mob that was in the file all along

`bd`, `bj` and `r` -- the tile entity, the block and the renderer -- in
`core/entity/mob_spawner.{hpp,cpp}` and `core/render/spawner_mesh.{hpp,cpp}`.

**This is the block spawner and not `az`/`k`.** The two share a word and nothing else: §27 is
the world sweeping a 9 x 9 of chunks for somewhere dark, and this is one cell counting down and
throwing four candidates at the ground around itself. The dungeon generator has placed block 52
at the centre of every mossy room since the worldgen work, and until now the block was scenery.

**The whole of `bd.b()`, in the order it runs.** Nothing here is invented; every literal is in
the bytecode.

- `d = c` first, then **`World.getClosestPlayer(x+.5, y+.5, z+.5, 16)`** -- and a null return is
  a `return`, so a spawner nobody is near does not turn, does not smoke and **does not count
  down**. Walking away from a dungeon freezes it exactly where it was.
- One `smoke` and one `flame` at a uniformly random point in the cell, every tick.
- `c += 1000F / (a + 200F)` -- **computed in float and widened afterwards**, which is why the
  constants are floats in the port -- then wrapped past 360 with `d` carried down with it. The
  renderer multiplies by ten, so the mob turns at `10000 / (delay + 200)` degrees a tick: about
  12.5 on a fresh 600-tick delay and **fifty on the tick before it fires**. The visible
  acceleration is the tell that a spawner is about to go off, and it falls out of the arithmetic
  rather than being animated.
- `a == -1` seeds `200 + nextInt(600)`; `a > 0` spends one and returns.
- Four attempts, and **all four run**: build the mob `EntityId` names, count that *same class*
  within the cell grown by (8, 4, 8) and give up at six with a fresh delay, scatter to
  `(x + (nextDouble()-nextDouble())*4, y + nextInt(3)-1, z + (nextDouble()-nextDouble())*4)` at a
  random heading, and ask `getCanSpawnHere`. A yes adds the mob, throws twenty smoke/flame pairs
  around the cell, gives the mob its own twenty `explode` puffs and **seeds a delay without
  stopping** -- so one firing can place up to four.

**Every draw is `World.rand`**, not a generator of the tile entity's own, so the scatter is
observable in everything else the tick does. The port takes it from `TickWorld::random()` for
that reason.

**Three things that are not there**, and each was checked rather than assumed: no
`requiredPlayerRange`, no `spawnCount`/`maxNearbyEntities`, and no `minSpawnDelay`/`maxSpawnDelay`.
Those are the 1.x spawner's NBT. There is also no way in a1.1.2 to change `EntityId`, so a
spawner a player places is `bd`'s constructor default `"Pig"` for ever -- which is what the
original does with one too.

**`getCanSpawnHere` is a virtual call, so a dungeon spawner respects light.** The port splits the
animal and monster halves for the reason `mob_spawn.hpp` argues at length: `ag.a()Z` reads nothing
the constructor drew and can be asked about a position, while `ma.a()Z` reads the size its own
constructor drew and cannot. So an animal is asked first, and a monster is built, asked, and taken
back out again -- the same shape `az` already uses, and the same two functions.

**The mob was in the world file all along.** `readMobSpawners` claims `MobSpawner` compounds out of
the column's *preserved* `TileEntities` as it joins the resident grid, through a new pair of
`WorldStreamer` column sinks (`cn.b(Lcu;)V` and `cn.c(Lcu;)V`). The tag is not rewritten: it goes
back out byte for byte, so the save stays 1:1 and nothing a chest holds is at risk. **Measured on
a copy of the real 660-chunk World1:** 13 resident spawners -- 5 Zombie, 6 Skeleton, 2 Spider,
which is `cg`'s `nextInt(4)` weighting with zombie drawn twice. Standing on one of them
(`--spawns <copy> 2000 at=219.5,65,218.5`, a real skeleton dungeon) it fired 5 times in 2,000
ticks, placed 7 skeletons, and was then stopped three times by the six-of-a-kind check.

**What is still open, and it is the write half.** A spawner *this* build creates -- a Creative
placement, or a dungeon this build generates -- is not written back into `TileEntities`, so it
reloads as `"Pig"`. Closing that is the tile-entity write path, which is one piece of work for
all four tile entities (a chest's contents included) and is tracked in `docs/todo-m3.md` step 5.
Doing half of it here would mean re-encoding a list this build cannot fully model, and losing a
chest's contents to gain a spawner's mob is not a trade worth making. The generator's own
`PopulationSideEffects` already carries the spawner's mob and is thrown away on the worker
(`chunk_generator.cpp`); that is where the write half starts.

**Two smaller things came out of it.**

- **`ge.z()` was transcribed wrong** and nothing had pinned it. `spawnExplosionParticle` draws its
  three Gaussians *first* and then subtracts each one, **times ten**, from the position it places
  the puff at -- so the cloud starts as a shell that collapses inwards. The port drew the
  positions first and dropped the throw-back entirely, which is the right number of particles in
  the wrong places. It is now `MobSystem::explosionPuff`, called by the death puff and by the
  spawner alike.
- **The renderer costs no draw call.** `r`'s nine GL calls compose to one `Placement`, and the
  miniatures append to the mob pass's own buffer: same sheet, same bind, same `C3D_DrawElements`.
  `placeSpawnerMob` composes the outer transform onto `placeMob`'s result rather than rewriting
  `RenderLiving`'s flip-and-lift, so the two cannot drift apart. Budget four models, 1,152
  vertices, 18 KB.

Two stated deviations in the renderer: there is no display-entity cache (a `Mob` is stack-sized
here and the jar's HashMap exists to avoid a reflective constructor), and a display slime is
always size 1 -- `ma`'s constructor draws `1 << nextInt(3)` from the entity's own
`java.util.Random`, so the jar's cached one is whatever the first draw gave it and there is no
seed to reproduce.

Coverage: `tests/mob_spawner_test.cpp` -- the constructor's defaults, the nine names round-tripped
through `ew`'s table, the sphere of sixteen from both sides, the spin rate and its wrap, the delay
running down and reseeding, a dark room filling and a lit one refusing every attempt without
seeding a delay, the crowd check counting only its own class, an unknown `EntityId` staying inert
while still smoking and turning, the animal/monster split, `blockAdded`/`blockRemoved` through the
tile-entity sinks, the `TileEntities` read with a chest stepped over in the middle of the list, and
the miniature staying inside its cage through a full turn. The Info page has a `cage` row for the
same reason the `spawn` row exists.

### 40. Create World is a screen, not two keyboards

Making a world used to be `askWorldName` followed immediately by `askSeed`: two system keyboards
opening back to back, no way to see the first answer again, no way to change anything else, and a
world on the card the moment the second one closed. `Screen::CreateWorld` replaced both, laid out
like World Settings because it is the same set of questions asked one moment earlier.

| row | notes |
|---|---|
| Name | the keyboard, opening on whatever the row already holds; the first free `World<n>` to begin with |
| Seed | the keyboard, **blank meaning Random** -- which the row now shows as `Random` rather than as a number nobody chose |
| Gamemode / Difficulty | the same two value rows World Settings has, sharing its cursors |
| Format | Packed or Folder. Packed is still the default and still right for the hardware; a player who is going to carry the save to a PC no longer has to make a world and convert it |
| Secret World | **Roll (1 in 4)**, Yes or No. Roll is a1.1.2's own flip and the default |
| Fix Ore Generation Bug / Fix Bedrock Hole Bug | the two Extra Settings switches, at the one moment they cost nothing |
| Create | the only row that writes anything |

**Three of these can only be asked here.** Gamemode and difficulty can be changed at any time; a
seed, a format and the two generation fixes are about ground that has not been made yet, and this
is the last moment at which that is true of the whole world. Putting the fixes on this screen is
the point of them: they cannot touch a chunk that already exists, and at Create there are none.

**Nothing is written until Create.** Every row edits `Menu::NewWorld`, so B leaves having made
nothing and a failed Create leaves the screen exactly as the player filled it in -- which is why
the name is sanitised and checked against the world list *before* `makeDirectories`, rather than
relying on the keyboard's filter having been the last thing to run.

Two smaller changes fell out of it. `askSeed` no longer rolls a seed for a blank box: it reports
`chosen = false` and `createWorld` rolls, because a keyboard that rolled its own could not tell
"random" from "the number I typed" and the row had to display a number the player never chose.
And both clock-seeded draws -- the seed and the SnowCovered flip -- now come off **one**
`JavaRandom`; two built from `clockSeed()` in the same call would be built from the same clock and
would agree with each other, which is not what rolling twice means.

The screen is 3DS-only and has not been seen on hardware. What it stands on is covered in core:
`tests/seed_text_test.cpp` (`a_blank_seed_means_roll_one` is exactly the contract `askSeed` now
relies on), `tests/world_list_test.cpp` (the name rules), `tests/packed_storage_test.cpp`
(`any_storage_opens_whichever_shape_the_folder_is_in`, which is the Format row's two answers), and
`tests/streamer_generate_test.cpp` (`the_generator_reads_the_worlds_own_extra_settings`, which is
what makes the two fixes mean anything once the world exists).

### 39. Extra Settings: one screen that is deliberately not a1.1.2

World Settings gained a seventh row, **Extra Settings**, below Delete and above Back. Its own
screen carries six rows, and every one of them either undoes a bug the original shipped or offers
a choice it never had. It is offered **outside a game only**, like Format, Copy and Delete: two of
its rows rewrite `level.dat`, which a running world holds, and two more want the world diorama,
which only the main menu has.

Everything on it belongs to one world, never to the console. Three of the six live in
`<world>/3dalpha.ini` (`core/settings/world_settings.hpp`) — the file a real Alpha client never
reads — and two are `level.dat` fields the original already has. That split is not arbitrary: a
key a1.1.2 never wrote must not travel back to a PC copy of the world, and a value a1.1.2 *does*
write is the value to change rather than a second copy of it beside the file.

| row | where it lives | what it does |
|---|---|---|
| Set Seed | `level.dat` `RandomSeed` | the seed **future** chunks generate from; what is on the card keeps its shape |
| Fix Ore Generation Bug | `3dalpha.ini` `fix_ore_generation` | floors WorldGenMinable's box instead of truncating it — see [worldgen-a1.1.2.md](worldgen-a1.1.2.md) |
| Secret World | `level.dat` `SnowCovered` | a1.1.2's one-in-four winter roll, after the fact |
| World Texture Pack | `3dalpha.ini` `texture_pack` | the pack this world is drawn with; **Default** follows the console |
| Move Panorama | `3dalpha.ini` `panorama_tile_x/z` | where the world list's diorama stands, a 128-block map tile at a time |
| Fix Bedrock Hole Bug | `3dalpha.ini` `fix_bedrock_hole` | bedrock at `y = 0` whatever the draw said |

**Neither generation fix changes a random draw.** Each widens what the already-drawn shape is
written into, so a world with one turned on stays in phase with a1.1.2 through every later pass —
asserted directly in `tests/ore_test.cpp` and `tests/terrain_test.cpp`. `worldgen::GeneratorOptions`
carries both, and `WorldStreamer::open` fills them by reading the world's own settings file rather
than being handed them, so the console's menu, the host harness and a test cannot disagree about
what a world generates.

**Two bugs found by playing it, both fixed and both worth recording.**

*The Secret World row read `No` on every world, winter worlds included.* It asked
`AnyStorage::peekLevel`, and a packed world answers a peek out of the manifest's 64-byte metadata
block -- which carries `lastPlayed` and `randomSeed` and nothing else, deliberately, because the
world list peeks every world on the card and the alternative is inflating a level blob per row.
Every other field came back at its `LevelData()` default, and `snowCovered`'s default is false.
Every world this port makes is packed, so the row was reading a default and calling it an answer.
`AnyStorage::readLevel` is the new call: the manifest, the level blob, inflate, decode, and still
no lock and no `lastPlayed` written. The folder backend's peek already read the whole file, which
is exactly why nothing noticed until a packed world was looked at.
`tests/packed_storage_test.cpp` pins the distinction from both sides.

*A winter world thawed and refroze forever.* Not the screen's bug -- it was in
`tick/behaviour.cpp` and predates it -- but the screen is what made it reachable.
`iceTick` and `snowTick` read `skyLightAt`; `he.a`, `p.a` and `fd.a` all read
`cn.a(by.b, ...)`, and `by.b` is `EnumSkyBlock.Block`. Sky light is 15 on anything the sky can
see, which is above all three thresholds, so every exposed block of ice and snow melted on its
first random tick -- and `TickWorld::snowAndIce`, which does read block light and does find it
dark, put it straight back. Block light is 0 outdoors whatever the hour, so in a1.1.2 nothing
melts until a player brings a light source near it. Derivation and the melt thresholds are in
[tick-a1.1.2.md](tick-a1.1.2.md) *Ice and snow read block light*.

**Secret World is the one row that is genuinely retroactive, and it is retroactive for free.**
`TickWorld::snowAndIce` is `cn.h()` — one position per chunk per tick, on a one-in-four roll —
and it reads the live `SnowCovered` flag. Setting it spreads ice and snow over ground that is
already there as the world is ticked. It does not take them away again, and the tooltip says so.
Set Seed is the opposite and its tooltip says the opposite: chunks on the card keep the shape they
were generated with, so the old world and the new one will not line up at the seam.

**Move Panorama uses the world diorama itself as the editor.** `preview::dioramaOrigin` gained a
tile offset — zero is where the table has always stood, so every world that has never been moved
reads as the original behaviour — and `MenuPreview::runWorldJob` reads the offset off the world's
`3dalpha.ini` on the worker thread, where a card read is allowed. The screen is
`PreviewScreen::Worlds` pointed at the same world, so what the player is aiming *is* the picture
the world list will show. A d-pad step writes the file and calls `MenuPreview::forgetWorld`, which
quiesces the worker, drops the grid and its tile meshes, and lets them stream in again from the new
corner; the file is the only channel between the screen and the worker, which is why the step is
saved immediately and why B puts it back the same way.

**A world's texture pack does not overwrite the console's.** `applyWorldPack` builds the named
pack into the live atlas at the moment a world is launched and sets `packOverridden_`;
`packName_` and `3ds.ini` are untouched, so `ensureAtlas` rebuilds the console's own on the next
visit to the menu. The world's choice spells Dev Art as a token (`kWorldPackDevArt`) rather than
as the empty string, because empty already means the third answer the console's setting does not
need: follow whatever the console is set to.

Coverage: `tests/world_settings_test.cpp` (round trip, an older build's file with none of the
keys, the spellings a person types by hand, Default against Dev Art),
`tests/packed_storage_test.cpp` (a packed peek is the header only, `readLevel` is the whole
level, and neither claims the world), `tests/tick_test.cpp` (ice and snow under an open sky never
melt; both snows melt above block light 11), `tests/ore_test.cpp`
(the measured quadrant loss, and that the fix is a no-op at positive coordinates),
`tests/terrain_test.cpp` (the hole rate, that the fix closes it, and that nothing above `y = 0`
moves), `tests/diorama_test.cpp` (the offset steps by whole map tiles and stays on a chunk
corner), `tests/streamer_generate_test.cpp` (the streamer reads the world's own file). The
screen itself is 3DS-only and has not been seen on hardware.

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


### 39. `TileEntities`: the list stops being a blob, and all four tenants land at once

`core/world/tile_entity.{hpp,cpp}` is `ic` and its four subclasses, and
`src/impl/storage/alpha_chunkfiles/chunk_nbt.cpp` is the codec. §38 could read a spawner's mob
out of a real world and could not write one back; this is the other half, and it is the reason
sign text now survives a reload too.

**It had to be all four or none.** A build that wrote `TileEntities` without modelling the chest
would empty every dungeon chest in the world the first time a column was saved, so the question
was never "can we persist a spawner" -- it was "can we re-encode the list", and that meant the
chest's 27 stacks and the furnace's three as well. `ic`'s static initialiser is the whole
registry and it has exactly four entries:

    a(ke.class, "Furnace");  a(fe.class, "Chest");  a(ob.class, "Sign");  a(bd.class, "MobSpawner");

so `TileEntityKind` is those four in that order, plus `Unknown` for anything else.

| id | class | tags beyond `id`/`x`/`y`/`z` |
| --- | --- | --- |
| `Furnace` | `ke` | `Items` (3 slots), `BurnTime` S, `CookTime` S |
| `Chest` | `fe` | `Items` (27 slots) |
| `Sign` | `ob` | `Text1`..`Text4`, truncated to 15 on read |
| `MobSpawner` | `bd` | `EntityId` String, `Delay` Short |

**Five things the jar settled that guesswork would have got wrong.**

- **`id` is not guaranteed to come first.** `hm` is an `NBTTagCompound` backed by a `HashMap` and
  writes its members in bucket order, so `Delay` really can precede the `id` that says what class
  a `Delay` belongs to -- and claiming a tag against the wrong class would both misread it and
  emit it twice on the way out, once from the field and once from `preserved`. So each element is
  read twice over the same bytes: `skipValue` first, then one pass for the id and one against it.
  Both passes run on memory already resident and a column holds a handful of these.
- **The chest is 36 slots long and 27 slots wide.** `fe`'s constructor allocates `new ev[36]`
  while `getSizeInventory` answers 27, and `fe.a(hm)` reallocates the array to `c()` -- so a chest
  is 36 until it has been round-tripped once and 27 afterwards. Nothing can reach slots 27..35,
  because every writer goes through the `gh` interface and that asks `c()`. `kChestSlots` is 27
  and `kChestSlotsAllocated` records the other number so nobody has to re-derive that it does not
  matter.
- **`ke.c` is not saved.** The furnace writes `BurnTime` and `CookTime` and recomputes
  `currentItemBurnTime` from the fuel slot on read, so there is nothing to keep.
- **An element with no `id`, or no position, is dropped -- and that is the original's behaviour.**
  `ic.c(hm)` looks the id up, finds nothing, prints `Skipping TileEntity with id null` and returns
  null; the chunk loader never puts a null in the map, so it is never written back either. The one
  lossy case, lossy in the same place the original is.
- **`ga.d(III)Lic;` heals lazily, and we cannot.** Ask the original's chunk for the tile entity at
  a position and, if the map has none but `ly.q[]` says the block is a container, it calls `jt.e`
  on the spot to build one. That works there because every access goes through that one method.
  Here the list is read at a save, so the rule is applied once over the whole column --
  `reconcileTileEntities`: a container block with no entry (or an entry of the wrong kind) gains a
  default one, a *known* entry whose block is gone is dropped. **Unknown entries are never
  touched**, because this build cannot tell which block a modded tile entity belongs to and
  guessing would delete it.

**What that buys, in the order a player would meet it.**

- **A sign keeps its text.** `readSigns` fills `SignStore` as a column arrives and `writeSigns`
  puts it back just before the column is written. The board's shape is not in the NBT -- `ob`
  stores four strings and nothing else -- so post-or-wall and the metadata are read off the
  column, which is the only place that has all three at once.
- **A spawner keeps its mob.** `writeMobSpawners` is the mirror of §38's reader, and
  `readMobSpawners` now walks the decoded list instead of parsing the preserved tag a second time.
- **A dungeon this build generates keeps its chest loot.** `PopulationSideEffects` was being
  passed as null on the generation worker with a comment saying the records had no consumer; they
  have one now. They go onto the generator's `Entry` rather than onto a column, because a pass at
  `(px, pz)` writes into a 2x2 quadrant and a dungeon rolled for one chunk routinely lands in the
  next -- the record has to follow the blocks. `finish` hands the list to the column it builds.
- **A chest or furnace placed by hand round-trips**, without either being openable. Nothing in
  this build puts an item into one; the empty entry is what a real client would find.

**Two new seams, and one of them is the one that was missing.**

- `WorldStreamer::setColumnSinks` gained a third callback, `saving`, which fires on a resident
  column immediately before it is queued for the card -- the autosave flush and the drop path
  both. It runs **before** `dropped`, which is a reordering: `dropCell` used to fire `dropped`
  first "so a sink that wants to write something back into the column still can", and with a sink
  that actually writes, dropping first would erase the entries the save is about to read.
- `WorldStreamer::markColumnModified` is **`ga.f()`**, setChunkModified, reached from `ic.j_()`.
  `nv` (GuiEditSign) calls it as the keyboard closes and `fe.a(int, ev)` on every stack put into a
  chest. Text is not a block, so nothing else marks the column -- without it the first line typed
  onto a sign already in the world would never reach the card.

`Section::mayHoldTileEntity` is `mayTickRandomly`'s shape for the save path: a palette-only
question, so fifteen sections of stone and air are rejected without touching the index array.
`block::tileEntityBearing` is `ly.q[]`, and block 54 gained a `TickBehaviour::Chest` row purely so
that column can answer -- `b` has no `onBlockAdded` of its own, and the spill in its
`onBlockRemoval` is still not ported.

**Measured on a copy of the real World1**: 1,119 chunks read and written, 66 tile entities (39
Chest, 27 MobSpawner), 195 item stacks, and `tools/nbtdiff.py difftree` against the untouched copy
reports **1,119/1,120 files semantically identical** -- the one difference being `level.dat`'s
`LastPlayed`, which is a clock. `--rewrite <world-dir>` in the host harness is that check: load
every chunk, write it straight back, change nothing on purpose, so every difference the diff
reports afterwards is a bug in the codec. It exists because the modelled tags are no longer only
the ones nothing cares about.

**And the reconcile pass finds nothing to do on a real world**, which is the other half of the
same measurement: `--rewrite <copy> reconcile` runs the heal-and-drop over all 1,119 columns and
reports **0** entries added or dropped, with the diff still clean. That is what says
`tileEntityBearing` agrees with `ly.q[]` and that the chunk-relative arithmetic is right -- a pass
that invented a chest or dropped a spawner would show up as a non-zero count long before anybody
opened the world in a real client.

**The packed format got it for free**, which is the point of `AlphaChunkCodec`: pack a copy of the
real world, `--rewrite <copy> reconcile` it through the packed backend (1,119 chunks, the same 66
tile entities, 0 reconciled), unpack it, and the diff against the untouched original is still the
`LastPlayed` line alone.

**And a world this build generated keeps its dungeons.** 510 columns walked out under the
sanitizers: 28 tile entities on disk -- 17 Chests holding 89 item stacks, and 11 MobSpawners
reading **5 Zombie / 4 Skeleton / 2 Spider**, which is `cg.b`'s `nextInt(4)` weighting and not one
`"Pig"`. Before this, every one of those cages reloaded as `bd`'s default and every one of those
chests was empty.

`tests/tile_entity_test.cpp` is new (16 cases) and its NBT is hand-written rather than produced
by `encodeChunk` -- a round trip through our own encoder and decoder would pass just as happily if
both were wrong in the same way. It includes the HashMap tag order above, an unknown `id` keeping
every tag it arrived with, a later version's `MaxNearbyEntities` on a known kind being written
exactly once, out-of-range slots on both array sizes, and negative chunk coordinates.

### 40. The map was a photograph: a chunk was sampled once, ever

Reported from play: the bottom screen's map does not change when you build, and does not change
when blocks decay. It was exact. `MapScreen::update` took a chunk the first time the streamer had
it resident and then skipped it for the rest of the session —

```cpp
if (store_.find(chunkX, chunkZ) != nullptr) { ...touch...; continue; }
```

— so everything a world does after the player first walks past it was invisible until the world was
reopened. A house, a drained lake, a burnt forest, leaves decaying, sand falling, a fluid spreading:
all of them on the wrong side of that `continue`.

**The gap was not the sampling, it was that nothing told the map anything had happened.** The
renderer is told: `WorldStreamer::tickBlockChanged` invalidates the sections around a changed block.
The saver is told: the same function sets `Cell::tickDirty`. Neither is any use to a map. The
renderer's path is bracketed by `tickRenderer_`, which only `stepTicks` and the player-edit entries
set, and the saver's flag is a flag — one owner, cleared when that owner has acted on it. A map is a
second reader with a different clock, and it holds samples of chunks the streamer no longer has.

#### A serial off one session-wide counter, read whenever the map next looks

`Cell::mapSerial` takes a fresh value from `WorldStreamer::blockSerial_` in two places:
`tickBlockChanged`, unconditionally — a decaying leaf is a block change whether or not anybody is
holding a renderer — and `adoptColumn`. `columnBlockSerial(cx, cz)` hands it back, or 0 for a
column that is not resident, which no live column's serial ever is.

**The adoption stamp is doing as much work as the bumps.** A per-column change *count* would have
restarted at zero when a cell was reused for other ground, so a chunk the player edited, walked
away from and came back to would compare equal to the sample taken before the edit — the map would
go on drawing a house that had since been demolished. Every value comes from one counter, so a
serial is never handed out twice and "same serial" can only mean "same column, untouched".

The whole-world figure, `blockChangeSerial()`, is what lets the map skip the per-chunk pass
entirely. It also wraps at 2^32, which is named in the header rather than guarded: the consequence
is one chunk drawn one edit out of date once every four billion block changes, against eight bytes
a cell to make it impossible.

#### Two costs the obvious fix would have added, and what stopped them

**A re-shade of the whole window on every frame a player is mining.** `MapStore::store` now compares
the new sample against the held one and returns whether the picture actually moved; an identical
sample keeps the patch, the south neighbour's patch and `stats().stored` — which is exactly what
`MapScreen`'s redraw signature hangs off. Tunnelling under a roof changes no surface block, so it
now costs the 256-column scan and nothing else. The comparison is 768 bytes against a window
re-shade of 88–400 µs on the host; it is the cheaper half by three orders of magnitude.

**A second hash lookup per visible chunk, every frame, for ever.** §0r already established that one
pass over the window's chunks is a fifth to a quarter of a redraw — 23.8 µs at 1:1 and 89.1 µs
shrunk, on the host — which is why `Refreshed` exists. The staleness pass is gated the same way: it
runs only when the window moved, when `blockChangeSerial()` moved, or when the previous pass ran out
of sampling budget on a chunk it knew was stale (`staleLeft_`, which is what stops a stale chunk
being stranded on a frame where nothing else happens). Standing still in a quiet world costs what it
did before.

Two limits, stated rather than hidden. A stale chunk **outside** the window is not re-read until the
window moves over it — the map remembers ground 360 to 570 blocks square and re-reading all of it on
every edit is not a trade worth making. And a stale chunk whose column is **no longer resident**
keeps the sample it has: it is the last true thing anyone could know about that ground, and the map
is not allowed to ask the card for it.

#### What is pinned

`a_resample_that_changes_nothing_costs_nothing_but_still_takes_the_serial` (`tests/map_test.cpp`) —
the identical re-sample keeps the patch and does not advance `stored`, the serial is taken anyway,
and a sample that does differ drops both its own patch and the one to the south.

`a_block_change_moves_the_columns_serial_and_a_reload_does_not_reuse_one`
(`tests/streamer_revisit_test.cpp`) — a raw `setBlockWithNotify` with no renderer in hand moves the
serial, so does a player's `setBlock`, a frame where nothing is written moves nothing, an untouched
neighbour keeps the value it was adopted with, and a column driven out of the grid and brought back
never returns with a serial it has already used.

**Not measured on hardware.** The re-sample path costs one chunk's 256-column scan out of the same
per-frame budget a new chunk comes from — ~1.3 µs on the host, so roughly 50 µs on an ARM11 — and
the gating above is argued from §0r's numbers rather than from a console run. The figure to take,
when there is hardware, is `lastDrawMicros` on the Info page while standing in a fluid.

**Superseded in part by §41.** The `staleLeft_` flag and the per-chunk pass this section describes
are gone: the streamer hands over a list of what changed instead of the map scanning for it, and the
budget is a slice of the frame rather than a count of chunks. The serial, the adoption stamp and the
`memcmp` in `store` are unchanged and are what §41 is built on.

### 41. The map may not make the game wait, and the spare core may help it

Reported from play, straight after §40 landed: *"Never make the game wait for a map update, the map
updates should just be queued up and then done when there's time for them. Also make sure the same
place won't get marked 'to be redrawn' multiple times. Also if the extra New 3DS core has time over
cuz it's not generating it should help offloading and generating the map, generation has priority
tho."* Three separate rules, and §40's design broke all three in the same place.

#### What §40 actually cost, and why a fluid was the case that settled it

`blockChangeSerial()` answers "did anything change?" in one compare, and behind that gate was a pass
over every chunk in the window: a store lookup and a streamer lookup each, 182 of them at 1:1 and
650 shrunk. That is fine for a block placed by hand — the gate is shut again on the next frame. It
is not fine for anything the world does on its own. **A lake draining moves the serial on every
tick for a minute**, so the gate stands open for that whole minute and the expensive pass runs on
every frame of it, to find the two chunks that actually moved. Fire, leaf decay and falling sand are
the same shape. Behind that, the sampling itself was a count — sixteen chunks a frame on a New 3DS —
which is a number of chunks and not a number of microseconds, so the frame's exposure was whatever a
sample happened to cost.

#### One list, filled where the writes already funnel

`ChunkQueue` (`core/util/chunk_queue.{hpp,cpp}`) is a fixed-capacity FIFO of chunk coordinates that
holds each coordinate **once**. `WorldStreamer::tickBlockChanged` — the choke point every block
write in the game goes through, tick-driven and player alike — pushes the chunk onto one, and so
does `adoptColumn`, so new ground and changed ground reach the map by the same road.
`takeChangedColumn` pops. The map's per-frame cost is now a pop per *changed chunk* rather than two
lookups per *visible chunk*: the draining lake is two pops.

The dedupe is the load-bearing part, and it is not tidiness. A fluid rewrites hundreds of blocks in
one chunk in one tick and every one of them arrives at `tickBlockChanged`; without the dedupe the
map would sample that chunk hundreds of times for one picture, and the frame's whole allowance would
go on it. The second offer of a coordinate is answered in a probe and a compare.

**The shape of it.** A ring whose slots never move — an entry is written into a fixed ring position
and stays there until it is popped — so the membership table can hold ring positions rather than
coordinates and nothing in it goes stale while the queue is walked. Removal leaves a tombstone,
because deleting from a linear-probe run means shifting the run back and that is the easy thing to
get subtly wrong; the table is four times the capacity so tombstones may reach the live count before
a rehash, and a rehash is one pass over the table against a queue drained once a frame. Nothing
allocates after `setCapacity`. A push onto a full queue is refused and sets `overflowed()`, which
the consumer reads once and recovers from once — for the map that means one pass over the window
comparing serials, which is exactly §40's old pass kept as the fallback it should always have been.

#### A slice of the frame, not a number of chunks

`MapScreen::update` reads `svcGetSystemTick` on the way in and stops sampling when its allowance is
gone, in the middle of the queue, and picks it up next frame. **400 µs on an old 3DS, 800 on a New
one** — the same work the old counts of 8 and 16 allowed at the estimated ~50 µs a sample, said in
the unit the rule is actually about, so that a sample which turns out to cost more on hardware costs
the frame no more than it is allowed to. The cold-start burst is 3.2 ms, ended the first frame that
samples nothing.

The clock is read **after** each chunk rather than before, so a frame always samples at least one
however little is left. A world with a thousand chunks to re-sample therefore costs exactly what a
world with three does; it simply takes longer to catch up, and "the queue is long" is a recoverable
state where "the frame is long" is not.

The window is still walked, but only when it moves, and only to find ground the map has never had —
the change list says everything else. That walk is the same nearest-first ring scan §0r put in, and
because the queue is FIFO, nearest-first survives all the way to the sampling without the queue
knowing anything about where the player is.

#### Core 2, and generation still first

`WorldStreamer::offerColumnWork` hands the generation worker up to eight resident columns and a
function pointer to run over them. On a New 3DS that worker owns core 2, and a console standing
still in a world that is already made leaves it idle; a chunk sample needs nothing but the column,
so it is exactly the work that can go there. On an old 3DS the worker is on core 0 at the bottom
priority and this runs in the slack the main thread leaves at VBlank, which is free for the same
reason the I/O thread's reads are.

**Generation keeps priority, and the worker enforces it rather than the caller.** `workerMain` looks
at the slate before it looks at the offer, and `runColumnWorkLocked` drops and retakes the lock
between columns so that a column coming onto the slate cuts the batch short there and then. An offer
is never a delay to the world being made; it is work done in the gaps between it.

**What makes the borrow safe is when it may be in flight, not a lock.** The worker is handed raw
`const ChunkColumn*`s, resolved on the main thread where the grid is settled — the worker never
looks a coordinate up, and a coordinate that is not resident is dropped by the offer and the rest
compacted to the front. The offer is made at the end of one frame's `tickMap` and withdrawn at the
top of the next frame's `WorldStreamer::update`, before anything there can drop, adopt or mesh a
cell: `reclaimColumnWork` clears the posted flag, which cancels a batch the worker never started and
cuts short one it did, then waits out at most the single column it is inside. Nothing writes a block
in that interval — on the console the player's edits and `stepTicks` both run earlier in the frame
than the map does — so it is not a data race either. A wait is the length of one chunk's work and
only happens if a frame was missed entirely.

The samples land in `MapScreen`'s own buffer and not in the store: `MapStore::store` moves an LRU
cursor, invalidates the south neighbour's patch and may evict, none of which another thread may do
behind the screen's back. The worker writes 1 KB into a slot of its own; the main thread puts it
away on the next frame.

#### What is pinned

`tests/chunk_queue_test.cpp` — a coordinate offered twice is on the queue once and refusing it is
not overflow; FIFO order, so a nearest-first producer stays nearest-first; a full queue refuses and
says so once, and emptying it does not clear that flag; and the drain-and-refill churn (4,000
push/pop pairs against a capacity of 32) that finds a membership table filling up with tombstones,
checking after every round that every live coordinate is found and nothing else is.

`tests/streamer_map_work_test.cpp` — two hundred blocks written into one chunk put it on the change
list once, taking it off is what lets the next write put it back, and a player edit goes on the same
list a tick's write does; an offered batch runs on the worker with the right indices and the right
columns, cannot be offered over, and has no answer until the streamer has withdrawn it; a
coordinate whose column is not resident is dropped by the offer and the survivors compacted; and
offers made on every frame while the frontier is still filling in never stop the world settling.

**Not measured on hardware.** The allowances above are converted from the host's ~1.3 µs sample at
the 30–40× the dev host has been measured at elsewhere; the claim that the change list removes a
per-frame window pass is structural rather than timed. The figure to take, when there is hardware,
is `lastDrawMicros` on the Info page — once standing next to a spreading fluid, once standing in a
quiet world — and the frame time beside it.


### 42. Farmland ignored being walked on: `onEntityWalking`, and an override that was missed

Reported as "farmland doesn't care if you run/jump over it", and the first half of the answer is
that in a1.1.2 *running and jumping are not what ruins a field* -- walking is, and nothing did.

**The jar.** `kh.c(DDD)V`'s footstep block ends with a line this port had named and skipped:

```java
Block.blocksList[l].onEntityWalking(worldObj, i, j, k, this);   // same i, j, k as the step sound
```

It sits inside the same `if (onGround && !sneakFlag)` and the same
`distanceWalkedModified > nextStepDistance && l > 0`, so it is **one call per footstep**, which is
one per whole block of ground covered -- a sprint earns them no faster, and a jump that goes
nowhere earns none. Disassembling all 402 classes for overrides of `a(Lcn;IIILkh;)V` gives four
hits: `ly` (empty), `km` (the staircase, which forwards to the block it is modelled on -- planks
and cobblestone, both empty), `ai` (redstone ore, `glow()`), and **`mi`, farmland**:

```java
if (world.rand.nextInt(4) == 0) {
    world.setBlockWithNotify(i, j, k, Block.dirt.blockID);
}
```

That is the whole override. **No fall-distance test, no crop test, no entity test** -- the version
where a jump ruins a field and a walk never does is Beta's. The earlier note in
[audio-a1.1.2.md](audio-a1.1.2.md) that redstone ore is the *only* a1.1.2 override was short by
this one, and is corrected there.

**Where it lives here.** `tick::entityWalkedOnBlock`, beside `tick::entityCollidedWithBlocks` and
called the same way and for the same reason: `PlayerBody::move` takes a `const TickWorld&` so that
moving a body cannot write blocks, so the mover records the cell (`steppedOn`,
`stepBlock{X,Y,Z}` -- the block *underfoot*, unaffected by the snow and liquid substitutions that
decide `stepSoundDue`) and the caller that owns the tick runs the callback immediately after the
move and before the collision scan, which is the jar's order. The dispatch is on the tick
behaviour, like `entityCollidedWithBlock`'s, so no new block column was needed: farmland and both
ores already carry one. Redstone ore reuses `redstoneOreActivated`, including its "only the unlit
one" test -- so **walking over ore now lights it**, which was the line that had been named as
missing.

Both callers: the player in `src/platform/ctr/main.cpp`, and every mob in `entity::MobPool::tick`,
because `moveEntity` is `Entity`'s and a cow trampling a field is the same code path as a player
doing it. The host soak tool is unchanged, as it is for the collision scan.

**The crop goes with the ground, and the second report was about the screen.** "Doesn't destroy
the crop on it instantly": `setBlockWithNotify` notifies the cell above and a crop's
`onNeighborBlockChange` is `mq.h` -- checkFlowerChange -- so the wheat drops and the cell becomes
air inside the same call, with no tick of grace. That part was already right, and the test that
pins it passes on the world data. What was wrong was that **nobody redrew it**.

`TickWorld`'s change callback only invalidates renderer sections while the streamer is holding a
renderer, and `stepTicks` and the four player-edit entry points were the only things that ever
held one -- the trap `WorldStreamer::setBlock` has documented since it was written, reached
through a door nobody had noticed. The entity pools are not player edits and do not run inside
`stepTicks`: they run straight out of the frame loop with `worldTick()` in hand. So the trample
landed in the world, the crop was removed from the world, and the section went on drawing both
until something unrelated touched it. The symptom is exactly what was reported -- wheat standing
on dirt.

**`WorldStreamer::RenderBracket`** is the bracket on its own, scoped: the constructor takes the
renderer, the destructor drains the light queue and restores the previous holder (so nesting is
safe, which the four entry points now rely on -- they were clearing the pointer rather than
restoring it, and all five are routed through it now). `main.cpp` holds one around the body loop
and one around the whole entity-pool block, so every write `moveEntity`'s tail makes is drawn:
the trample, the crop going with it, a dropped stack pressing a wooden plate, a falling block
landing. Those last two were invisible for the same reason and are fixed by the same scope.

`tests/streamer_revisit_test.cpp` --
`a_block_an_entity_writes_marks_its_section_only_inside_a_render_bracket`: a footstep's trample
outside a bracket changes the world (dirt, and the wheat gone) and leaves the section clean;
inside one it marks it. The same two halves as the player-edit test above, through the entities'
door.

**Named deviation: only the player and the mobs pay it.** `distanceWalkedModified` and
`nextStepDistance` are `kh`'s, so in the jar a dropped stack, a boat or a minecart sliding along a
field earns footsteps and trample it too. Here the walk counters live on `PlayerBody`, which is
what the player and every mob move with, and the other pools have their own sweep with no such
counter -- so an item pushed across farmland by a current leaves it alone. Adding it means giving
those pools the counter, not a second copy of this dispatch.

`tests/trample_test.cpp` -- a walk across a field ruins some of it and never more cells than it
earned footsteps; sneaking earns no footstep and so leaves it alone; jumping on the spot never
tramples, which is the line between this version and Beta's; 4000 direct rolls land between a
fifth and a third, around the quarter; and a footstep on redstone ore lights it.
The crop is checked twice: it stands through every roll the trample does not take and is gone in
the same call as the one that does, and a walk across a planted field leaves nothing floating.

---

### 43. The sky: two flat planes, 780 stars, and a fog colour that was a constant

There was no sky. The top screen was cleared to one daylit blue (`kSkyColour = 0x90D9FFFF`) and
the terrain faded into a second, unrelated one (`kFogColour`, 144/217/255), both written in
`renderer.cpp` and neither a function of the time of day -- so midnight was a bright blue afternoon
with dark ground under it, and there was no sun, moon or star anywhere in the build.

**The colours are `cn`'s and there are three of them.** `getSkyColor` (`cn.b(F)Laj;`),
`getFogColor` (`cn.e(F)`) and `getCloudColour` (`cn.d(F)`) are the same function of the day over
three different base colours, held as `long` fields set in the constructor and **never written
again**:

| field | value | as hex | what it is |
|---|--:|---|---|
| `cn.D` | 8961023 | `0x88BBFF` | the sky |
| `cn.E` | 12638463 | `0xC0D8FF` | the fog |
| `cn.F` | 16777215 | `0xFFFFFF` | the clouds |

a1.1.2 has **no biome tint in the sky at all** -- the per-biome colour and the temperature lookup
are later versions', and this class file reads one world field. A sky that changed with the ground
under it would be invention, so this port's does not.

The day's fraction is `clamp(MathHelper.cos(celestialAngle * PI * 2) * 2 + 0.5, 0, 1)`, which is
**flat at 1 for most of the day** (the cosine only has to reach 0.25) and falls to nothing over the
same eighty seconds the block light steps down in. The sky is that fraction times its base, so it
reaches **pure black**; the fog gets a floor instead -- `f * 0.94 + 0.06` on red and green,
`f * 0.91 + 0.09` on blue -- which is why a midnight horizon is dark slate and not nothing.

**What `glClearColor` and `GL_FOG_COLOR` actually get is neither.** `iq.h(F)V`, updateFogColor,
lerps the fog colour **towards the sky** by `1 - pow(1 / (4 - renderDistance), 0.25)`: 0.293 at
Far, 0.240 at Normal, 0.159 at Short and exactly 0 at Tiny. The same three floats go to both calls,
and that is what makes the horizon seamless -- whatever the far plane clips is already fully fogged
where it is cut, and the screen behind it is that colour.

**Named deviation:** the original's `renderDistance` is one of four settings; this port's is a
chunk count that the debug page can put anywhere. The setting is recovered as `4 - log2(chunks)`,
clamped -- 16, 8, 4 and 2 chunks land exactly on Far, Normal, Short and Tiny, and six chunks lands
between Normal and Short where it belongs.

**The geometry, from `e.class`.** renderSky translates by nothing at all (its one `glTranslatef` is
a literal (0, 0, 0)), so the whole sky is camera-relative and follows the player:

- a **flat plane sixteen blocks up**, drawn in the sky colour **with fog on**. It is a 13 x 13 grid
  of 64-block cells from -384 to +448, and the cells exist for the fog: GL computes it per vertex,
  so one enormous quad would fade across its diagonal instead of radially. *The distance fade is
  the horizon gradient* -- there is no dome and nothing is baked.
- a **second plane sixteen blocks down**, same grid wound the other way, in
  `(r * 0.2 + 0.04, g * 0.2 + 0.04, b * 0.6 + 0.1)` of the sky -- a fifth of it, bluer, with a
  floor. That is the renderer's line, not the world's; `cn` never computes it.
- the **sun**, 30 blocks half-width at y = +100, and the **moon**, 20 at y = -100, both textured,
  both **added** (`glBlendFunc(GL_ONE, GL_ONE)`) with fog and the alpha test off. The moon's UVs
  are the sun's reversed in both axes, which is in the bytecode and not a transcription slip.
- **780 stars**, from `e.f()V`: 1500 candidates out of `new Random(10842L)`, each three floats and
  a size, kept when `d < 1 && d > 0.01`, pushed to radius 100 and given a billboard rolled by a
  random angle. The rejected candidates still draw their four floats, so the stream cannot be
  short-circuited. Drawn at `getStarBrightness` (`cn.f(F)`), which is
  `clamp(1 - (cos * 2 + 0.75))^2 * 0.5` -- **0.75 where the colours use 0.5**, so the stars appear
  well after the sky has begun to dim and never exceed half brightness.

**Only the celestial half turns.** `glRotatef(celestialAngle * 360, 1, 0, 0)` is pushed before the
sun and popped after the stars. There is **no Y rotation** in this version -- the `glRotatef(0, 0,
0, 1)` before it is a literal zero -- so a1.1.2's sun rises and sets along the **Z** axis rather
than east to west. That is the version's, not a bug here.

**The fog for the sky pass is not the world's either.** `iq.a(-1)` -- setupFog with a negative pass
-- sets the start to 0 and the end to `farPlane * 0.8`, where the world's terrain fog runs from a
quarter of the far plane to all of it. So the sky is already slightly fogged directly overhead,
which is what puts a gradient in a flat ceiling.

**Where it lives here.** `world::skyColour` / `fogColour` / `starBrightness` / `viewFogColour` in
`core/world/daylight`, beside `celestialAngle` and `skyLightSubtracted` -- the same clock, four more
functions of it. The geometry is `core/render/sky`, built **once at start-up** into 70 KB of linear
memory and never touched again: every part of the sky that changes with the day is a uniform, a
combiner constant or a matrix, so there is no per-frame vertex work at all.

- **It reuses the detail pipeline**, with the same trick the chat uses: the vertices are written at
  **1/64 of a block** (`kSkyUnitsPerBlock`) and the matrix carries a factor of 16, because the
  detail format's 1/1024 reaches 32 blocks and the sky plane is 448 across.
- **Two matrices per eye**, both with the eye at the origin: one for the planes, that one turned
  about X for the sun, moon and stars. `Mtx_RotateX(..., bRightSide = true)` was disassembled out
  of libcitro3d before it was trusted -- it is `M * R` with OpenGL's sign, so it is exactly
  `glRotatef` after the camera transform.
- **Its own far plane, 512.** The world's is the render distance plus a ring, which at four chunks
  is 80 -- and the sun's far corner is at 108, so the sky drawn through the world's projection
  would have no sun in it. Depth writes are off and it draws first, so a second depth range costs
  nothing. The original does clip its sky plane (256 at Far) and cannot be seen doing it, for the
  reason the fog colour gives above.
- **`terrain/sun.png` and `terrain/moon.png` are two more pages of the entity sheet**, 32 x 32 in
  slots of their own at (128, 32) and (192, 32). A texture each would be a third bind for two quads
  a frame; the sheet had fifteen slots free. Dev Art gets the usual placeholder grid, warm for one
  and pale for the other.
- **Five draws per eye**, 1,120 quads: two planes, the sun, the moon, and all 780 stars at once.
  The star draw is skipped outright when `starBrightness` is zero, which by day it exactly is.
- The **frame split before the cube pass is no longer free** -- `C3Di_SplitFrame` used to return
  early on the first eye because nothing had been recorded. Something has now, and the split is
  what drains it before the bind that turns the geometry stage on. Same hazard, not a new cost.

**Named deviation: the sky draws at every render distance.** `iq.c(F)V` guards the whole
`renderSky` call with `if (renderDistance < 2)`, so the original at Short or Tiny has no sky at all
-- no sun, no moon, no stars, only the clear colour. That is a frame-rate concession of its era and
not something a player chose to look at, so this port draws it always. The guard is one line in
`Renderer::drawSky` if hardware ever asks for it back.

**Derivation and tests.** `tools/genref.java --jar <client.jar> --sky <scratch>` builds a **real
World**, sets its clock and asks it for all three colours at fourteen times of day; the stars come
from a transcription of renderStars' own loop, because that one builds a display list and cannot be
called. `tests/sky_vectors.hpp` is the output and `tests/sky_test.cpp` checks against it -- the
colours to 1e-4 of a channel, the star count, and ten sampled star centres recovered from the built
quads. The geometry the vectors cannot cover is checked as invariants: every star on the sphere of
radius 100, square, flat and perpendicular to the line from the camera; both planes at +-16 with
64-block cells; the sun and moon inside their own pages.

**Not done, and deliberately.** Clouds. `clouds.png`, `getCloudColour` and `e.b(F)V` / `e.c(F)V`
are a separate pass with a scrolling texture and a fancy 3D variant, and the ask was the sky and
its colour. The cloud base colour is recorded above for whoever takes it.

**The first hardware run: four draws out of five drew the first one's vertices.**

On a console the sky was wrong in a way none of the host tests could see: no sun at any hour, a
pale disc below the horizon at night, a deep navy overhead instead of the sky's own blue, and
nothing at all below. The vertices were right, and so was the matrix -- `Mtx_LookAt`, `Mtx_RotateX`
and `Mtx_Scale` were read out of libcitro3d's disassembly and checked against drawPass's
per-section translation before anything was changed. What was wrong was one line of citro3d
bookkeeping:

```c
C3D_BufInfo* buf = C3D_GetBufInfo();     // fetches the config AND marks it dirty -- once
const auto drawRange = [&](int first, int vertices) {
    BufInfo_Init(buf);                   // mutates it; marks nothing
    BufInfo_Add(buf, base + first, ...);
```

**`C3D_GetBufInfo` is what marks the buffer config dirty, not the writes through it.** So only the
first range's base ever reached the GPU, and the other four ranges drew the *sky plane's* vertices:
the void plane painted its colour over the plane sixteen blocks **above** the camera, nothing was
drawn below at all, and the sun and moon became one 64-block quad at that same height, turning with
the day and seen edge-on as a pale stripe. Every reported symptom is that one missing mark.

The fix is drawChat's pattern -- a local `C3D_BufInfo` and `C3D_SetBufInfo` per draw -- and it
cannot be hoisted, because the base is exactly what differs between the ranges. **The one-draw
passes that fetch and mutate the shared config are correct** (`drawSelection`, `drawCrosshair`, the
TNT flash) and so is the chat's strip loop, which re-bases nothing and only moves the first vertex;
this pass was the only one that changed the base between draws.

**How it was found, since it is the reason to keep this note:** flat colours per piece, then two
quads at plus and minus ten blocks drawn through the *outline* pipeline beside two drawn through
the sky's own. The float pair landed exactly where they were put and the packed pair did not draw
at all -- which is the same bug a second time, because that probe re-used the fetched config too.
A first attempt measured nothing at all because it borrowed the crosshair's vertex buffer, which
`drawCrosshair` rewrites later in the same frame: **the GPU fetches vertices when it runs the list,
not when the list is recorded.**

**Nothing else here has been run on hardware.** It builds for both targets and the arithmetic is under
test; what a console will show is unmeasured, and the fill cost of two full-screen planes plus 1,560
star quads a frame is the number to take first.

### 44. Lava was the wrong colour because it was the pack's, not the client's

"Lava and fire don't have their animation, and lava's colour looks a bit too red." Two reports and
one finding under them: **a1.1.2 registers six `TextureFX` against `terrain.png` and this build had
ported two of them.** §12 did the flames. The other four are `at`, `eg`, `ml` and `ht` -- lava,
flowing lava, water and flowing water -- and nothing had ever read them.

#### The colour is the evidence

The generated lava ramp is `red = heat * 100 + 155`, `green = heat²  * 255`, `blue = heat⁴ * 128`:
red is near its ceiling from the start and **green is what moves**, so lava runs red, then orange,
then pale at the hottest cells. Sampling the real jar's `terrain.png` against the transcription:

| | mean R | mean G | mean B | mean A |
|---|---|---|---|---|
| `terrain.png` tile 237 (what was being drawn) | 245 | 65 | 0 | 255 |
| `TextureLavaFX`, settled (what the client shows) | 224 | 129 | 37 | 255 |

Twice the green. That is the whole of "a bit too red" — the static tile is a plausible-looking
placeholder, which is why it never read as obviously wrong the way fire's pixel-lettering did.
Water's static tile is much closer (41/93/255/138 against 37/60/255/154), so water changes little
and was never reported.

#### Four class files, and they are not each other

`core/texture/fluid_fx.{hpp,cpp}`. The differences are load-bearing and none of them are tidyable:

- **Water** blurs three cells in x over 3.3 and runs its bubble pass as a **second loop**; **lava**
  blurs nine cells over 10 and fuses the bubble pass into the blur loop, so a quarter of its reads
  see this tick's values where the wrap brings them back round.
- Lava's sample square is displaced by `(int)(sin(other axis · 2π/16) · 1.2)` — a sine of the
  *opposite* coordinate for each axis, which is why lava swirls and water shimmers.
- The flowing pair keep a tick counter and read their field through it: `(i - k·16) & 255` for
  water, `(i - (k/3)·16) & 255` for lava. **That scroll is the whole of why a waterfall runs
  downwards**; the simulation itself does not move.
- Water's ramp pins blue at 255 and is **translucent**, alpha 146 rising to 196. A pack's opaque
  water tile was being drawn as a wall of blue.

`tileSize = 2` on the two flowing classes means `RenderEngine.updateDynamicTextures` writes the same
256 texels into a **2 × 2 block** of tiles — which is exactly the four `core/mesh/fluid.cpp` already
spins its sample square across, so those tiles were never decoration.

The randomness cannot be transcribed, as for the flame: `Math.random()` is clock-seeded and two runs
of the *original* disagree. Stated `JavaRandom` seeds instead, which is what makes any of it
testable. **The scroll is pinned exactly**: `TextureLavaFlowFX` differs from `TextureLavaFX` only in
the counter, so with one seed the flowing output *must* be the still output rotated by whole rows,
and the test checks all 40 ticks of that.

#### Six runs a tick, not twelve tiles

Two flames were four `C3D_SyncTextureCopy` of 512 bytes a frame. Ten more tiles naively is
twenty-four. `Atlas::updateTile` grew an `across` count instead, and it works because of a property
of the PICA layout that `tiled_test.cpp` now pins: **tile n + 1's two runs begin exactly where tile
n's end, and where a texel lands inside a run does not depend on the column at all** — subtracting
the run's own base cancels the column term out of `tiledOffset`. So a row of a flowing block is one
picture's words repeated and two copies of 1 KB. Six runs, twelve copies, against four before.

#### A staging race, found on the way, that this change would have made visible

Read out of `libcitro3d.a`, outside a frame `C3D_SyncTextureCopy` is: `gxCmdQueueWait` with no
timeout, stop and clear, `GX_TextureCopy` into the queue, `gxCmdQueueRun`, then
`gspWaitForEvent(GSPGPU_EVENT_PPF, false)` -- which returns at once when the last frame's display
transfer left a PPF unconsumed. The wait that makes a copy safe to follow is therefore at the
*start of the next copy*, and `Atlas::writeTile` filled its one 1 KB staging buffer before reaching
it. Each push could overwrite words the previous push's second copy was still reading.

With two flames that was, at worst, the top half of tile 31 showing tile 47's. With six fluid runs
queued straight behind the flames it would have put water on the fire. Staging is now a 16-tile
pool: every push takes fresh words, and `Renderer::drawFrame` hands them all back only after
`C3D_FrameBegin` has returned, which is the one point the queue is proven empty. A push that finds
the pool spent is refused and its tile keeps its last picture. **This does not explain fire
standing still** -- a torn tile still moves.

#### What is still unmeasured, and what the next run will say

Not measured on hardware — same standing gap as §12's flames, and the reported symptom is **fire
standing still**, which the source cannot explain: the simulation is verified on the host, the tile
arithmetic is under test, and `main.cpp` pushes both tiles between `stepTicks` and `drawFrame`.

The lava bake makes the next run diagnostic. `applyFluidTiles` writes the settled picture into the
atlas at **pack load**, on the path that does not go near VRAM. So:

- lava orange and **moving** → the per-frame VRAM tile copy works, and fire was animating already;
- lava orange and **still** → the bake works and the per-frame copy does not, which is one fault
  explaining both reports, and `Atlas::writeTile`'s VRAM branch is where to look;
- lava still red → the pack load is not calling `applyAnimatedTiles` at all.

---

### 45. The main menu's bottom screen shows what the list is about: skins, a pack scene, a world diorama

**On the main menu the bottom screen of the Skins, Texture Pack and World screens is drawn on the GPU.**
Everywhere else it stays libctru's console, and in game it always is: the world renderer owns the
frame and the VRAM there, which is why `MenuPreview` is made by `Menu::init` and never by
`initOverlay`. While one of the three screens is up a 320x240 RGBA8 + DEPTH16 target is linked to
`GFX_BOTTOM` and drawn inside the top screen's frame. Leaving deletes it and calls
`consoleInit`, which `overlay.cpp` already established is safe to repeat.

**The previews use the world's own detail shader.**
- The vertices are `DetailVertex` and the combiner is texture times vertex colour.
- There is no lightmap and no fog.
- Alpha test is on, so leaves and glass cut out as they do in the world.
- Every view is orthographic. The matrix is built by hand in screen pixels, then passed
  through `Mtx_OrthoTilt` for x and y.
- Depth is written into the middle half of the renderer's reversed range: cleared to 0,
  `GPU_GREATER`, `C3D_DepthMap(true, -1, 0)`. The citro2d backdrop drawn at depth 0 is therefore
  always behind the preview.

**Skins.** Every character is the player model seen head-on through an orthographic camera, which
is a flat picture of it. The selected character is the same model at the same place with the same
scale. Its stride grows from zero over 0.35 s while it turns, so nothing jumps when it takes over.
- The pose is `cr.a(FFFFFF)`, read from the jar with javap, as a player is posed: holding
  nothing, not riding, `swingProgress` 0.
  - The arms swing at `cos(swing * 0.6662 [+ pi]) * 2 * amount * 0.5`, against the legs'
    `* 1.4`.
  - The idle sway is `cos(age * 0.09) * 0.05 + 0.05`, with a roll of `sin(age * 0.067) * 0.05`.
  - The swing block contributes nothing but the arms' rotation points (-5 and 5), which the
    constructor already sets.
  - A zombie's `cb` pose throws exactly this arm swing away, so the biped moved out of
    `mob_mesh.cpp` into `render/player_model` where both can use it.
- **The walk amount is derived.** On the ground the velocity settles at a step of
  `kGroundAcceleration / (1 - kGroundFrictionBase)` a tick, which is 0.2203. `limbYaw` chases
  four times the step, capped at one (mob.cpp, from the jar), so it settles at 0.881.
- Skins are 64x32 pages in one 256x256 sheet, 32 pages. The rows within 8 of the cursor are kept,
  and the page furthest outside that window is reused.
- `texture::decodePlayerSkinPage` is `applyPlayerSkin`'s decode without a sheet. Default is copied
  out of the active atlas's own player page.

**Texture packs.** The scene is 6x5x6 blocks: grass, sand, gravel, a tree, a workbench, a furnace, a
bookshelf, ores, planks, bricks, glass and TNT.
- It is meshed once with `mesh::addBox`, with faces culled against opaque neighbours. Nothing
  about the geometry depends on the pack, so switching packs only rebinds a texture.
- Rows within 2 of the cursor are decoded on the worker by `texture::buildTerrainAtlas`, the
  terrain half of `buildAtlas`, including the generated animated tiles. That is 256 KB of linear
  memory each.
- A New 3DS with more than 24 MB of linear memory free keeps 3.
- A pack that is not ready yet keeps the previous scene on screen. Row 0 shows the active pack.

**Worlds: the diorama.**
- **Coverage:** the 3x3 of 128-block map tiles centred on the tile holding block 0, 0, the same
  for every world; moving it is a separate system still to come. That is 384 blocks, 24x24
  chunks, 576 reads. (It first followed the player's last position; changed on request.)
- **Cells:** a cell is 4 blocks across and **one block tall** (96x96x128, 32x32 a tile). Across,
  0.55 px a block puts the table about 210 px wide, so a finer cell would be sub-pixel; up, every
  height is kept, which is what makes an overhang an overhang. A cell is solid at a height when
  any of its sixteen columns is, and shows the block most of them have -- so one column of stone
  does not turn a cell of grass to stone. `map::showsOnMap` decides what counts, so a torch or a
  rail is air here as it is on the map.
  - **It first kept one height a cell**, the tallest column's, which is what "respects every
    height" replaced. The grid went from 18 KB a world to 1.3 MB (a byte a cell plus a bit),
    so grids cached for the session went from every world visited to 3, or 5 on a New 3DS.
  - The cap is now what a u16 index reaches, 16,384 quads a tile; a tile past it keeps its ground
    and loses overhang undersides. Measured worst tile on generated terrain: 3,259 quads (203 KB),
    21,434 for a whole table (1.34 MB), so the cap is a backstop and not a working limit.
- **Only air the sky reaches is drawn against**, or every cave surface in 576 chunks would be
  meshed and never seen. `openDioramaAir` has two depths because it is the part a 268 MHz ARM11
  would notice: `Sky` is the straight-down scan alone and `Full` follows the air sideways too.
  A tile read runs `Sky`; `Full` runs once, when the whole table is read, and every tile is
  meshed once more against it. `Sky` marks a subset of `Full`, so a tile drawn from it is missing
  faces rather than wrong. **Measured on the dev host over a generated 576-chunk table:** fold
  607 ms (about 1 ms a chunk, against reads that dominate it), `Sky` 8 ms, `Full` 105 ms,
  meshing all nine tiles 242 ms, and a grid of 1.83 MB.
- **Table:** the double slab as one block, a cube 384 blocks on every edge. The top is
  `WorldGen::kSeaLevel` (64), taken from the version's generator, so the bottom is at -320. Ground
  at sea level reads as the table's surface, peaks stand up out of it and pits cut into it. The
  first version ran only from y=0 to 64, a flat slab tiled three times per side; on hardware that
  read as far too thin.
- **Camera:** 0.55 px a block, turned about the middle of the table's **top**, which is put at
  y=100 on the screen. The cube's sides therefore run off the bottom edge on purpose -- the
  ground is what is worth the screen, not the slab. Depth covers 450 blocks: the top's corners
  are 272 out and the cube reaches 384 down.
- **Missing chunks:** a chunk that is absent or not read yet is one quad at table height. Its UVs
  are its share of the double slab's top tile stretched over the whole table, so a missing tile
  is exactly its ninth. The host test checks the thirds.
- **Faces:** every face belongs to the open air in front of it, so each is written once, by the
  tile whose air it faces -- a ground top under open air, a neighbour's side facing it, and the
  underside of whatever hangs over it. Walls merge up the column and tops along x, while the
  block **and** the light both hold.
  - **Two faces have no air cell of their own, and both were holes to see in through on
    hardware.** The **rim**: a hill that runs off the edge of the table faces air that is not in
    the grid, so the table's outer ring is walled separately, above the table's top only -- below
    that the table's own side quad stands in front of it. The **top of the world**: y=128 is
    treated as sky, so a column standing at y=127 is capped rather than open.
- **Light is the world's own.** A chunk carries a sky and a block level per block; a cell keeps
  `max(sky, block)` of its four corners as a nibble, and a face takes the level of the air cell it
  faces. That is what `world::effectiveLightLevel` reduces to at noon, which is where the diorama
  always stands, so the level goes into the vertex's light byte as its sky nibble and the preview
  binds the world renderer's own `Lightmap` on unit 1 with a second texenv stage. Caves with
  torches in them read as lit, and an overhang shades what is under it.
  - Four corners and not all sixteen samples: light is read for most of the volume of every
    chunk, and sampling sixteen took the fold from 607 ms to 1,086 ms for no visible difference
    at four blocks a cell.
- **A face the camera cannot be looking at is never submitted.** The coloured stream is laid
  down grouped by facing (`kDioramaFaceOrder`, counted in `DioramaMesh::run`) and the draw picks
  the groups by `dioramaFaceVisible` for the angle the table is at. The camera orbits at a fixed
  30 degrees above the horizon and never looks up, so overhang undersides always face away and
  two of the four wall directions face away at any moment. Measured per facing over a generated
  table: 5,454 tops (always shown), 4,374 / 3,758 / 3,776 / 3,996 for the four wall directions
  and 76 undersides (never shown) -- so the draw submits about 13,400 of 21,434 quads, 62%, and
  the two visible wall directions vary that only between 61% and 65% over a turn. The table's own
  two far sides are skipped the same way: four quads, but the biggest in the scene. Nothing else
  would drop any of this, because the diorama draws with `GPU_CULL_NONE` like the rest of the
  game, so a back face is transformed and rasterised and then loses the depth test.
  - **What the terrain hides is not worth computing, measured.** Tracing each quad at 32 yaws
    against the grid finds 260 of 21,434 -- 1.2% -- that the camera faces at some angle but can
    never see past the terrain in front of them. `openDioramaAir` has already dropped everything
    sealed inside the ground, and a cell four blocks across swallows most of what is left of a
    cave, so what survives is the table's outer skin and an orbiting camera does see it. The
    stairway that turns back on itself is real, but at this cell size there is almost none of it.
  - Greedy 2D merging was measured against the same cells and keys for comparison: 20,387 quads
    to 16,949, 17% off the vertex memory, at the cost of a second merge axis in the mesher. Not
    in either; recorded so it does not get re-derived.
- **Sides:** the table's four sides run from its bottom to its top, each the slab's side tile
  once.
- **Colours:** `map::averageTileColour` of each block's top, north and bottom faces, with the
  face shade applied.

**Reading a world without opening it is `world/world_peek`.** Both storages claim `session.lock`
on open and rewrite `level.dat` on close, which is wrong for a world that is only being looked
at and may be open elsewhere.
- `WorldPeek` reads the level and chunks through the version slot's codecs and layout.
- It opens a packed region only if its file is non-empty, because `RegionFile::open` initialises
  an empty file in place.
- It never commits: `RegionFile`'s destructor deliberately does not.
- `world_peek_reads_a_{folder,packed}_world_and_writes_nothing` snapshot every file in the world
  before and after a full read and require them identical.
- **`loadChunks` is the batch entry point (2026-09-13).** It takes many chunks at once, groups
  them by region and hands each group to `format::RegionFile::readMany`. **Every chunk asked for is
  visited exactly once**, with a null column for one that was Absent or Failed -- a batch cannot
  tell those two apart, so a caller that needs to wants `loadChunk`, but reporting both is what
  lets a caller count a group down and act on the last one instead of waiting for the batch. The
  order is sector order, absent first, and not the order asked for. A folder world falls back to
  one read each: a chunk there is its own file and no ordering removes an open.

**One worker, `preview/preview_worker`,** runs in the `Generation` role: core 2 on a New 3DS, the
bottom priority on an Old one.
- **Wanting replaces.** Every cursor move posts the screen's list of rows not yet resident, and a
  running job checks `stillWanted` between chunks.
- **Loading order.** The worker reads the centre tile alone, then the other eight **as one
  batch**, with a "complete" marker at the end. The two are split because the centre tile is what
  puts something on the table quickly, and batching the other eight together is what makes the
  reads cheap: their chunks interleave in the region file, so eight tiles at once coalesce into
  far fewer runs than eight tiles apart.
- **The batch does not hold the screen.** `readDioramaTiles` counts each tile's chunks down and
  calls `tileDone` the moment the last one lands -- against a fresh `Sky` pass, since a mesh reads
  `open` -- so `menu_preview` meshes and posts tile by tile while the rest of the batch is still
  arriving. **The first console run without this was the bug**: the centre tile appeared at once,
  the screen then sat for several seconds, and the other eight filled in back to back, because
  nothing was posted until all 512 chunks were read and folded. On the host the nine tiles now come
  back at 9, 24, 43, 45, 58, 62, 78, 80 and 81 ms into an 81 ms read.
  - What made it possible is that `WorldPeek::loadChunks` reports **every** chunk asked for, with
    a null column for one that did not read, and `RegionFile::readMany` puts the absent ones first
    because they cost no I/O. Without an answer for those, a tile with unexplored chunks in it
    could never count down to zero.
  - Splitting the *read* instead was measured and does not work: a second pass over any four outer
    tiles already spans the region's whole sector range, so three passes cost 143 reads and five
    cost 153, against 86 for two -- more operations and no more progress to show.
- **What the batch is worth, measured** over a real packed world's 576-chunk table (1.52 MB of
  payload, four regions), counting `readAt` calls through a counting `RandomAccessFile`:

  | grouping | card reads | transferred | at the modelled 4 ms/op |
  |---|---|---|---|
  | one chunk at a time | 588 | 1.58 MB | 2.35 s |
  | one batch per tile | 176 | 3.41 MB | 0.70 s |
  | centre tile, then the other eight | **98** | 3.02 MB | **0.39 s** |
  | the whole table at once | 75 | 2.87 MB | 0.30 s |

  Counts include the 12 reads that open four regions. The last row is 23 reads better and was not
  taken: nothing would reach the screen until all 576 chunks were in. Widening `kBatchGapSectors`
  and `kBatchReadBytes` instead of batching more chunks was measured and rejected -- per tile, a
  64-sector gap and a 256 KB scratch reaches 62 reads but transfers 6.08 MB, four times the
  payload, for a buffer an Old 3DS should not spend. See `3ds-performance.md` §7.
- **The stop is still a stop.** A batch is asked `keepGoing` before the read and before each
  chunk. A chunk it reached is marked as it is reported -- `Present` if it folded, `Absent` if the
  world does not hold it -- and what it never reached stays `Unread` for the next pass rather than
  being written off as table.
- **Caching, sized against a heap that is empty.** On the menu no world is loaded, and the
  renderer's VBO pool is a cap and not a reservation -- it holds nothing until sections mesh, and
  it is sized in `Renderer::init` and again after `Menu::shutdown` has freed the preview and
  joined this worker. So nothing the menu keeps comes out of the render distance, and the limits
  are measured instead of fixed: `gridLimit()` asks `heapFreeBytes()`, halves it as every reader
  of that figure does, holds back 6 MB and divides by `kDioramaGridBytes` (1.83 MB, derived from
  the cell counts and pinned by a test), with a floor at the old fixed 3 / 5 and a ceiling of 24.
  Mesh slots went from 7 to 16; they are linear memory, of which the menu has tens of megabytes.
  - **Read-ahead did not move, on purpose.** A world is 576 chunk reads off the card, so a fast
    scroll outruns the card and not the RAM; a wider window would only spend card time on worlds
    being scrolled past. `wantedOrder` is driven by `preloadRadius(kWorldRadius)` and not by the
    slot count, so the extra slots hold worlds already read rather than reading further ahead.
    Coming back over a world the cursor has already crossed is the common motion, and that is
    what this makes free.
  - **The console is not as roomy as it looks.** `MemoryType: Application` with
    `SystemModeExt: 124MB` means 64 MB of application RAM on an Old 3DS and 124 MB on a New one,
    and `heap.cpp`'s split puts that at roughly 21 MB newlib / 43 MB linear on an Old console and
    a measured 40 MB / 82 MB on a New one. Grids are `std::vector`, so they are in the *smaller*
    of the two pools on an Old 3DS; tile meshes are `linearAlloc`, in the larger. The two are
    budgeted separately for that reason.
- **Uploads.** Finished work is copied into linear memory on the main thread, at most 600 KB a
  frame.
- **Colours.** Results painted with colours from a pack that has since changed are dropped by
  revision.
- **`quiesce(path)`** runs before delete, copy and convert. `Menu::shutdown` joins the worker
  before a world opens.
- TSan reports no races on the worker, peek or diorama tests.

**The first console run showed a garbled bottom screen with bright blue noise over it.**
- `C3D_RenderTargetSetOutput` hands its flags to `GX_DisplayTransfer` unchanged.
- An `OUT_FORMAT` of 0 is RGBA8: four bytes a pixel into the console's two-byte RGB565
  framebuffer, and a whole second buffer's worth written past its end into linear memory.
- Disassembly shows `C2D_CreateScreenTarget` passing `0x1000`, which is `OUT_FORMAT(RGB8)`, for
  the top screen's BGR8 framebuffer. No object in libcitro3d or libcitro2d references
  `gfxGetScreenFormat`.
- The flags now carry `OUT_FORMAT(gfxGetScreenFormat(GFX_BOTTOM))`; the GSP and GX format enums
  share numbering.

**Otherwise not yet measured on a console.** Check these first on hardware:
- The target's display transfer into the console's single-buffered framebuffer: tearing, and the
  console coming back cleanly.
- The ortho views' depth and facing.
- Frame time with the diorama turning.
- An Old 3DS's time to read 576 folder-format chunk files.

---

### 46. Survival: health on the top screen, a break loop that is not a timer, and four screens made of one slot list

**M3 step 4.** Survival was the last gamemode drawn disabled, and the reason written next to the
flag was specific: a selectable mode that played exactly like Creative would be a label that lies.
It is on now. Everything below came out of `a1.1.2_01-client.jar` method by method; the derivation
with an obfuscated name against every rule is `docs/physics-a1.1.2.md` *Survival*, and only what
was surprising or expensive is repeated here.

**Two new generated tables, because they are tables rather than code.** `tools/genref.java
--harvest` emits `strVsBlock`, `canHarvestBlock`, armour points and food heals; `--recipes` emits
`dw`'s list in match order plus the smelting and fuel tables — 96 recipes, 7 smelting rules, 14
fuels. `tools/configure.py` turns them into `harvest.hpp` and `recipes.hpp` the way `drops.json`
already became one. Both emitters are byte-identical on a rerun, which is the standing bar for an
oracle. `--recipes` has one oddity worth knowing about: `dw.a()` prints "96 recipes" to stdout when
it initialises, so the emitter swallows `System.out` around the call or the JSON is not JSON.

**Health is a struct of its own and not a field on the body.** `PlayerBody` moves against a
`const TickWorld&` on purpose — moving must not be able to write blocks — and it is shared with
every mob. Health writes the world (a hurt sound, a death's worth of dropped items) and the
inventory (armour wears), so it went into `core/entity/player_vitals.hpp` with a `PlayerContext`
carrying the four things a hit can reach. The body reports its landing through a new `landedFall`
field, exactly the shape `steppedOn` and `stepSoundDue` already had.

**Three rules in the damage path are counter-intuitive and all three are the jar's:**

- The invulnerability window and "already dead" are checked in `dm.a`, **before** `ge.a` is
  called — so an overridden `EntityLiving` path cannot be reasoned about on its own.
- Difficulty scales a hit only when the attacker is `dq` (a monster) or `kg` (an arrow). A fall,
  lava, a cactus, a blast and drowning are the same on Peaceful as on Hard — and on Peaceful a
  monster's hit lands and costs zero, rather than not landing.
- Armour carries its remainder between hits (`total % 25`), which a1.1.2 does not save. Neither do
  we; a reload rounds in the player's favour by less than a point.

**The break loop is the part that reads least like a timer.** `nj.c(IIII)` accumulates
`ly.a(Ldm;)F` — relative hardness — and a new cell only *records* itself on the tick it is first
hit. There is a five-tick delay after every break. `dm.a(Ldm;)F` divides the tool's speed by five
with the eye in water and by five again off the ground, so mining while swimming and jumping is
twenty-five times slower than standing still. And the order in `b(III)` is destroy, **wear**,
`canHarvestBlock`, drop — so a pickaxe on its last point breaks the stone and drops nothing,
because the hand is empty by the time the question is asked. That is faithfully reproduced and it
looks like a bug until you read the method.

**The crack overlay has no polygon offset to use.** `RenderGlobal.drawBlockBreaking` sets
`glPolygonOffset(-3, -3)`; the PICA200 has no equivalent, so `core/render/break_overlay.cpp`
expands the box by 0.002 instead, which is the same trick the selection outline already uses.

**The hearts are on the top screen, and that is the one deviation in the step.** Decided with the
user against `docs/3ds-performance.md` §11, which says the HUD belongs on the bottom screen because
the top one then draws nothing but the world. The argument for the exception is that health is the
one number read *while looking at the thing that is hurting you*. It is written up in §11 itself
rather than only here, and **it owes a hardware fill-rate measurement** — at most 150 alpha-tested
quads, two draw calls, 20x9 pixels an icon, and none of that measured on a console yet.

**`gui/icons.png` had the item-sheet bug waiting for it.** The file was in the pack list and never
reached the GPU. `Atlas::initIcons` is called from both `Renderer::init` and `setAtlas`, because
calling it from only the second is exactly how the item sheet came out blank for a release. A pack
without the file gets a generated stand-in, as `entity_skins` already does, so a player still never
has to supply anything.

**The four container screens are one slot list.** `lo` (the inventory with its 2x2), `hx` (the
workbench), `id` (the furnace) and `ea` (a chest) differ only in which slots come before the
backpack, so `core/item/container_session.hpp` is one class with a `resolve(index)` and the click
rules live once in `core/item/container.hpp`. Two things in it are decisions rather than
transcription:

- **The tile entities are copied, not borrowed.** A furnace's contents live in its column's
  `std::vector`, and the furnace's own lit/unlit swap rewrites the block — which can reorder that
  vector under a held pointer. The session pulls a dense copy before every click and pushes it back
  after any click that changed it; nothing outlives a call.
- **A screen whose block has gone closes.** a1.1.2 has no `canInteractWith` in this version at all,
  so a chest blown up under an open screen stays open on a tile entity the world has already
  spilled, and everything taken out of it after that is a copy. Closing is the only answer that
  duplicates nothing.

Two bugs the furnace found on the way: the lit swap loses the tile entity unless it is saved and
put back around `setBlockWithNotify`, and ticking tile entities while iterating the column's list
invalidates it — the tick collects up to 32 furnace positions first and then runs them.

**The bottom-screen layout is core code.** `core/gui/container_layout.cpp` places every slot as a
rectangle and is host-tested for overlap, for staying inside the page band, and for the d-pad's
step; the drawing in `hud.cpp` and the hit test both read the same array, which is what stops a
touch target drifting a few pixels from the thing it is under. The **hand is the hotbar band
itself** rather than a second copy of it, because two hotbars on one screen would have to agree
about which slot is selected. A large chest drops to 17-pixel slots so all fifty-four fit without a
scroll.

**What it was checked against.** 1,603 host cases pass, including nine new files —
`player_vitals`, `tool_rules`, `block_breaking`, `survival_use`, `hud_mesh`, `break_overlay`,
`container`, `crafting`, `furnace`, `container_session` and `container_layout`. `--survive
<world-copy>` runs the whole of it against the real 660-chunk world and fails if a fall of more
than three blocks costs no health, if water never drowns, or if stone broken by hand drops
cobblestone. On that world: a 10.81-block fall took 8 (`ceil(10.81 - 3)`), 320 ticks under water
took 2, stone by hand took 152 ticks and dropped nothing, and a stone pickaxe took 13 ticks,
dropped one cobblestone and lost a point of durability.

**Not measured on hardware.** The HUD's fill, its stereo, the crack overlay's pass, and the four
container screens under a stylus.

### 47. What the first Survival playthrough found, and what the jar said about each

Five reports off one session on hardware. Four were real and are fixed; the fifth is open with the
plain-terrain half of it ruled out.

**The opening hand was Creative's and every mode got it.** `Overlay::setInventory` filled an empty
inventory from the palette whatever the mode was — which is exactly right for a new Creative world
and is the difference in Survival between having to mine the first tree and not. Now gated on the
mode. Spectator is gated out too, and for a reason that is not cosmetic: it has no hotbar to show a
hand in, and the hand it was being given would have followed the player into Survival the first time
the pause menu changed mode.

**Spawning inside the ground.** Two separate things were missing and both are in the jar.

`kh.q()` — `while (posY > 0) { setPosition(); if (nothing collides) break; posY++; }` — is what
stands a new body on top of the ground, and `Minecraft` runs it from `bi.q()` every time a player is
built: at world entry *and* on respawn. This port only ran it on respawn. Worse, it ran it
immediately, against a world whose columns had not streamed in — a lift against air finds nothing to
climb out of and returns at once, and then the terrain arrives underneath the player. So the lift is
now **owed**: the body is held still (which `respawnPending` already did for a respawn) and the lift
is paid on the first tick its own column is resident. One subtlety kept from the original: `q()`
runs *before* `cn.a(dm)` reads `Player` out of `level.dat`, so a **saved** player is placed exactly
where the file says and is never lifted, however buried. Only a world with no player in it yet is
owed a lift.

The other half is that a new world had no spawn point at all — `spawnX/Y/Z` stayed at their
defaults of 0, 64, 0. a1.1.2's `cn` constructor, on the branch it takes when there is no
`level.dat`, walks from (0, 0) by `nextInt(64) - nextInt(64)` on both axes until `cn.f` accepts a
column, and `f` accepts exactly one thing: **`cn.g` reports sand**. That single test is why every
Alpha world begins on a beach, and without it a Survival world could just as easily begin in the
middle of an ocean. `core/world/spawn_point.{hpp,cpp}` is all four methods —  `g` (climb from 63
while the block above is not air), `f` (is it sand), the constructor's walk, and `cn.a()`'s weaker
±8 nudge off a column of pure air — over a probe callback, so the searches are host-tested against
a synthetic world and the generator is only bound to them at the one call site.

**y is never searched for in any of it.** Both loops move x and z; `spawnY` stays 64. What puts the
player on the surface is the lift and nothing else, and that is worth stating because a "find the
ground height" spawn would be a different game: it would put the player on top of the mountain that
Alpha drops them inside of and then lifts them out of.

The one deviation is a bound. The original's walk has no limit; here every step of it is a column
generated on the main thread while the player waits at the menu, so it stops at 512. Measured over
60 random seeds: a median of **19** columns, p90 **106**, worst **265** — so the bound almost never
bites, and at the 3.7 ms a column this host takes (tens of milliseconds on an ARM11) the median
world creation pays about a second for it. It is paid once in a world's life.

**Dying looked like it dropped nothing, and then the A that respawned dropped an item.** Two bugs
that compose into one story, and the jar names both.

`dm.j()` wraps its whole nearby-entity sweep — `boundingBox.expand(1, 0, 1)`, then
`onCollideWithPlayer` on each — in `if (health > 0)`. That guard is not an optimisation: death
scatters the inventory at the player's own feet with a 40-tick pickup delay, and the game-over
screen stands there for much longer than forty ticks. Without it the corpse vacuums its own grave
back up while the player is still reading "Game over!". This port swept unconditionally, so the
player respawned holding everything — which from the outside looks exactly like a death that never
dropped anything.

And then: `Overlay::takeDeathChoice()` clears `dead()` in the middle of the frame, so every guard
below it that asked `!overlay.dead()` came back true again with that same frame's A still set in
`down`. The press that chose Respawn also reached the world as the drop-one-item button. The frame
now runs against a `wasDead` read before the choice is taken, which closes the break and place paths
at the same time.

**The HUD row was too small and in the wrong place.** a1.1.2 measures the row outwards from the
hotbar it sits on top of — hearts at `w/2 - 91`, armour mirrored at `w/2 + 91`, bubbles in the row
above, all 32 up from the bottom of the same screen. This port's hotbar is on the other screen, so
that layout was a row floating across the bottom middle of the world with nothing under it. Hearts
now hang off the top left, armour off the top right, bubbles below the hearts (above is the screen
edge now), and every cell is drawn at `kHudScale` = 2, an integer because the icons are pixel art
sampled without filtering and 1.5 would make some rows of a heart one pixel tall and some two.
**Superseded by 48**, which takes the scale to 1.875 and pays that price knowingly. The
arithmetic inside a row is untouched: same halves, same flash and outline, same one-pixel jitter at
four health — scaled with everything else, so it stays the same fraction of a heart.

**A blob of world full of water from bedrock to ground level — open, and much narrower.** The
first report read as terrain: a roughly circular area where the ground climbed steeply to build
height with everything under it hollow. The follow-up changed the shape of the question — the hole
was **not air**. It was **water, with a layer of ice on it**, in a `SnowCovered` world, and the
outline was organic rather than round.

That is a different fault, and it has a specific shape in the generator: `generateTerrain` writes
water into every cell below y = 64 that the density field leaves empty, and ice at y = 63 when the
world is snow-covered. So "water to bedrock under a wall of stone" is exactly what an **inverted
density field** would produce — `d3 = (scaleNoise + 256) / 512` going negative flips the sign of the
y term, and the column fills below and solidifies above, right through the taper that normally keeps
terrain off the ceiling.

**It is not happening.** Four things were measured rather than reasoned about, and each of them
came back clean:

- **Terrain.** A scan for the inversion signature — solid at y = 120 with water at y = 20 — over
  two seeds x 97 x 97 chunks around the origin, and around 96 000, 960 000 and 6 400 000 blocks out
  where Alpha's noise is known to start degrading. **No hits anywhere.** The deepest water found
  was y = 51 and the tallest unbroken water column twelve blocks.
- **The same scan driven through the jar's own `nw`**, by reflection over an uninitialised `cn`.
  It agrees with the port cell for cell on the summary figures — same highest block, same lowest
  water — so the generator is not merely clean, it is clean *the way a1.1.2 is clean*.
- **Population.** a1.1.2 has no lake generator; the only thing that writes water after terrain is
  `WorldGenLiquids`, fifty single blocks a chunk. Over 17 x 17 chunks of fully populated and lit
  world the tallest water column was twelve, with the lowest wet cell at y = 11.
- **The fluid tick.** The suspicion worth having was that a spring could manufacture source blocks
  and fill a cave from the floor up. A waterfall in a sealed chamber was built in the port and in a
  **real a1.1.2 `cn` World** side by side — same floor, same spring, the jar's clock advanced a tick
  at a time and `cn.a(false)` pumped until it went quiet — and the two settled profiles are
  identical, seventeen cells wide by eight tall, down to which cells are sources. One source goes
  in and one comes out. Both are now `tests/tick_test.cpp`'s
  `a_spring_falling_into_a_chamber_matches_the_jars_own_waterfall`.

One more property fell out of that and is worth writing down, because it is the reason an ocean does
not empty itself into the caves generation carved under it: **still water has no random tick.** Of
the four fluid ids only `water` has `false` in that column (still lava random-ticks, for fire).
Generation schedules nothing, so a sea sits on a cave mouth until a neighbour update reaches it —
which in a1.1.2 means until a player breaks something. `still_water_over_a_hole_does_not_drain_until_something_disturbs_it`
pins it, including the other half: disturb it and it does drain.

So the blocks were written by something outside the generator, the populator and the fluid rules.
What is left is the chunk cache and the packed storage a column is written to and read back from,
the streaming worker, the mesher, and a bucket in Creative. **The world's seed and roughly where the
patch was would settle it in an afternoon** — the generator can be re-run for those exact chunks and
diffed against what is actually on the card, which is the test that separates "generated wrong" from
"written wrong afterwards".

### 48. The bottom screen is two shapes now, the hotbar's slot is square, and a worn tool says so

Five changes to what the two screens look like, four of them layout and one of them a thing that
was never drawn at all.

**The top screen's rows came down by a sixteenth.** `render/hud_mesh` used to scale the icon sheet
by a whole `kHudScale = 2` and argued for the integer: at 1.5, a third of every heart's rows are
one pixel tall and the rest two, which is what makes scaled-up pixel art look broken rather than
bigger. The row was asked to come down by about a twentieth, which no integer can do, so the scale
moved into the space the vertices are already written in -- sixteenths of a pixel -- as
`kHudTexelUnits = 30`. That is 1.875 pixels a texel, 6.25 % under the old 2, and the arithmetic
stays exact: 30 is a whole number of units and nothing in the builder rounds. The artefact the old
comment rejected is still there in principle and is one pixel in eight in practice.

**The hotbar's slot is square.** A slot is 35 or 36 pixels wide -- nine into 320 does not divide,
so `hotbarSlotX` lets the widths differ rather than leaving five pixels of nothing at the edge --
and it was 28 tall. The band went from 32 to 40, which makes the slot 36, and the eight pixels came
from the focus banner rather than from the page: the page still starts at 48.

**The focus banner lost its gradient, which was a rule as well as eight pixels.** The banner faded
from its label row back into the page by *darkening* the pixels it found, and a destructive fade
cannot be applied twice -- so sixteen pixels of screen had to be painted exactly once per clear and
every page underneath had to promise never to touch them. The label row alone says the same thing.
The row is still reserved, because a page that painted over it would erase it, but the reservation
is one character row instead of two and carries no arithmetic.

**Spectator's pages use the forty pixels the band is not taking.** The old note in `hud.hpp` argued
that a page whose height depended on the gamemode would be two layouts, two sets of constants and
two map window sizes to measure -- so Spectator got 32 pixels of backdrop and a smaller map for the
sake of one number. It is two layouts now and the argument is answered rather than ignored: every
page is a `constexpr` function of the page's first row (`lookLayout`, `paletteLayout`,
`itemsLayout`, `columnFor`, `bandFrom`), written once and evaluated twice, and both answers are
checked at compile time against `kBandedPageTop` (48) and `kBarePageTop` (8). `hud::pageTop()` says
which is in force; `Overlay::setGamemode` sets it, **before** its own early return and from the
argument rather than the field, because `gamemode_` starts as Spectator and a Spectator world would
otherwise never tell the screen what shape it is.

The map really is the one place that is two sizes, and the cost is an estimate rather than a
measurement: 36,864 pixels redrew in about 700 microseconds on a New 3DS, the banded window is now
212 x 162 = 34,344 (roughly 650) and the bare one 212 x 202 = 42,824 (roughly 815). Neither has
been run on hardware and `3ds-performance.md` §11 owes this page a figure either way. What the
taller window does change for certain is the chunk store: 212 x 202 at zoom -1 is 424 x 404 blocks,
which touches 28 x 27 = 756 patches against the 588 the banded window touches. Sizing the store for
the banded window would thrash the moment a player switched mode -- the ring scan touches the centre
first, so the least-recently-used entry is the ground under the player -- so the capacities went up
with the window and kept their headroom: 1,024 on an old 3DS (1.5 MB) and 2,048 on a New one (3 MB),
4 % of each console's newlib heap.

**The margins went to the pages.** The map is 212 wide rather than 208 and stops four pixels short
of the tab strip rather than eight; the palette's panel and the look pad do the same; the inventory
panel is inset one pixel a side rather than two and its slots are 31 rather than 30, which is 67 %
more area than the 24-pixel slots the page started with. The container screens follow through
`buildContainerLayout`, which takes the page's top as an argument now -- in practice always
`kLayoutPageTop`, because the last nine slots of every one of those screens are the player's hand
and a mode that can open one is a mode that has a band to draw them in.

**A worn tool now says so.** `ab.b(kd,ey,ev,II)V` -- RenderItem.renderItemOverlayIntoGUI -- is the
method the stack count was already taken from, and the wear bar is the half of it that had never
been drawn. Its arithmetic is integer, not the `Math.round` of later versions:

```
int j = 13 - damage * 13 / maxDamage;     // how much bar is left
int k = 255 - damage * 255 / maxDamage;   // how green it still is
```

then three quads at `(x + 2, y + 13)` over a 16-texel icon: black 13 x 2, then 12 x 1 in
`(255 - k) / 4 << 16 | 16128`, then the bar itself `j` x 1 in `(255 - k) << 16 | k << 8`. So the
spent part is a quarter-bright dark green, the remaining part runs green to red, and the black
quad's second row is the bar's underline. The icons here are 24 pixels rather than 16, so
`drawWearBar` scales texel *edges* rather than texel counts -- `span(13, 15)` and `span(13, 14)`
give 3 and 2 at that size rather than 3 and 1, which is what keeps the two quads in proportion.

It shows exactly when the jar shows it: `ev.d` greater than zero. Every item in a1.1.2 carries
`maxDamage = 32` from `Item`'s own constructor, blocks included, and `data/a1.1.2/items.json`
records that faithfully -- but nothing in this build writes damage to a stack that is not a tool,
a weapon, armour, a hoe, a bucket or flint and steel, and a1.1.2 has no metadata-carrying block
item for the field to mean something else on.

**And ZL/ZR take the cursor with them.** The shoulder pair changes the slot in hand from anywhere,
focused or not. Focused on the inventory or the palette the cursor is usually down in the grid, so
a press used to move the white outline in the band while the amber one stayed where it was -- two
marks disagreeing about which slot the next press acts on. `Overlay::cursorToHand` moves the cursor
to the band, and knows the screen's two cursor spaces: a container session numbers the hand as its
last nine slots, so it moves `containerCursor_`; every other page has the band as the place the
cursor is when it is not on the grid, so it clears `focusGrid_`. The map page has no cursor and is
left alone.

Coverage: `tests/hud_mesh_test.cpp` measures the rows in vertex units rather than pixels now, since
the scale is no longer a whole number of them; `tests/container_layout_test.cpp` passes the page top
explicitly. The rest is 3DS-only drawing and has not been seen on hardware. 1,609/1,609 host tests
pass; the 3DSX cross-build succeeds.

### 49. The near plane was three bugs, the spawn lift ran a tick too late, and the death screen outlived its world

Four reports off one Survival session. Three of the four are one number.

#### The near plane was three bugs

Reported as three separate things: the view cuts through the ceiling when you jump, it cuts into the
wall and the floor when you die, and standing against a wall and pitching down slightly cuts through
the corners.

**They are all `kNearPlane`, and the reason it was not obvious is that on this console the near plane
is not in front of the eye.** `Mtx_PerspStereoTilt` builds an *off-axis* frustum, so from the row
read out of `libcitro3d.a` (the same derivation `cullFrustum` already carried) each eye is displaced
sideways by `t*A*iod/2` and its near rectangle spans `[-t*A*n, +t*A*n]` around that displaced point.
With fov 70 on 5:3 (`t = 0.700`, `A = 400/240`) and the default 7 px of infinity disparity, the
displacement is **0.19 of a block** — most of the way to the player's own half-width before the
rectangle has any width at all:

| `kNearPlane` | sideways reach | upward reach | corner radius |
|---|---|---|---|
| 0.2 | 0.420 | 0.244 | 0.486 |
| 0.1 | 0.305 | 0.122 | 0.329 |
| 0.05 | **0.248** | **0.061** | 0.255 |

Against those: the player box is 0.6 across, so **0.30** to a wall it is flush with; and it is 1.8
tall against a 1.62 eye, so **0.18** to a ceiling its head is against. At 0.2 both are lost, which is
all three reports — jumping loses the ceiling, walking along a wall loses the wall, and a pitch
swings the corner into whichever is nearer. Note that 0.1 does not fix the wall: the sideways figure
is dominated by the stereo displacement, which does not shrink with the near plane.

`kNearPlane` is a1.1.2's own **0.05** now. The stereo displacement is horizontal in *screen* space
and nothing here rolls the camera, so it never costs ceiling clearance — only wall clearance, which
is the tighter of the two margins that remain.

**What made the trade obsolete was §"Shimmer" above.** 0.2 was chosen against a 16-bit depth buffer,
where `d^2*(far-near)/(near*far*65536)` at the far plane is metres of block. Both eyes have been
`GPU_RB_DEPTH24_STENCIL8` since that fix, which multiplies resolution by 256: at 0.05 and a
176-block far plane a depth unit is **0.037 of a block at the far plane itself**. The comment on the
constant had not caught up — it still argued from 16 bits — and neither had two others that quoted
0.2, including the arithmetic under the held item's depth window. That window in fact gets *safer*:
a world fragment reaches the hand's 0.95 only within `kNearPlane / 0.95` of the eye, so the shell
the argument depends on shrank from a centimetre to five millimetres.

What this gives up is what the original gives up: stand inside a block and the face is drawn rather
than clipped away, which is the face filling the screen. That is the behaviour being matched.

**Not measured on hardware.** The numbers above are arithmetic on the projection; what 0.05 looks
like through the 3D slider is a hardware question.

#### Half a heart for being born

Reported as a fresh Survival world opening at 19 health, and on respawn too.

`respawnBody` stands the body at `spawnY + 1`, and `spawnY` is 64 whatever the ground there is
doing — §47's lift is what takes it out of the hillside, owed until the column is resident. But the
frame loop ran the player's `y()` **first**, at the top of the same tick iteration, and `ge.y()`
asks whether the eye is inside an opaque block and takes a point for it. One tick of that is one
point: half a heart, with nothing on screen to say why.

a1.1.2 cannot have this bug, and the reason says what the fix is: `kh.q()` is called from `dm`'s
constructor, so the body is out of the ground before anything ticks it at all. The wait-and-lift
decision is hoisted above the `y()` block now — lift, then `y()`, then move — and `y()` is skipped
outright on a tick the body is still being held for its column, where it is standing in a chunk that
does not exist yet.

Host coverage: `tests/player_vitals_test.cpp` →
`an_unlifted_spawn_point_suffocates_and_the_lift_is_what_stops_it`, which pins the ordering rule
itself (buried body, one `y()`, 19 health; the same body lifted first, 20) because the loop it was
broken in is 3DS-only.

#### The death screen outlived its world

Die, choose *Title menu*, open another world: the game-over screen is on the bottom screen over a
living player, and there is no way out of it. `Overlay` is one process-long object — `begin()` is
what makes it forget the last world — and `dead_` was not among the things `begin()` cleared. Only
the death screen itself clears the flag, and the frame loop only raises the screen on
`!vitals.alive() && !dead()`, so the state was self-sustaining. `begin()` clears the four death
fields directly rather than calling `setDead(false)`, which would close a container session and
re-sync the inventory that `begin()` is about to set up in its own order.

### 50. The chest: a texture that reads its neighbours, the triple-chest hole, the seed that breaks bedrock, and a screen that scrolls

Four things asked for together, and three of them are one class -- `b`, BlockChest -- read out of
the jar with `javap -c`.

#### The texture: `b.a(Lnm;IIII)I`, which is a branch and not a row

`blocks.json` records a block's six tiles as a table, and for the chest that table is the
*inventory* answer (`b.a(I)I`: lid 25, side 26, front 27 on +Z). In the world the chest asks about
the cells around it, which a table cannot say, and the extractor has always reported it rather than
guessed -- `worldDependentFaces`, printed on every run.

So there is a new generated column, `worldTexture`, assigned by a maintainer exactly as `tick` is:
the extractor can name which faces need a world, but the *rule* behind them is a branch and has to
be written. The rules live in `src/core/block/world_texture.hpp`, and the chest is the only one
with an entry.

What the rule does, transcribed rather than tidied:

- **A lone chest turns its front away from an opaque neighbour.** Four tests in the order -z, +z,
  -x, +x, last one wins, so a chest in a corner faces along x. It reads `Block.opaqueCubeLookup`
  (`ly.p`), which is `BlockDef::opaqueCube` and not `opaque` -- glass does not turn a chest.
- **A pair draws one picture across two cells.** The joining faces go plain (26); the long sides
  take the double front (41/42) or the double back (57/58), and which half a cell draws is an
  offset of -1 or 0. The -x face of a z-pair and the +z face of an x-pair see it mirrored, which
  is the original's `offset = -1 - offset`, and it is why the seam does not swap sides as you walk
  round.
- **It reads the two cells beside the *partner* as well**, so a block at either end turns the whole
  pair. That is eight cells at most, all within the mesher's one-cell scratch pad.
- **Three in a row is not modelled and that is faithful.** The middle chest pairs with its -z or -x
  neighbour and the third draws half a pair that is not there, which is what a1.1.2 shows.

The chest leaves the mesher's fast path for the same reason the furnace did (that path draws
`faces` and reads no neighbour), which `configure.py` now folds into `unitCube` from the new
column. One more thing changed with it: `WorldStreamer::tickBlockChanged` invalidates the **fourth
corner** section as well. Face culling never needed a diagonal neighbour; this rule does, so a
block dropped across a column corner used to leave a stale chest face until something else touched
the section.

#### The triple chest, which is a hole in `World.canBlockBePlacedAt`

`b.a(Lcn;III)Z` -- canPlaceBlockAt -- counts the chests beside the cell, refuses more than one, and
refuses any neighbour that is already half of a pair. It was not ported; now it is, as
`TickBehaviour::Chest` in `tick::canPlaceAt`.

And **placing it into water still makes a triple**, because `cn.a(IIIIZ)Z` returns true for a cell
holding water, lava, fire or a snow layer *before* it asks the block at all:

```
if (existing == water || lava || fire || snow) return true;
return id > 0 && existing == null && Block.blocksList[id].canPlaceBlockAt(...);
```

That early return was already transcribed in `core/item/use.cpp` (it is what lets a sapling go into
a pond), so adding the chest's rule above it gave the bug for free: dry land refuses the third
chest, a puddle does not. `tick::chestInventoryParts` already nested up to five, and its header no
longer calls a cluster "placeable" -- it says how it is reached.

#### The seed that breaks bedrock: `jn.a(Lev;Ldm;Lcn;IIII)Z`

ItemSeeds is seven lines and one of them is missing:

```
if (l != 1) return false;
if (world.getBlockId(i, j, k) == Block.tilledField.blockID) {
    world.setBlockWithNotify(i, j + 1, k, blockType);
    itemstack.stackSize--;
    return true;
}
```

No air test, no `canBlockBePlacedAt`, no `canPlaceBlockAt` -- **the only placement in a1.1.2 that
asks nothing**. The crop is written over whatever is above the farmland, and farmland is 15/16 tall,
so its top face stays clickable through the gap under a block sitting on it. A seed therefore
removes bedrock, which nothing else in the game does. The port had been routing item 295 through
`ItemBlock.onItemUse` (its `places` column is 59), which tested all three things and quietly fixed
the bug; it now has its own path, routed by the behaviour of the block it places, as the door, the
sign and the fire branches are.

#### The screen scrolls, and that part is ours

a1.1.2's chest screen is as tall as its chest, because its chest is never more than double. A
cluster of three, four or five is 9, 12 or 15 rows -- up to 255 pixels of slots in the 106 the page
band has. So `container_layout` takes a `chestFirstRow`, shows six rows, and gives every slot
outside the window a **zero-sized rectangle**: nothing draws it, no touch finds it, no d-pad step
walks on to it, and every slot index stays exactly what `ContainerSession` numbers it. Clamping
lives in the layout, so the caller can hand back whatever it was holding.

The controls are the console's: L and R scroll (a world screen has no tabs for them to step),
the two arrow buttons in the gutter beside the grid take a touch, and a d-pad press off the top or
bottom row of the window scrolls instead of leaving the grid -- the content moves under the cursor
rather than the cursor running off the window, and once there is nothing left to scroll to, the
next press leaves for the backpack as it always did. The bar beside the grid is drawn with a thumb
as long a fraction of its track as the window is of the chest.

#### Four things the first look at it on a console found

- **A double chest wore grass down its long sides.** The cube pass does not sample terrain.png; it
  samples a 512 x 512 atlas of 64-pixel repeat slots, handed out per tile *a cube face can show* --
  and that list was built from the block table's `faces` rows, which is exactly where the four
  halves of a double chest's picture do not appear. `cubeSlotOf` falls back to tile 0's slot for a
  tile it does not know, and tile 0 is grass. `buildCubeAtlasLayout` now also asks
  `block::worldTextureTiles`, which is the rule's own list of every tile it can emit (7 for the
  chest, 4 of them new). The atlas needs **58 of its 64 slots**, up from 54; the pin in
  `greedy_test.cpp` moved with it, and `chest_test.cpp` checks the claim both ways -- every listed
  tile has a slot, and every tile the rule produces over all 81 neighbour arrangements is listed.
- **The chest screen's title is gone.** "Chest" sat across the top row of slots on a single chest
  and "Large chest" took two lines beside a screen that has no room to spare. The other three
  screens keep theirs: a result slot, an armour row and a furnace's two inputs do not say what they
  are, and a grid of chest slots does.
- **The scroll thumb was the page's own colour.** Flat `kPanelFace` inside a near-black track reads
  as a hole in the bar rather than as the bar; it is a raised bevel on `kSlotFace` now, and the
  track is 8 pixels wide instead of 6 so the bevel has room.
- **The world list kept a row, not a world.** `listWorlds` sorts by last played, so coming out of a
  world moves it to the top and everything above it down one -- and `worldCursor_`, which survives
  the trip, then pointed at a neighbour. It is remembered by **name** now (`worldCursorName_`), put
  back by `refreshWorlds`, and `createWorld` sets it to the world it just made *and* leaves the menu
  on the world list rather than on the Create screen, which is where `runPause`'s `resumeScreen_`
  used to drop the player after a brand new world.

#### What was checked

`tests/chest_test.cpp` is new: the texture rule over a 3 x 3 patch (lone chest in all four
orientations, both pairings, the mirrored halves, the diagonal that turns a pair, and the broken
triple), the placement rule through `item::rightClick`, and the water bug making a triple that
`chestInventoryParts` then joins. `tests/use_test.cpp` covers the seed on farmland, off the top
face, and through bedrock; `tests/container_layout_test.cpp` covers the window, the clamp, the
hidden rectangles and the bar. Host suite **1621/1621**, the 3DSX build links, and
`extract_blocks.py --verify` still agrees with the jar on all 70 blocks. **No hardware run of my
own**; the four fixes above came off one, which is where the grass came from.

## Standing constraints

- **Never ship Mojang assets** — textures, sounds, fonts, jar contents. The bundled fallback pack
  must be CC-licensed and attributed in `romfs/licenses.txt`.
- **ViaLegacy is GPLv3: documentation only.** Protocol IDs and wire sizes are facts about a 2010
  protocol; its code is never copied.
- **craftus_reloaded is MIT** — reusable with attribution.
- **Jar extraction is a maintainer step**, never a player step and never part of the build. Its
  output is checked in.
- **The real world is worked on through copies only.**
