# Architecture

## The core/platform split

The single most important structural decision: **all game logic is platform-independent and builds
for the host (Linux/SDL2) as well as the 3DS.**

3DS iteration is slow — build, package, then either boot an emulator or copy to an SD card. NBT
bugs, protocol bugs, worldgen mismatches and mesher errors are all far cheaper to find in a desktop
debugger with ASan/UBSan than on hardware. Anything in `src/core/` must compile and run on the host
with no `#ifdef _3DS`.

```
src/
  core/                version- and platform-independent; builds for host + 3DS
    nbt/               NBT reader/writer, generic tree copy + dump
    world/             Chunk column, Section (16^3), palette storage, lighting
    world/format/      the packed on-disk format: region containers, manifest, conversion
    block/             BlockDef + render types; the table itself is generated
    item/              ItemStack; later ItemRegistry, recipes
    entity/            entity types, physics, AI
    net/               Connection, PacketCodec, table-driven dispatch
    mesh/              section -> 12-byte vertices; the per-section visibility mask
    render/            the visibility walk: what to draw, and in what order to mesh it
    io/                FileSystem seam + the POSIX implementation both targets use
    util/              gzip/zlib wrappers, Vec3/Mat4/Plane, frustum, ring buffers, work queue
  impl/                slot implementations; only the selected ones are compiled
    storage/alpha_chunkfiles/     strings/modified_utf8/    items/b1_2/
    worldgen/alpha_nobiome/       containers/none/          entitydata/none/
  platform/
    ctr/               libctru/citro3d renderer, SOC networking, FS, ndsp audio, input, stereo
    host/              SDL2 + OpenGL mirror of the same interfaces
versions/              a1.1.2.json — build-time manifest per game version
data/a1.1.2/           blocks.json, items.json, recipes.json, packets.json, atlas.json
shaders/               world.v.pica, gui.v.pica, (optional) quad.g.pica
tools/                 configure.py, atlas packer, ETC1 encoder, codegen, protocol replayer
tests/                 host-side unit and round-trip tests
build/<version>/gen/   generated: version_config.hpp, tables.cpp, packets.cpp, sources.mk, .rsf
```

The platform layer is a set of narrow interfaces — `IRenderer`, `IFileSystem`, `ISocket`, `IAudio`,
`IInput` — implemented twice. `core` never includes `<3ds.h>` or `<citro3d.h>`.

`IAudio` exists now and is called `audio::Backend` (`core/audio/backend.hpp`): ndsp behind it on the
console, a `.wav` writer behind it on the host. It is deliberately the narrowest of them — start a
stream, stop it, ask whether it is still playing — because that is all a1.1.2's music ticker needs
and nothing in the port can emit a sound effect yet.

The file system is the exception that proves the rule: `io::FileSystem` is an interface, but its
POSIX implementation lives in `core` and serves *both* targets, because devkitARM's newlib provides
the descriptor API and libctru's devoptab maps it almost directly onto the FS service. One
implementation means the host tests exercise the code the console runs. It is also one of the few
places the project accepts a vtable — called once per file, never per block, and binding it
statically like the version slots would stop tests from injecting anything.

Concretely: `core` plus the selected slot implementations build into a static library
(`3dalpha_core`), and both the app and the host test binary link it. The one include root is `src/`,
so every include carries its tree — `#include "core/nbt/nbt.hpp"`, `#include
"impl/storage/alpha_chunkfiles/chunk_path.hpp"` — which makes an accidental dependency on a slot or
a platform visible in the include line itself.

Fixed-width types live in `core/util/types.hpp` as `mc::u8` … `mc::i64`. They deliberately mirror
libctru's spelling so platform code reads like core code, but they are namespaced, so core has no
global typedefs to collide with `<3ds.h>`.

Host tests build with `-fsanitize=address,undefined`. That is the point of the split: an
out-of-bounds read in the NBT parser is a diagnosed line and a file name here, and a silent
corruption or a hang on hardware.

## Version selection — the anti-hardcoding mechanism

**One binary per Minecraft version, selected at configure time.** An a1.1.2 build contains zero
b1.7.3 code, because the other versions' sources are never handed to the compiler. Nothing about the
version is decided at runtime, so there is no JSON parser and no profile lookup in the shipping
binary — the tables are `constexpr` arrays in `.rodata`.

`versions/<id>.json` is a build-time manifest. `tools/configure.py` turns it into
`build/<id>/gen/`:

```cpp
namespace mcver {
    inline constexpr int  kProtocol    = 2;      // constants
    inline constexpr int  kWorldHeight = 128;
    inline constexpr bool kHasWindows  = false;  // feature bits

    using Storage     = impl::AlphaChunkFileStorage;   // slot implementations,
    using StringCodec = impl::ModifiedUtf8Codec;       // statically bound - no vtables,
    using WorldGen    = impl::AlphaNoBiomeGen;         // the compiler inlines through them
}
```

Consequences that must hold everywhere:

- No literal packet ID outside a generated packet table.
- No literal block ID outside the registry. `data/<version>/blocks.json` is code-generated into
  `enum class Block` plus a `constexpr BlockDef kBlocks[256]` in `.rodata` — indexed directly by id,
  so a lookup is a bounds check and a load, with no map and nothing to build at startup. Ids the
  version does not define resolve to a visible unknown block rather than reading past the end,
  because a world can legally contain one.
- World height, section size and inventory layout come from `mcver::`, not from `#define`s.
- The renderer knows only *render types* (cube, cross, fluid, torch, rail, door, stairs, …),
  never block IDs.
- **No `#ifdef` on version.** Small deltas use `if constexpr (mcver::kHasX)`, which still
  type-checks the branch it discards; whole differing subsystems become slots.

Full scheme, including title IDs for installing several versions at once, in
[build-versions.md](build-versions.md). Version-specific checklists in
[porting-to-other-versions.md](porting-to-other-versions.md).

## Threading model

**What the code actually creates**, read off `platform/ctr/main.cpp`'s `workerSpawn` rather than
planned. A larger priority number is a *lower* priority on the ARM11, and the kernel is SCHED_FIFO:
equal priority is a queue, not a share.

| Thread | Old 3DS | New 3DS | Work |
|---|---|---|---|
| main | core0 | core0 | game loop, GPU submission, input, **meshing** |
| generation worker | core0 at `0x3F` | **core2**, one step below main | worldgen, inflate, borrowed read-only column work |
| audio decode | core0, one below main | **core2**, one *above* generation | Vorbis into the NDSP ring |
| I/O | core0, two below main | core0, two below main | chunk file read/write |
| net | core0, two below main | core0, two below main | `poll`, packet parse, column inflate |

Three things in that table were wrong for long enough to be worth naming:

- **Meshing is on the main thread**, behind a per-frame budget, and is survivable there at 30 µs a
  section. The table used to promise it to the worker. Worldgen is what forced the worker and is
  the half that exists: a generated column is 3.7 ms on a desktop and therefore tens of
  milliseconds on a 268 MHz ARM11, several whole frames for one column. `WorldStreamer` runs
  generation on one worker, one column at a time.
- **The I/O thread is on core 0 on both consoles**, not core 3. It spends nearly all its life
  blocked in an IPC round trip to the FS sysmodule, so a core of its own would waste one — and on a
  New 3DS core 2 is already the generator's. **Core 3 is the system's and this application never
  gets it.**
- **Core 2 has two tenants, not one.** The audio decoder is there too and sits one step *above*
  generation, because `workerMain` takes its next job the moment it finishes one and a full slate
  never let a lower-priority thread in. That is what made the music skip under load.

**Who creates that worker is a seam, and it has to be, because `std::thread` cannot name a core —
and on this toolchain it does not merely decline to.** devkitARM's pthread shim calls
`threadCreate(func, arg, 32*1024, 0x3F, 0, false)` with the core hardcoded, so every `std::thread`
lands on **core 0 at the bottom priority**, beside the render thread. The ARM11 scheduler is strictly
priority-ordered, so a worker there runs only in what is left of a frame once the main thread blocks.
That is a sliver on a console holding 30 fps, and it is why a walking player could outrun generation
and never see it catch up.

`WorldStreamer::setWorkerThreadOps` lets a platform create the thread itself.
`src/platform/ctr/main.cpp` uses it to put the worker on **core 2 on a New 3DS** — free to an
application, and already granted by Luma's synthesised 3DSX exheader (`0xFF002109`, bit 13 "Access
core2") — at the main thread's own priority, because there is no one on that core to be polite to.
On an old 3DS there is no such core and the fallback is core 0 at priority `0x3F`, which is what the
shim would have done anyway, now said out loud. The debug page reports which one it got, since
asking for core 2 and getting it are different questions.

**Its ordering rules are load-bearing, not incidental.** Population order is the world in a1.1.2, so
the worker consumes a queue built by the main thread in spiral order, strictly in order, one at a
time; job N therefore starts against the world left by jobs 1..N-1 on any machine at any frame rate.
`tests/streamer_generate_test.cpp` compares a threaded and an inline run of the same path chunk file
for chunk file. See [status.md](status.md) §0.

Rules:

- Results are published through a **revision counter** per chunk/section (the pattern
  craftus_reloaded uses). A worker stamps the revision it read; on harvest, a result whose revision
  no longer matches is discarded rather than applied. This removes almost all locking from the hot
  path and makes stale results a non-issue.
- The main thread never blocks on the worker. Mesh results are drained once per frame under a
  `LightLock_TryLock` — if the worker holds it, we simply try again next frame.
- The 3DS has no `epoll`. Networking uses non-blocking sockets + `poll()` on the I/O thread, with a
  ring buffer handing complete packets to the main thread.
- Never decompress or touch the filesystem on core0.

## Memory budget

Old 3DS gives 64 MB of application RAM by default. Target allocation:

Memory comes in three pools, and **which pool** matters as much as the total: the GPU can fetch only
from linear memory and VRAM, never from the newlib heap.

| Consumer | Pool | Budget (o3DS) | Budget (n3DS) |
|---|---|---|---|
| Block data (palette sections) | heap | ~8 MB @ render distance 6–8 | ~20 MB @ distance 10–12 |
| Chunk generator, while making world | heap | ~3 MB @ distance 6 | ~7 MB @ distance 10 |
| Network / decompression scratch | heap | ~1 MB | ~1 MB |
| Code, stack, libc, misc | heap | ~8 MB | ~8 MB |
| Mesh VBO pool | linear only | ~12 MB | ~32 MB |
| Textures (atlas + GUI + entities) | VRAM | ~1 MB | ~2 MB |
| Render targets (stereo top + bottom) | VRAM | 0.8–1.5 MB | same |

That pool row used to read "VRAM-first, linear fallback". **The CPU cannot write VRAM** — the store
takes a permission fault — so a tier the CPU fills every frame cannot live there. Textures and
render targets still do, because the GPU is what writes them. See
[3ds-performance.md §3](3ds-performance.md).

**The linear heap is the binding constraint, not total RAM.** libctru's default split gave 91 MB
newlib heap and only 32 MB linear on a New 3DS — 70 MB parked where meshes cannot go. We replace
`__system_allocateHeaps` with a linear-heavy policy, measured on hardware at **40 MB heap / 82 MB
linear**, which multiplies the mesh budget by 2.6× and therefore the achievable render distance. See
[3ds-performance.md §2b](3ds-performance.md).

The generator's row is transient and only exists while ground is being made: it holds two rings of
frontier that its population passes have not finished with, and that band's length grows with the
radius being filled — 98, 162 and 210 raw 32 KB columns at load radius 4, 8 and 11, measured. Once
the columns are on the SD card the streamer reads them and the generator never runs. It is sized by
`ChunkGenerator::cacheColumnsFor`, and undersizing it is not a slow path but a wrong world.

A raw Alpha chunk column is **80 KB**; at render distance 8 that would be 23 MB of block data alone.
Palette-compressed 16³ sections bring a typical column to ~25 KB. See
[3ds-performance.md §Storage](3ds-performance.md).

The debug overlay reports free linear heap and free VRAM every frame. Watch it during sustained
flight, not just at spawn — fragmentation shows up over minutes.

## Frame structure

The process is a shell around this loop rather than the loop itself: `C3D_Init`, then menu, then
game, then menu again, with the menu owning its own frame loop while it is up (citro2d, top screen,
one target it creates and gives back around each visit) and `runGame` owning the one below. START
opens the **pause menu** — Resume, Options, Exit World — which is the *same* `Menu` object running
the *same* frame loop over a world that is still open, so the world is genuinely stopped while it is
up; Exit World leaves for the main menu and only the title screen's Quit ends the process. **In a
session it is stepped instead**, one call per frame from `runGame`'s own loop, because a console
that stopped would stop for everybody it is linked to — the menu is the same screens either way, and
what changes is which of the two loops owns the frame. See `platform/ctr/menu.hpp`.

**A menu takes the top screen and does not hand it back.** citro3d holds one linked target per
screen output, so the menu's own target evicts the left eye and deleting it leaves the slot empty —
`Renderer::reclaimScreen()` is what re-links both eyes and re-syncs `gfxSet3D` after every pause.
Nothing needed it while a Renderer was built and torn down around each menu visit; a pause menu is
the first thing that outlives one.

```
poll input, HID + touch
drain worker results (try-lock)
tick world (fixed 20 Hz, decoupled from render)
build visible-section set  (frustum + flood-fill BFS, once for both eyes)
C3D_FrameBegin
  for each eye (skip eye 2 when 3D slider == 0):
    clear, set stereo projection, draw opaque front-to-back, draw translucent back-to-front
  if bottom screen dirty: clear + draw GUI
C3D_FrameEnd
enqueue mesh jobs for dirty sections (budgeted per frame)
```

Visibility is computed once and reused by both eyes with a slightly widened frustum — only the
projection matrix and the draw pass differ per eye.

Time of day never enters this loop. Sky light is sampled per fragment from a 16x16 lightmap texture
rather than baked into vertex colour, so advancing the clock rewrites 256 texels and rebuilds
nothing. Measured free on hardware — see [3ds-performance.md §1b](3ds-performance.md).

## Rendering far from the origin

a1.1.2's world runs to ±32,000,000 blocks and its terrain generator produces the Far Lands at
12,550,824 (see [worldgen-a1.1.2.md](worldgen-a1.1.2.md)). Those are reachable coordinates, so the
renderer has to stay correct at them.

**A float stops being able to name individual blocks at 16,777,216.** Below that it holds every
integer exactly; above it the spacing is 2, then 4. At the Far Lands the spacing is exactly 1 block,
which means two separate failures:

  * `Camera` held its position as `float`, so the player's position *snapped to the block grid*.
    Walking became a series of one-block jumps with nothing in between — frozen for four frames at
    0.05 blocks a frame, then a whole block at once. The original does not have this: `Entity.posX`
    is a `double` and level.dat stores it as `TAG_Double`, which `world::PlayerData` already
    modelled. The camera is now a `double` too, so this was a fidelity gap and not a trade.
  * Even with an exact camera, the mvp matrix is a *difference* of two numbers around 12,550,000 —
    the section's world origin and the camera's negated position. Both are exactly representable
    (they are multiples of 16) and their difference still is, but the intermediate products inside
    the matrix multiply are not, so geometry shivers by a block or two as the camera moves.

**The fix is a change of origin, not an approximation.** Every frame the renderer takes the camera's
own chunk as the render origin:

  * `Renderer::viewProjection` places the eye at `camera.x - originChunkX * 16`, computed in double
    and narrowed once. The eye is always somewhere in `[0, 16)`.
  * `Renderer::drawPass` translates each section by `(section.chunkX - originChunkX) * 16`,
    subtracted in `i32` before it becomes a float.
  * `Frustum::setOrigin` tells the culler to subtract the same origin, because the planes were
    derived from that same relative matrix. Without it the culler and the draw loop disagree about
    where the world is and visible sections get rejected.

All three must use the same origin, so all three derive it from the camera rather than passing it
around — a one-chunk disagreement between the eye and the section matrices is a 16-block shift of
the entire world that would only appear while moving.

Measured at the Far Lands with a 0.05-blocks-per-frame walk: the old path has a maximum error of 0.5
blocks and moves in 1-block steps; the new path is exact to the last bit of a float and steps by
0.05. The host `--fly` harness uses the same relative path so it is exercised under sanitizers.

**This is a deliberate deviation from the original, and the only one at these coordinates.** Java
a1.1.2 renders with absolute float coordinates and does jitter out there. The terrain is reproduced
bit-for-bit; the stutter is not, because it is an artefact of the original's renderer rather than
anything the world contains, and nothing about a save file depends on it.

**Getting there.** Walking to 12,550,824 at the free camera's sprint takes about six real days, so
the debug overlay's settings page has a teleport row: it opens the system keyboard prefilled with
the current position and takes `x y z`. It is on the settings page rather than on a button of its
own **because that page already owns the d-pad** — it is behind a SELECT chord and `handleInput`
consumes the d-pad and returns before the rest of the frame sees it — so the row costs no global
binding, and A and B stay free for survival mode. The text is parsed by
`core/util/coord_text.hpp`, which lives in core so it can be tested on the host; the 3DS side is
only the applet and the console.
