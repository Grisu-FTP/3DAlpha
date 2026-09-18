# The 3DS optimisation playbook

Everything here is a design constraint, not a later optimisation pass. Measured numbers replace the
estimates as each milestone lands — keep the "measured" column current.

## The hardware you are actually targeting

| | Old 3DS / 2DS | New 3DS / New 2DS |
|---|---|---|
| CPU | 2× ARM11 MPCore @ 268 MHz, VFPv2, **no L2** | 4× ARM11 @ up to 804 MHz, 2 MB L2 |
| App RAM | 64 MB (`APPMEMTYPE 0`); 80 MB via exheader | 124 MB (`SystemModeExt: 124MB`) |
| Cores available | core0 + ~30 % of core1 (`APT_SetAppCpuTimeLimit(30)`) | core0, core1 slice, **core2 exclusive** |
| VRAM | 6 MB, higher bandwidth than FCRAM | same |
| GPU | PICA200 @ 268 MHz | same |

PICA200 facts that dictate the design:

- Programmable **vertex** and **geometry** shaders. The fragment stage is **fixed-function**: six
  TEV combiner stages, no fragment shader. **All lighting must be baked into vertex colour.**
- 3 usable texture units (the fourth is procedural). Textures must be Morton-tiled (swizzled).
- Texture formats: RGBA8, RGB8, RGBA5551, RGB565, RGBA4444, IA8, I8, A8, IA44, I4, A4, **ETC1**,
  **ETC1A4**.
- Colour render buffers: RGBA8, RGB8, RGBA5551, **RGB565**, RGBA4444. Depth: 16 / 24 / 24+8.
- Hardware fog with a **128-entry LUT**.
- Up to 12 vertex attributes; byte/ubyte/short/float components; attributes may sit at byte-granular
  offsets inside a vertex.
- The top screen framebuffer is **240×400** (physically rotated), so projections use the `*Tilt`
  variants: `Mtx_PerspTilt`, `Mtx_PerspStereoTilt`.
- **No occlusion queries.** Visibility is a CPU problem.

ARM11 has VFPv2 but **no NEON**. Hand-vectorising is not available; keep hot loops branch-light and
cache-friendly instead.

---

## 1. Vertex format: 12 bytes, 4 vertices per quad

craftus_reloaded uses a 16-byte vertex and 6 vertices per quad — **96 bytes per quad**. The shipped
format is 12 bytes and 4 vertices per quad — **48 B/quad, a 2x cut** in vertex bandwidth and
vertex-shader invocations. On a GPU-bound device this is the largest single win available.

```cpp
struct WorldVertex {      // 12 bytes
    int16_t u, v;         // atlas coords, 1/16384 units
    uint8_t x, y, z;      // position within a 16^3 section, 0..16
    uint8_t face;         // face index 0..5; reserved for the geoshader path
    uint8_t r, g, b;      // static: face shade x AO -- never time-dependent
    uint8_t light;        // (skyLevel << 4) | blockLevel -- see section 1b
};
```

It was 10 bytes through M0, with light baked into `r,g,b`. Section 1b is why it grew, and why 12
rather than 11: the GPU fetches the `s16` pair from an address that must stay 2-byte aligned, and an
odd **stride** puts every second vertex's UV on an odd address no matter where the field sits. The
byte that padding would have wasted became the face index the geometry-shader path needs.

Field order **is** the memory layout: citro3d's `AttrInfo_AddLoader` takes no offset, it accumulates
sizes in declaration order. The section origin goes into the MVP uniform, so positions stay
byte-sized. PICA byte and short attributes arrive as raw values converted to float — **not
normalised** — so the shader scales them.

Draw with a **single global, immutable index buffer** holding `0,1,2, 0,2,3, 4,5,6, 4,6,7, ...`:

```c
C3D_DrawElements(GPU_TRIANGLES, quads*6, C3D_UNSIGNED_SHORT, sharedIndices);
```

Every chunk draw reuses it, so **chunk meshes carry no index memory at all**. Sized for the
worst-case section (12,288 quads) it costs 144 KB of linear memory, once, for the whole process.

### Why not 8 bytes

The original 8-byte plan put UVs in `uint8` at 1/256 units. It does not work: a 16x16-tile atlas
needs `u = 256` for the right edge of the last tile and a `uint8` stops at 255. Insetting tiles to
fit loses a texel ring off every block texture, which is unacceptable for pixel art.

Two routes back to 8 bytes, both deferred until there is a mesher to benchmark against:

- **Tile index + corner** — one byte selects the atlas tile, two bits the corner, and the shader
  reconstructs the UV. Costs roughly six extra vertex-shader instructions (`mul`/`flr`/`add`) per
  vertex, trading ALU for bandwidth. 32 B/quad.
- **Lightmap instead of baked RGB** — see below; frees three bytes but spends two.

## 1b. Day/night: a lightmap, not baked vertex colour

Baking light into the vertex colour means **a day/night change invalidates every mesh**, because
Alpha dims sky light across the day cycle. Re-meshing the loaded world at dusk on a 268 MHz CPU is
not viable, and this is not a corner case — it happens twice per 20-minute Minecraft day, forever.

Minecraft's own answer is a lightmap texture indexed by (block light, sky light), sampled per
fragment, so only the lightmap changes with time. The PICA can do exactly that: three texture units
and six TEV stages are ample, and the terrain pass uses two of each.

```
stage 0:  atlas x primary colour     (face shade x AO -- all static)
stage 1:  previous x lightmap        (time-dependent, 16x16 texels)
```

**It costs one byte per vertex, not a second UV pair.** Both light levels are nibbles, so the vertex
carries `(sky << 4) | block` and the vertex shader reconstructs the texture coordinate. There are no
integer ops in the PICA vertex ISA, so the nibble split is arithmetic:

```
mul r1.x, div16, in_rgb.w     ; light / 16
flr r1.y, r1.x                ; sky   = floor(light / 16)
mul r1.z, mul16, r1.y         ; sky * 16
add r1.w, in_rgb.w, -r1.z     ; block = light - sky * 16
```

Four instructions and one byte, against four bytes for a second `s16` UV pair. `flr` and source
negation both assemble under the installed picasso — verified before the format was committed.

Time of day then costs **256 texel writes per change and zero mesh rebuilds**. The lightmap is 1 KB
in RGBA8, small enough to stay resident in the texture cache regardless of what else the frame does.
Sampling it with `GPU_LINEAR` gives the smooth gradient between light levels for free; wrapping must
be `GPU_CLAMP_TO_EDGE`, or light level 15 wraps round to 0 at the texture edge.

### Measured: the lightmap is free (M0b, decided)

The threshold table below was written before any data was taken. The result came in far outside it:

| Layers | A/B rounds | lightmap ON | lightmap OFF | delta |
|---|---|---|---|---|
| 8 | 93 | 4230 us | 4230 us | **-25 ps/fragment** |

The delta is **negative**, which no real cost can be, and it collapsed monotonically toward zero as
rounds accumulated (-750, -250, -130, -25). That is the signature of a quantity whose true value is
zero: a genuine per-fragment cost would settle on a stable positive number instead of shrinking with
sample count. The residual is 60x below the "free" threshold.

**So day/night costs one byte per vertex and nothing per frame.** The second texture fetch is
absorbed entirely — the fragment pipeline is bound on framebuffer write bandwidth and rasterisation,
not on texture fetch, and both textures (16 KB atlas, 1 KB lightmap) are small enough to stay
resident in cache. The vertex format is settled at 12 bytes and the mesher can be written against it.

One caveat recorded honestly: the overdraw quad is a single flat surface, so its lightmap samples
land in a narrow region of the texture and the cache hit rate is near-ideal. Real terrain samples the
lightmap more widely. The whole texture is 1 KB — smaller than L1 — so this is very unlikely to
matter, but if the mesher ever disagrees with this result, this is the assumption to re-test first.

### What this trades away, and the rule that was used to judge it

The lightmap adds **one texture fetch per fragment** on a device that is fill-rate bound. That is the
real cost, and it is paid on every pixel of every frame, whereas the re-mesh cost it avoids is paid
only at dawn and dusk. So it has to be measured rather than assumed.

The M0b probe (`X` toggles the lightmap, `Y` runs a controlled-overdraw pass, `L`/`R` set the layer
count) reports GPU time from the hardware's own timer, so vsync cannot hide the cost the way
wall-clock frame time would. The stress pass draws a screen-filling quad N times with the depth test
off, making the fragment count exactly `N x 400 x 240` — so the difference between lightmap on and
off, divided by that, is the per-fragment cost with nothing else in it.

The threshold, fixed in advance so the data could not pick it:

| Delta per fragment | Verdict |
|---|---|
| **under ~1.5 ns** | Take the lightmap. At a stereo frame of ~600k fragments that is under 1 ms, well inside a 16.7 ms budget. |
| **1.5 – 4 ns** | Take it, but the fill-rate knobs in §6 stop being optional on Old 3DS. |
| **over ~4 ns** | Too expensive. Fall back to a per-section light uniform via a TEV constant colour — day/night survives, smooth lighting does not. |

### Validated on hardware (M0)

Three interleaved attributes at a 10-byte stride (`GPU_SHORT` x2, then `GPU_UNSIGNED_BYTE` x3
twice), `BufInfo_Add` permutation `0x210`, `C3D_DrawElements` against the shared index buffer, and
TEV stage 0 modulating texture against primary colour. The stride is 12 bytes from M0b onward, with
both byte attributes widened to four components.

## 2. Geometry-shader quad expansion — **built, drawing, and the default**

`picasso` supports `.gsh point c0` geometry shaders, usable with both `C3D_DrawArrays` and
`C3D_DrawElements`. Submit **one 8-byte vertex per quad** and let the geoshader `setemit`/`emit`
four vertices plus the primitive: **8 B/quad, 12× less than craftus.**

Cost: geoshader mode occupies the primitive engine, dropping vertex-shader parallelism from four
units to three. §1 is the baseline that must work regardless.

This now exists. `mesh::QuadVertex`, `shaders/quad.v.pica`, `shaders/quad.g.pica`, a third pipeline
in `renderer.cpp`, and a switch on both harnesses (`--mesh <world> quads`, `--fly <world> d f
[switch] quads`) and on the debug settings page. **It draws the whole render distance on hardware**,
after five launches spent finding out why it did not — see *The first hardware run, and the hang*
below. The M2 gate measurement it was built to take is now takeable and has not been taken.

**It is what the game boots into.** `DebugSettings::geometryQuads` defaults to true and `main.cpp`
applies it once before the first frame, which is free there and nowhere else — the pool is empty and
the streamer has published nothing, so the re-mesh a format change normally forces has no work to do.
"Off by default" was the right answer for exactly as long as the path hung the GPU; it does not, and
the way back is still there and still automatic (`quadDrawsStopped_`).

**The host harness keeps its explicit `quads` word** rather than following the console's default.
Every measured number in this document was taken from the unadorned invocation, and silently changing
what `--mesh <world>` means would invalidate all of them at once.

**The 12-byte path stays, and it is a baseline rather than a legacy.** It is the watchdog's fallback,
it is the only encoding that can carry per-corner light or a biome tint — see the three things the
8-byte format gives up, below — and it is what `tests/quad_format_test.cpp` checks the geometry
shader's corner basis against.

### What is in the 8 bytes

```
u8 x, y, z      the block cell inside the section, 0..15 -- a cell, not a corner
u8 face         0..5, which selects the corner basis, the UV orientation and the shade
u8 tileX, tileY the atlas tile, pre-split so the shader does not divide by 16
u8 light        (sky << 4) | block, exactly as the 12-byte format carries it
u8 ao           reserved, zero
```

Against 4×12 bytes of vertex **plus six 2-byte indices** — 60 bytes a quad, so the traffic ratio is
**7.5×**, not 6×. The index buffer disappears entirely: `C3D_DrawArrays(GPU_GEOMETRY_PRIM, …)`.

Three things the format gives up, and they are the reason it is a second format rather than a
replacement. **Per-quad colour** — face shade comes from a uniform table indexed by the face, which
is exact for a1.1.2 (`Block.colorMultiplier` is white for every block in the game) and wrong for any
version with biome tint. **Per-corner anything**, so smooth lighting and AO cannot ride on it.
**Sub-block geometry**, the same limit the 12-byte format has.

### How the corners are rebuilt, and why it cannot drift

`mesh::kFaceBasis` gives each face a `base`, two edge vectors `e1`, `e2`, and a `uvSign`. A corner is
`base + i*e1 + j*e2` over (i,j) = (0,0), (1,0), (1,1), (0,1), which is `kFaceCorner`'s own order;
tile-local UV is `(i, (1-uvSign)/2 + j*uvSign)`. A `static_assert` rebuilds `kFaceCorner` and
`kFaceCornerUV` from that basis for all six faces and all four corners, so a wrong sign fails the
build rather than turning a face inside out on hardware. `renderer.cpp` uploads the same table as
the shader's `faceBasis[18]`, so there is no second copy of the numbers.

The vertex shader transforms the base point and both edges (12 `dp4`); the geometry shader adds
them. That is exact, because a projection is linear and the edges carry `w = 0`.

**The emission order is a strip, not the corner loop.** The primitive buffer holds three vertices,
so a quad is corners 0, 1, 3, then 2 over the top of slot 0 with `inv prim`. That splits the quad
along the other diagonal from the index buffer's `0,1,2 / 0,2,3`; both triangulations are the same
surface because a cube face is planar, and `tests/quad_format_test.cpp` checks all four triangles
come out counter-clockwise.

### Two things picasso will not let you write

Both cost an instruction to work around and neither is in any example:

- **Only one uniform per instruction, as src1.** `add r1.xyz, in_pos, faceBasis[a0.x]` does not
  assemble; the operands have to be the other way round.
- **Only one input register per instruction.** In the geometry shader, `add r0, v0, v1` does not
  assemble either — one of them goes to a temp first.

And one that is not settled: **whether `a0` is readable in the instruction straight after a `mova`.**
picasso inserts nothing — the assembled vertex shader is exactly the 44 instructions the file
writes, counted out of the shbin — so if the hardware wants a delay, nothing supplies one. The
lightmap unpack was moved to sit between the two, six instructions that depend on neither, which
costs nothing and removes the question.

### Instruction counts, out of the assembled shbin

| shader | instructions | runs |
|---|---|---|
| `world.v.pica` (§1) | 22 | per vertex, four per quad |
| `quad.v.pica` | 44 | per quad |
| `quad.g.pica` | 29 | per quad |

So the geometry-shader path executes 73 instructions a quad against 88, and **where** they run is
what decides it: the vertex half is divided across three shader units and the geometry half is not
divided at all. That is the whole risk in this design, and it is not something host measurement can
answer.

### Measured on the host, over the 1,118-column world

Quad counts are identical to the last quad — 2,133,406 cube quads, 55,846 opaque and 102,825
translucent detail — which is the check that this is a substitution and not a change.

| | 4 × 12-byte | geoshader | |
|---|---|---|---|
| world geometry | 107.34 MB | **25.96 MB** | 4.13× |
| per column | 98.3 KB | **23.8 KB** | |
| cube range alone | 97.66 MB | 16.28 MB | 6× |
| mesh time a section, best of five at `-O3` | 31.6 µs | **28.7 µs** | fill 11.2 → 10.8, emit 20.9 → 18.4 |

The whole world drops 4.13× and not 6× because **the 16-byte detail format is untouched and is now
37 % of the bytes.** Fluid was 8.9 % of geometry under the old format; it is the larger half of the
problem under this one.

The pool stops being the constraint. `--fly`, 1,200 frames, same three phases:

| | peak, 4 × 12-byte | peak, geoshader | evictions |
|---|---|---|---|
| o3DS distance 6, 12 MB | 8.24 MB | **1.66 MB** | 0 → 0 |
| o3DS distance 8, 12 MB | 11.15 MB (at the ceiling) | **3.33 MB** | **1,014 → 0** |
| n3DS distance 10, 32 MB | 26.13 MB | **5.94 MB** | 0 → 0 |

Distance 8 on an old 3DS was the configuration that ran at its ceiling and churned. It now holds
everything: **1,781 section meshes over the run become 928**, because the extra 853 were re-meshes
of sections that had been evicted. Settle frames do not move (123, 191, 277), which confirms again
that settling is bound by the streamer's two columns a frame and not by geometry.

Two consequences worth noticing. **The size-class sweep stops discriminating** — every ratio from
2.00 to 1.10 now fits distance 8 with zero evictions, so 1.15 is no longer load-bearing, though it
still wastes least. And **waste as a percentage goes up**, 7.6 % to 13.3 %, because the 2 KB
smallest class rounds up far more meshes when the average one is six times smaller; absolute waste
is still down, 8.2 MB to 3.4 MB, but `kSmallestSizeClass` is now the wrong number if this path
ships.

### The first hardware run, and the hang

New 3DS, debug page, cube format → geoshader. One or two chunks came back in the new encoding and
then the console died: both screens frozen, no HOME, held down to power off. That is not a slow
frame. `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` is the only unbounded GPU wait on the frame path, and
the main thread it blocks is also `aptMainLoop` — so a command list the GPU never completes takes
the whole application with it. Everything else in the frame refuses rather than blocks; the VBO pool
does so explicitly.

**The static half of the path was then verified end to end, against the built shbin and against
libctru's and citro3d's own source, and it is correct.** This is written down so it is not
re-derived:

| checked | result |
|---|---|
| DVLB layout | 2 DVLEs, `DVLE[0]` vertex and `DVLE[1]` geometry — the source order in `ctr_add_shader_library` holds |
| entry points | VS `main=0 end=49`, GS `main=49 end=78`, one shared 78-word blob, both stages uploaded |
| VS outmap | six `dummy` outputs → `outmapMask = 0x3F`, count 6 → `GPUREG_VSH_OUTMAP_TOTAL1/2 = 5`. `DVLE_GenerateOutmap` counts a dummy output before its `default: continue`, so dummy is the right declaration and the count is right |
| GS outmap | position, texcoord0, texcoord1, colour; `mergeOutmaps = 0`, so libctru uses the GS outmap alone — which is complete, since the GS emits every semantic the rasteriser needs |
| gsh mode | `GSH_POINT` |
| gsh stride | 6 → `GPUREG_GSH_INPUTBUFFER_CONFIG = 0x08000005`; six registers is the VS's six outputs, i.e. one input vertex per invocation. devkitPro's own `geoshader` example uses the same convention (two outputs a vertex × three vertices a triangle) |
| gsh permutation | libctru's identity default, `0x76543210 / 0xFEDCBA98` |
| `setemit` encodings | disassembled out of the blob: `vtx=0`, `vtx=1`, `vtx=2 +prim`, `vtx=0 +prim +invert`. The strip order and `inv prim` assemble as written |
| uniform allocation | `mvp` c0-c3, `fogparam` c4, `faceBasis` c5-c22, first `.constf` at c95 — nothing the per-bind `faceBasis` writes can land on |
| `mova` → `a0` | seven instructions separate them in the assembled code |
| command reserve | a quad bind plus a draw is about 430 words worst case against a 1024-word reserve |

So the fault is not visible by reading, and it has to be bisected on hardware. What that bisect has
established, in three launches:

1. **The probe draws and the GPU comes back.** One draw, six quads, one eye. The shader and the
   pipeline are right on hardware.
2. **The game wedges on four draws, 1,701 quads, zero command-list splits.** So neither the split
   guard nor the command buffer is involved. That is a very small frame — four sections — which is
   exactly what the first frames after a format switch hold, the mesh budget being four sections.
3. **It wedges just as hard on two draws and twenty-eight quads.** The first ramp pinned the cube
   pass to one draw of sixteen quads and the frame still went out with two draws, because the ramp
   limited only the cube pass. So the size of a geoshader draw is not the variable and neither is
   how many of them there are — the rung that died had a `DrawElements` through the detail pipeline
   sitting immediately behind the geoshader `DrawArrays`, which is a thing the probe has never done.
   Reading a ramp backwards only works if the ramp actually varies the thing that matters; this one
   did not, and the cost of finding that out was one launch.
4. **A watchdog that falls back to a blocking wait is not a watchdog.** The first version spent its
   deadline and then called `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` anyway, on the reasoning that there
   was nothing else to do; a wedged GPU never returns from that either, so the console still died
   with nothing on screen. It now opens no frame at all and the caller draws nothing, which keeps
   input, ticks and the bottom screen alive. The bottom screen is the report channel precisely
   because `consoleInit` leaves it single-buffered and it needs no GPU to update.

4. **It is a non-geoshader draw following a geoshader one.** With the detail and translucent passes
   skipped, the quad path drew the whole render distance — every geoshader draw unlimited, one
   after another — and the GPU kept up. Allowing the detail passes back in wedged it immediately,
   with the cube pass pinned to a single sixteen-quad draw. So it is not the shader, not the size of
   a draw, and not how many of them there are.

**The mechanism, and the fix.** Turning the geometry stage off repartitions the shader units —
`GPUREG_VSH_COM_MODE`, and `GPUREG_GEOSTAGE_CONFIG` with it — and `shaderProgramConfigure` writes
those registers straight into the command list. Nothing drains the pipeline first, so the
repartition lands while the previous draw's vertices are still in flight. No further register write
can help, because a register write is only another command behind the ones already queued.

Ending the list does help. `C3D_FrameSplit` hands what has been recorded to the GPU and starts a new
one, and the GPU finishes the first before it begins the second — so the shader-unit change in list
two happens after every draw in list one has retired. That is the drain, and it is the only one
reachable from outside citro3d. `Renderer::drawEye` now splits on **both** sides of the cube pass
whenever the quad format is live: on the way in because the second eye's bind turns the geometry
stage *on* behind the first eye's detail draws, and on the way out because the detail pass's bind
turns it off behind the geoshader draws. The outgoing split is skipped when the pass emitted no
geoshader draw, since then there is nothing to drain.

Cost: two extra command lists per eye, so two or four a frame. That is the price of the format
working at all, and it is charged only while the format is selected. The Info page shows them as
`geo`, beside the command-buffer figure below.

**Proved in both directions, in one launch.** The ramp was turned round to test the fix rather than
the fault — split on at one 16-quad draw, split on with no limit at all, then split off. It drew the
whole render distance through the first two, 159 draws and 78,758 quads a frame, and wedged on the
third the moment the split was taken away. So the drain is sufficient at full scale and its absence
is what was killing the console; neither half of that is inferred.

Two things about the instruments, worth keeping for the next path that programs the GPU differently:
**a ramp only answers about the axis it varies** — the first one left the detail passes unlimited,
so its smallest rung still contained the fatal draw and reported a hang at "1 draw, 16 quads" that
had nothing to do with either number. And **`C3D_FrameSync` is a VBlank wait, not a GPU wait**: it
is the frame-rate limiter, it always returns, and the watchdog keeps it. Only the queue drain that
follows it can wedge, and only that is under the deadline.

- **A geometry-shader cube in the probe** (hold SELECT at boot, then LEFT). Six quads, one draw, one
  eye, no streaming, no pool, no command-list split. **It draws, and the GPU comes back.** So the
  shader and the pipeline are both right on hardware and the hang is something the game's draw loop
  adds on top of them — which also settles §2's one open question, `a0` after `mova`: seven
  instructions of separation is enough. See the prediction table in `docs/status.md`, where this one
  launch struck out every row but the hang itself.

  Its texture does not match the 12-byte cube and cannot: `quad.v.pica` divides tile coordinates by
  16 because the game's atlas is 16 tiles to a side, and the probe's is four, so a tile index here
  selects a sixteenth of the probe's atlas rather than a quarter. `tileX` steps by two across the
  six faces so they come out green, green, brown, brown, grey, grey — a flat one-colour cube could
  not tell "the tile fields reached the shader" from "they were ignored".
- **Four compile-time knobs** at the top of the anonymous namespace in `renderer.cpp`, every one of
  them inert unless the mesh being drawn is a quad mesh: `kGeoMaxSections`, `kGeoMaxQuads`,
  `kGeoCubePassOnly`, `kGeoForceMono`.
- **`kGeoSplitPasses`, the fix above, as a switch** — so its absence stays measurable rather than
  becoming an unexplained `C3D_FrameSplit` nobody dares remove.
- **A ramp, `kGeoRamp`, which is those knobs walked automatically.** A wedged GPU stays wedged for
  the session, so a bisect cannot walk *down* from a hang — there is only ever one hang and it is
  the last thing that happens. It has to walk up and be read backwards: start below anything that
  could plausibly break, loosen one limit a second, and the rung being held when the GPU stops is
  the one that did it. Quads are ramped first with the draw count pinned at one, then the draw count
  with the quads unpinned, because those are the only two axes left between the probe and the game.
  Each rung is announced on the bottom screen through `geoTrace` as it is entered, so the answer
  survives a console that never draws anything again. The ramp advances only after a
  `C3D_FrameBegin` that returned, which is the only proof the previous list completed.
- **A watchdog on `C3D_FrameBegin`**, again only under the quad format. It spends a two-second
  deadline on `C3D_FRAME_NONBLOCK` before falling back to the blocking wait, and on expiry it stops
  issuing quad draws and `main.cpp` puts the cube format back to the one that is known to draw. It
  cannot un-wedge a GPU that is genuinely gone — nothing in the process can — but it turns a silent
  death into a `GPU STALL` counter on the debug page, and it does rescue the softer failure where
  the queue is merely backed up.
- **The defect found by reading, fixed regardless.** `MeshRanges::detailOffset()` returned
  `cubeBytes`. At 48 bytes a quad that is always a multiple of 16, so the detail and translucent
  vertex buffers always started 16-byte aligned; at 8 bytes a quad an odd quad count put their base
  at 8 mod 16 — an alignment the 12-byte path cannot produce, so nothing about the 12-byte path
  working says it is safe. The cube range is now padded up to 16, `total()` covers only the ranges
  that have content so a cube-only section is charged nothing, `copyTo` zeroes the pad, and
  `tests/quad_format_test.cpp` checks both offsets in both formats over odd and even quad counts.
  Meshing the same world both ways still gives identical quad counts, which is the check that this
  moved bytes and not geometry.

### The second hardware failure: an exception, not a hang

The drain fixed the wedge and the format then ran. What it did not fix was reported next: **at high
load it still killed the console — but as an exception screen or a reboot, not as a freeze.** That
is a different animal. A wedge is the GPU never finishing a list, and the watchdog exists to catch
it; an exception is the *CPU* taking a data abort, and the watchdog has nothing to say about one.
The reported triggers were the 3D slider up, a large render distance, and the farlands with the
slider at zero. Each of those is a term in the same product.

**The bound is the command buffer, and the guard that was supposed to enforce it could not.**
`GPUCMD_Split`, disassembled out of `libctru.a` rather than assumed:

```
gpuCmdBuf     += gpuCmdBufOffset;   // advance past what was recorded
gpuCmdBufSize -= gpuCmdBufOffset;   // and shrink the size by the same amount
gpuCmdBufOffset = 0;
```

Free space is `size - offset`. Before a split that is `size - offset`; after it, `(size - offset) -
0`. **A split reclaims nothing.** It hands the recorded words to the GX queue and starts a new list
inside the space that was already left. So `splitIfCommandBufferIsFull` flushed a list when a
section would not fit, bought not one word, and then recorded the section anyway — straight past
the end of the 1 MB `linearAlloc`, into the linear heap. Nothing surfaces at that moment: the fault
appears at the *next* `linearAlloc` or `linearFree` walking a smashed free list, which is an
exception screen somewhere else entirely.

**Why the geometry-shader format is what reaches it.** A section draw costs about **43 command
words** — a buffer-info bind (6), four dirty float uniforms in the PICA's 24-bit packing (15), and
eleven register writes for the draw (22), counted out of citro3d's disassembly. At
`kCommandBufferBytes` = 1 MB that is 262,144 words, or **6,096 draws in a frame**. The draw count
is bounded by what the VBO pool holds, and a quad mesh is 8 bytes where a 4-vertex mesh is 48 — so
the *same* 12 or 32 MB budget holds roughly six times as many sections. The farlands is where
almost every section is dense and non-empty; stereo doubles the passes; render distance squares the
column count:

| render distance | sections in range | stereo cube draws if all are non-empty |
|---|---|---|
| 8 | 2,312 | 4,624 |
| 12 (New 3DS play ceiling) | 5,000 | **10,000** |
| 24 (debug page ceiling) | 19,208 | **38,416** |

In the 4-vertex format those farlands sections are six times larger, the pool refuses most of them,
and the draw count never approaches 6,096. That is the whole of why this reads as a geoshader bug
and is not one.

**The fix is a real bound, not a bigger buffer.** Sizing the buffer up only moves which view breaks
it, and the ceiling has to exist somewhere regardless. `drawPass` now checks
`commandWordsFree()` before each section and, when there is not enough for one, stops drawing for
the rest of the frame — every later pass included, since nothing refills the budget until
`C3D_FrameBegin`. The result is holes in the world, far ones first (the cube and detail passes walk
near to far), and `droppedSections` counted on the Info page. **Below the ceiling it cannot fire at
all**, so a non-zero reading is never noise.

The Info page's `splits` row became `cmd`: words used and the percentage of the budget they are. A
view sitting near 100 % is a view about to start losing geometry, and that percentage — not a
guess — is what `kCommandBufferBytes` should be raised against.

### The third hardware failure: the watchdog was watching the wrong frame

Reported next: switching the cube format back and forth a few times, in the farlands, and then the
console died — top screen black, no exception dump, no HOME, nothing on the bottom screen either.
**The absence of a `geo:` line is the whole clue.** `geoTrace` needs no GPU and no completed frame,
so a watchdog that fires always leaves one; a death with a silent bottom screen is a main thread
that never reached the watchdog at all.

It did not, and the reason is one line:

```cpp
if (cubeFormat_ != mesh::CubeFormat::Quads && !quadDrawsStopped_) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);   // unbounded
    return true;
}
```

**`C3D_FrameBegin` waits for the *previous* frame, and that branch tested the *next* one.** The wait
is `gxCmdQueueWait(queue, -1)` — disassembled out of `renderqueue.o`, where `C3D_FRAME_SYNCDRAW`
resolves the timeout argument to `-1` and `C3D_FRAME_NONBLOCK` to `0`, which is the only difference
between them and the whole basis of the watchdog. The queue it waits on holds what the frame before
recorded. `cubeFormat_` describes what the frame about to be recorded will hold. Toggling the debug
page back to 4-vertex flipped that field instantly, so the very next `drawFrame` took the unbounded
wait on a queue still full of geoshader draws — and if one of those was the list that wedged, the
main thread, which is also `aptMainLoop`, never came back. The watchdog was two lines further down
and was never reached.

Switching the format back and forth is exactly how a player reaches it, and the farlands is where
the frame is heavy enough for the wedge to be there waiting.

The fix is to gate the wait on what was actually recorded. `Renderer::geoWorkInFlight_` is set where
`C3D_DrawArrays(GPU_GEOMETRY_PRIM, ...)` is recorded and cleared only where the queue is *proven*
empty — a `C3D_FrameBegin` that returned, or a drain that did — so it survives the toggle for as
long as the draws do.

**And the same mistake, in three more places.** `C3D_FrameSync` is a VBlank wait, not a GPU one; it
was already written down here and the three callers that mattered still read as though it were a
drain:

```
C3D_FrameSync:
    ldr r6, [r4]              @ frameCounter[0]
    ldr r5, [r4, #4]          @ frameCounter[1]
    bl  gspWaitForAnyEvent
    ...                       @ loop while neither has moved
```

`Renderer::rebuildChunks` — which is what a format switch *is* — called it and then `linearFree`d
the whole VBO pool, 12 to 32 MB, and immediately reallocated and re-meshed into it. On any frame
that outruns the refresh, which is the whole of the far-from-origin case, the GPU was still fetching
vertices out of that memory; on a frame that never finishes it always is. `Renderer::setAtlas` freed
the block atlas the same way, and `Renderer::shutdown` freed the pool, the index buffer and all
three shader programs the same way — the last of those handing the top screen to a menu whose own
`C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` has no watchdog behind it at all.

All three now use `drainGpu`, which is the only drain the public API can express: poll
`C3D_FrameBegin(C3D_FRAME_NONBLOCK)` on a deadline and close the frame it opens on success. That
close is free — `C3Di_SplitFrame` finds an empty list, and `C3D_FrameEnd` transfers only targets a
`C3D_FrameDrawOn` marked `used`, which an empty frame has none of.

Two things worth keeping from this one. **A watchdog is only as good as the predicate that reaches
it** — this one was correct in every way except which frame it asked about, and that made it absent
exactly when the format was being changed, which is the one action the whole feature exists to
offer. And **`C3D_FrameSync` is not a drain**: it was disassembled, written down, and then three
callers went on trusting the name. The name is the trap; `drainGpu`'s comment carries the
disassembly so the next reader does not have to take it on faith.

### The fourth hardware failure: three ways the third fix stopped one call short

Reported after all of the above shipped: at the Far Lands, with the geometry-shader format live and
a large render distance, the console still died — **as a freeze with HOME dead, and separately as an
exception screen** — and neither left anything on the card. No Luma dump under `3dsx_app/`, no
`3dalpha-oom.txt`. The dump handed over with the report turned out to be `005`'s
`crash_dump_00000009.dmp` byte for byte, picked back up off the card; `cmp` says so. So there was no
new evidence in it and none was taken from it.

What could be settled without a console was settled first, and it cleared the ground:

| checked | result |
|---|---|
| the build on the card | current — `crashlogs/current-build.elf` matches `build/a1.1.2/*.elf` and carries every one of the new trace literals, so these deaths are genuinely post-fix |
| host core, ASan + UBSan | 674/674 |
| host core, TSan | 674/674, zero races |
| `--fly w 12 1500 gen`, `--fly w 12 800 flip` | clean under ASan, generation and format switch both |
| the command budget, against libcitro3d's disassembly | correct — `C3D_Init` 16-aligns the size and `linearAlloc`s exactly that many bytes (**no doubling**), storing `size/4` words; `C3D_FrameBegin` reloads `gpuCmdBuf` and `gpuCmdBufSize` from the context and zeroes the offset. `kCommandBufferBytes / 4` a frame, refilled only there, is the right model and `commandWordsFree()` measures it |

Which leaves `src/platform/ctr/`, the layer no test reaches. Three defects were in it, and all three are
in the fix for the third failure rather than anywhere new.

**One: `geoWorkInFlight_` was cleared on a drain that failed.** All three teardowns did

```cpp
drainGpu(kGpuDrainSeconds);   // returns false if the deadline expired
geoWorkInFlight_ = false;     // ...cleared anyway
```

`geoWorkInFlight_` exists to answer "could the queue still hold a geoshader draw", and it is the gate
on the unbounded `C3D_FRAME_SYNCDRAW`. Clearing it after a drain that has just reported the GPU did
**not** come back tells the next `drawFrame` the queue is empty on exactly the evidence that says it
is not — and that frame then takes the unbounded wait on the list that wedged. **That is the third
failure, re-armed inside its own fix.** It is now cleared only on the branch where the drain
returned true.

**Two: the watchdog's deadline was chosen from the session's history, not the GPU's state.**

```cpp
const float seconds = gpuStalls_ == 0 ? kGeoWatchdogSeconds : kGeoRetrySeconds;
```

`gpuStalls_` is never cleared, so one stall anywhere in a session put **every later frame** on the
32 ms retry deadline — permanently, and including after `main.cpp` had dropped the cube format back
to the one that is known to draw. `kGeoRetrySeconds` was sized as "one frame's worth, long enough to
catch a GPU that comes back", which is right for a GPU believed wedged and wrong for a healthy one:
at the Far Lands a perfectly honest frame's queued work outlasts 32 ms. The watchdog then fired on
merely-slow frames, `drawFrame` returned having drawn nothing, and it did so every frame from then
on. **The top screen freezes on its last good frame and HOME still works** — which is a different
death from the one below, and telling them apart from the couch is one button.

The state and the history are now separate fields. `gpuWedged_` means the watchdog fired and no
frame has completed since; a completed frame is the only proof the GPU came back, so it clears
there and the full deadline returns with it. `gpuStalls_` stays the latched count the debug page
reports, and the `geo:` line is now printed once per *episode* rather than once per session, so a
second wedge after a recovery is reported too.

**Three: the menu's frame was the one unbounded GPU wait left in the binary.** `menu.cpp` opened
with `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` and nothing behind it. Every wait on the world's frame path
had been put under a deadline; the menu is *where a wedged GPU gets handed over*, and it was missed
because `Renderer::shutdown` drains before handing the screen across — so it reads as covered. It is
not: the drain has a deadline of its own, and **the case where that deadline expires is exactly the
case where the GPU is already gone.** A guard whose failure mode is the failure it guards against is
not a guard. `ctr::beginFrameBounded` is the same `C3D_FrameSync` plus polled `C3D_FRAME_NONBLOCK`
the renderer uses, exported for callers that own no `Renderer`; on expiry the menu opens no frame,
draws nothing, holds its last image, and keeps `aptMainLoop` and HOME alive.

`probe.cpp` keeps its plain `C3D_FRAME_SYNCDRAW` deliberately. It is the reference path — the thing a
hardware answer is measured against — and it is only worth that if it behaves like unadorned citro3d.

**`quadDrawsStopped_` also has a way back now, and it is a deliberate one.** It survives a run of
good frames on purpose: a GPU that missed a deadline is not one to hand a geoshader draw back to
because the next frame happened to come through. `Renderer::setCubeFormat` clears it, which is
reached both by `main.cpp`'s fallback selecting 4-vertex and by a player selecting geoshader again.
Either way the decision was made somewhere that can be reasoned about.

**None of this is measured, and the exception screen is not explained by any of it.** The freeze has
a mechanism and a fix; the exception is a CPU fault, the watchdog has nothing to say about one, and
with no dump written there is nothing to resolve. Luma's own screen prints the process name, the
exception type and the registers before anything reaches the card — that is the artifact to capture
next, and until one exists this section has three fixes and one open failure, not four fixes.

### The heap split, and the bound under the render distance

The fourth report came with `sdmc:/3dalpha-oom.txt` on the card, which settles what `005`, `006` and
`007` could each only narrow: **the console runs out of newlib heap.** Not linear, not the command
buffer, not the GPU. `crashlogs/007` explains why that arrives looking like two different deaths.

The line itself, kept because it is the only measurement of a real failure this project has:

```
OUT OF HEAP  free 4405k of 40960k  blocks 10688k  owed 1582k  pool 13425k
             cols 382  sect 1314  chunk -63 -2000007  geoshader
```

**Read it before believing the section below it.** Resident columns are `blocks 10688k` -- 10.4 MB
of the 35.7 MB in use, **29 %**. The grid was not what ran the heap out; at `cols 382` against the
729 that render distance 12 asks for, it had not even finished loading. What is left is the chunk
cache's clean side (capped at 8 MB), the generator's own cache (`16 * loadRadius + 96` columns, so
about 8 MB here), the 1.5 MB owed to the card, decoded sound, the map's patch cache, and allocator
overhead across tens of thousands of 2 KB nibble arrays. **Those together are the larger half, and
the reporter named none of them** -- `clean` and `gen` are on that line now for the next one.

So the honest split of what follows: **the heap was simply too small for what the game legitimately
uses**, and raising it is the fix for this crash. The column budget below is a real bound and a
necessary one, but it is a backstop for a case this report is not -- at 10.4 MB it would not have
fired. Sizing it was worth doing; claiming it prevents this would not be.

Note also `28.0 KB` per column here against the `21.7 KB` measured below. The measurement is of
*freshly generated* columns; these have been lit and ticked, and `NibbleArray::set` materialises a
plane that `assign` had collapsed and never collapses it again. **That is a candidate for `compact()`
after relighting** -- 6.3 KB a column, and the 0.0 % figure below does not cover it because the probe
never ran the relighter.

And `chunk -63 -2000007` is 32 million blocks out, two and a half times the Far Lands. The 1.55x
below is measured at the Far Lands proper; nothing here says what the cost is that far out beyond
the 28.0 KB this one line implies.

**What a column costs, measured through the real generator** rather than reasoned about -- 169
columns at chunk (0,0) and 169 at chunk (784426,0), broken down by plane:

| | ordinary | far lands |
|---|---|---|
| **per column** | **14.0 KB** | **21.7 KB** |
| blocks | 9.0 KB | 12.8 KB |
| sky light | 2.8 KB | 6.6 KB |
| block light | 1.1 KB | 1.2 KB |
| metadata | 0.0 KB | 0.0 KB |
| non-uniform sections | 763/1352 | 1078/1352 |

**1.55x, not the "several times" `006` predicted, and not for the reason it gave.** The palette is
not failing. Ordinary terrain is all-air above the surface and all-stone below, and a *uniform*
section costs nothing at all; the Far Lands simply has far fewer of them. That is a smaller effect
than guessed and it is still the whole problem, because the grid is `(2d + 1)^2` columns either way:

| render distance | columns | ordinary | far lands |
|---|---|---|---|
| 12 | 729 | 10.0 MB | 15.5 MB |
| 16 | 1089 | 14.9 MB | 23.1 MB |
| 20 | 1681 | 23.0 MB | 35.6 MB |
| 24 | 2401 | 32.8 MB | **50.9 MB** |

against a heap that was capped at **40 MB** and also holds code, stacks, the generator's own column
cache, the chunk cache and the map.

**Two ways to make a column cheaper were measured, and both are dead.** `compact()` recovers 0.0% --
nothing in the game calls it, which looked like a lead, but the generator already uses
`NibbleArray::assign()`, which collapses a plane to uniform on load, and metadata is never
materialised at all. A 1-/2-bit palette tier recovers 2%: the distinct-id histogram peaks at 5-8 ids
per section, and only 36 of 1352 Far Lands sections hold four or fewer. The representation is close
to optimal.

**The room was next door.** `gpu_memory.cpp` says so in its own comment: "The heap policy hands us
82 MB of it", and `vboBudget` caps the pool at 32 MB on a New 3DS and 12 on an Old one. Roughly
38 MB of linear memory sat idle while the heap beside it ran out. The split was `remaining / 3`
capped at 40 MB -- a fraction, chosen before either side had been measured.

It is now chosen from what linear actually needs: the pool's own budget plus a 16 MB overhead for
the command buffer, the index buffer, the atlas, the audio ring, the staging textures and
`vboBudget`'s own 8 MB refusal margin. The heap gets the rest.

| available | heap before | **heap after** | linear after | pool gets |
|---|---|---|---|---|
| 123 MB (New 3DS) | 40 MB | **75 MB** | 48 MB | 32 MB |
| 64 MB (Old 3DS) | 21 MB | **36 MB** | 28 MB | 12 MB |

Checked numerically across twelve sizes down to 16 MB before it was built: the pool keeps its full
budget in both real configurations, and no size panics that did not already panic under the old
policy. That mattered enough to check rather than reason about -- `__system_allocateHeaps` runs
before services are up and calls `svcBreak(USERBREAK_PANIC)` if it asks for more than exists, so a
mistake here is a console that does not boot and cannot say why.

**And a real bound under it, because a bigger heap only moves which view breaks it.** This is the
same lesson as the command buffer two sections up, and it is the second time this project has
learned it. `WorldStreamer::setMemoryBudget` puts a ceiling in bytes on the resident columns. Over
it, the admission radius shrinks a ring at a time and everything outside is dropped; comfortably
under it -- seven eighths, so a view sitting on the line does not oscillate -- it grows back. The
render distance becomes a request rather than a promise, and `Stats::admitRadius` reports which it
is, on the Info page as `admit N of N rings`, silent while the budget is not biting.

**The hysteresis is the part that is not decoration.** Evicting the outer ring without also refusing
to *admit* it is an eviction treadmill: the load path reads straight back what the budget just
dropped, for ever, freeing nothing and costing SD I/O. Both halves are the same radius.

The console gives columns five eighths of whatever heap the split produced -- deliberately
conservative, because too large is the crash this exists to stop and too small is only a shorter
view. Three tests cover it in `tests/streamer_generate_test.cpp`; the budget ships **off** on the
host so every documented `--fly` number keeps its meaning.

**What is still open**, and it is now the biggest thing here: the 26 MB of heap that the one real
report shows is *not* resident columns. The accounting above closes it to within about 8 MB by
reasoning, which is not the same as measuring it. `clean` and `gen` on the out-of-memory line are
what will settle the two largest terms, and until they come back from a console the split is sized
against an estimate rather than a measurement.

### What is still unknown, and it is the part that matters

**None of the above is the M2 gate.** The gate failed at 0.208 µs per quad of GPU time, and that
number can only be re-taken on hardware. What the host has shown is that the format costs nothing to
adopt — less memory, slightly less CPU, identical geometry — so the experiment is free to run.

On the console: settings page → cube format → geoshader, wait for the world to re-mesh, and compare
GPU draw on the Info page against the same view in the 12-byte format. Either answer is decisive.
The hang that blocked this is fixed; if `GPU STALL` ever appears on that row again, the number
beside it is not a measurement and the watchdog has already dropped the format back to 4-vertex.

- **If the pass gets materially faster**, the cost was vertex fetch and shading, and this becomes
  the path. The remaining vertex-side lever after it is a shorter shader.
- **If it does not move**, the cost is per-triangle setup — the triangle count is identical across
  both paths — and *nothing on the vertex side will ever help*. The only lever left is emitting
  fewer triangles, which means greedy meshing, and greedy meshing has a problem of its own: merged
  faces need the texture to repeat across the merged quad, and a 16×16 atlas with one wrap mode for
  the whole texture cannot do that. That would be the next thing to solve, and it would be a change
  to the atlas rather than to the mesher. **Now built** (docs/status.md §22): a 512×512 cube atlas
  of 3×3 tile repeats inside 8-texel gutters, a ⅛-pixel seam against the rasteriser's T-junction
  cracks, and 2.02× fewer cube quads on the real world. First hardware run: 34 → 22 ms GPU. Either answer to the measurement above is now worth less, because
  both halves of the cost shrink with the quad count.

## 2b. Take control of the heap split (implemented)

**The linear heap, not total RAM, is the binding limit on mesh memory** — the GPU cannot fetch
vertices from the newlib heap.

libctru commits the whole remaining application region at startup and splits it with
`HEAP_SPLIT_SIZE_CAP = 24 MB` and `LINEAR_HEAP_SIZE_CAP = 32 MB`, which on a memory-rich console
lands everything spare in the newlib heap. Measured on a New 3DS:

| | libctru default | 3DAlpha policy |
|---|---|---|
| newlib heap | 91 MB | **40 MB** |
| linear heap | 32 MB (at the cap) | **82 MB** (83,860 KB) |

Both columns are measured on hardware, not projected.

70 MB was sitting in a heap that meshes cannot use. `src/platform/ctr/heap.cpp` replaces the weak
`__system_allocateHeaps` with:

```
heap   = clamp(remaining / 3, 16 MB, 40 MB)
linear = remaining - heap
```

Newlib holds block data (~15 MB of palette sections at render distance 12), code, stacks and
scratch; everything beyond that is better spent on meshes. **This multiplies the mesh budget by 2.6×,
which is what sets maximum render distance.**

Note the 40 MB heap is `kHeapMax` clamping, not `remaining / 3` — on a 124 MB New 3DS the divisor
would have given ~41 MB anyway, so the two agree here. On a 64 MB Old 3DS the divisor governs
(~21 MB heap / ~43 MB linear), which is the intended behaviour: the small console gives proportionally
more to newlib because its render distance, and therefore its mesh pool, is smaller.

Why override the function rather than just setting `__ctru_heap_size` / `__ctru_linear_heap_size`
(also weak): those are fixed constants, and libctru calls `svcBreak` when they exceed what is
actually available — which differs between Old and New 3DS and between launch methods. Deriving the
split from the real figure is the safe form. The routine runs before services are up, so the model
cannot be queried; the policy reads the available memory instead.

Linear allocation steps down in 4 MB decrements if the address space cannot satisfy the full
request. A smaller mesh pool is a degraded game; a panic is no game at all.

## 3. Put hot VBOs in VRAM

VRAM (6 MB) has materially higher bandwidth than FCRAM, and the GPU fetches vertices from either.

Measured on hardware: two stereo top targets at RGBA8 + DEPTH16 cost **1,125 KB**, leaving
**5,019 KB of 6,144 KB free** — exactly `2 x 400 x 240 x (4 + 2)`, so the budget is predictable
arithmetic rather than guesswork. Adding a 320x240 bottom target costs another 450 KB; switching the
colour buffers to RGB565 saves 375 KB. So after render targets and the atlas, **~4.5 MB of VRAM is
available for vertex data** — more than the 3–4 MB originally estimated.

Allocate section VBOs **VRAM-first with a `linearAlloc` fallback**, prioritising the nearest
sections. craftus does not do this — it is free performance.

> ### Reversed on hardware: no VBOs in VRAM
>
> **The CPU cannot write VRAM.** A `memcpy` into a `vramAlloc`'d pointer takes a *permission fault
> on write*, and that is how this was found — the first console run that got as far as the render
> loop died at `0x1F38CA00` uploading a freshly meshed section, in `VboPool::upload`.
>
> This is a rule, not an accident of one build. citro3d's `C3D_TexLoadImage` range-checks its
> destination against `[0x1F000000, +0x600000)` and routes anything inside it through
> `C3D_SyncTextureCopy` rather than `memcpy` — visible in the disassembly of `libcitro3d.a`.
> `Atlas::init` already obeyed the rule, building the atlas in linear memory and letting
> `C3D_SyncDisplayTransfer` do the move. Only the VBO pool did not, because its allocator seam
> covered `allocate`, `release` and `flush` but not the write itself.
>
> So the claim above — "it is free performance" — is **false**, and the error was in the word free
> rather than in the bandwidth. A VRAM VBO costs a second copy into a linear staging buffer, a cache
> flush, a GPU round trip and a sync, *per upload*, on the path that re-meshes up to 58 sections in
> a frame while the player turns. Against a plain `memcpy` into linear, that is not a close call at
> streaming rates. The bandwidth win was never measured — the table at the end of this document
> lists VBOs-in-VRAM as "measure against linear-only" — so what goes is the unmeasured optimisation
> and not the measurement.
>
> `budget.vram` is 0 in `vboBudget`, and `GpuVboAllocator::allocate` refuses the VRAM tier
> outright; the pool already treats a refused tier as full and falls through. The tier itself stays
> in `core/render/vbo_pool` because the pool is tier-generic and the host tests cover both paths.
> **It becomes worth revisiting when meshes stop being restreamed** — geometry uploaded once, such
> as a static hub or a distant-terrain impostor, pays the staging cost once and keeps the
> bandwidth.

Pair it with a pooled allocator so 30 Hz mesh churn does not fragment either heap. craftus's
`VBOCache` is the right idea with the wrong data structure (an O(n) sorted vector scanned under a
lock); use size-class free lists.

**Built, and measured — `core/render/vbo_pool`.** Nothing in it sorts by distance: `VisibleSet::toMesh`
already arrives nearest first, so filling VRAM in arrival order fills it with the closest geometry.
Eviction is least-recently-drawn through an intrusive list, so touching a section is O(1) and so is
choosing a victim. A section drawn in the current frame can never be evicted; the pool refuses the
upload instead and the caller retries next frame, which is what stops geometry flickering out to
make room for geometry that is not on screen yet.

### The size-class ratio, measured rather than picked

Rounding each mesh up to a size class is what makes a freed block reusable, and what it costs is the
rounding. Swept over all **2,853 real section meshes** in the 660-chunk world, then re-run through
48 frames of turning on the spot at distance 8 against a fixed 12 MB budget — the configuration
where the pool runs at its ceiling:

| Ratio | Classes | Wasted | Geometry resident in 12 MB | Blocks recycled | Allocator calls |
|---|---|---|---|---|---|
| 2.00 | 10 | 45.6 % | 8.12 MB | 81.0 % | 551 |
| 1.50 | 15 | 23.6 % | 9.81 MB | 80.8 % | 546 |
| 1.33 | 21 | 16.6 % | 10.42 MB | 80.8 % | 545 |
| 1.25 | 27 | 13.3 % | 10.71 MB | 80.4 % | 558 |
| **1.15** | **42** | **7.7 %** | **11.15 MB** | **74.6 %** | **719** |
| 1.10 | 61 | 5.4 % | 11.41 MB | 71.8 % | 798 |

Re-measured once fluid geometry existed, which added 8.9 % to a real world's mesh bytes and put
them in a different vertex format. **The curve did not move**: 1.15 still wastes 7.7 % and powers of
two still waste 45 %, so the ratio was not overfitted to an all-cube world.

Two things came out of this that were not obvious in advance.

**Powers of two are much worse than the textbook third.** They waste 45 %, because section mesh
sizes are not spread evenly across each octave — they bunch. In a 12 MB pool that is 3.03 MB of
geometry thrown away, a quarter of the budget.

**The expected trade-off barely exists.** Finer classes should match a freed block to a new mesh
less often, and they do — but going from 2.00 to 1.15 costs six points of recycling rate and 168
extra allocator calls across 48 frames, three and a half per frame. Against 3.03 MB more geometry
on screen, that is not a trade at all.

**1.15 is where it stops paying.** 1.10 buys another 0.26 MB for half again as many classes and more
distinct sizes handed to `linearAlloc` — and external fragmentation over a long session is exactly
what this host measurement *cannot* see, so the conservative end of the flat part of the curve is
the right place to sit. The smallest class is 2 KB.

### What the pool does through a full turn

Three revolutions, 16 frames each, real 70° frustum, VRAM fixed at the measured 4.5 MB:

| Configuration | Peak resident | Held | Uploads / 48 frames | Recycled | Evictions | Refused |
|---|---|---|---|---|---|---|
| o3DS, distance 6, 12 MB | 7.69 MB | 8.29 MB | 379 | — | **0** | 0 |
| o3DS, distance 8, 12 MB | 11.15 MB | 12.00 MB (at the ceiling) | 2,829 | 75 % | 2,311 | 0 |
| n3DS, distance 8, 32 MB | 18.49 MB | 19.92 MB | 871 | — | **0** | 0 |
| n3DS, distance 10, 32 MB | 27.63 MB | 29.82 MB | 1,294 | — | **0** | 0 |

Both M2 gate configurations have room, and the New 3DS one has a great deal of it now that the gate
is distance 8: **o3DS at distance 6 and n3DS at distance 8 both turn on the spot without evicting
anything at all**, the latter at 19.92 MB of a 32 MB pool. Distance 10 also evicts nothing, but it
now holds 29.82 MB where before fluid it held 26.95. **It is still the configuration with the least
headroom left**, so it remains what the next render type to gain an emitter should be measured
against — memory headroom is a separate question from the frame-rate gate, and lowering the gate
did not make distance 10 any roomier.

Distance 8 on an old 3DS is the one that runs at the ceiling. It still never refuses an upload, and
75 % of its uploads are served from a recycled block with no allocator involvement — which is the
size-class free list doing precisely the job craftus's sorted vector does badly. What it costs is
re-meshing: 2,829 meshes over 48 frames is **59 per frame**, and at the measured 28.5 µs per section
that is an *estimated* ~1.7 ms of meshing per frame. **On the main thread**, not the worker: the
text said "worker-thread" and the worker has never meshed -- it generates, and meshing runs on the
main thread behind a per-frame budget (`docs/architecture.md`, *Threading model*). 1.7 ms of a 16.7
ms frame is tolerable where 1.7 ms of somebody else's core would have been free, so the number is
worth more attention than it was being given. Tolerable, and it is a harsh
stress — 22.5° per frame is a full revolution in half a second.

VRAM is fully used in every configuration, which is the intended outcome: it is the smallest and
fastest tier, so it should always be full.

That last paragraph and the "VRAM fixed at 4.5 MB" in the table header are **superseded** — the
tier is off on hardware, see the reversal in §3. The totals are not: every figure above is a
property of the pool at a 12 or 32 MB ceiling, and the ceilings are unchanged, so what moved is
only which allocator serves the bytes. Peak resident stays under both ceilings with linear alone.

Caveat, the same one as every other M2 figure: one world, one player position, on the surface.

### One sharp edge, found by fuzzing rather than by reading

Eviction notifications accumulate until the caller reads them; they are **not** reset per frame.
That is not fussiness. The last rung of the allocation ladder gives up geometry and can *still* fail
to place the mesh, so an upload that returns "no slot" may have evicted several sections on its way
there. Resetting the list each frame silently dropped exactly those, and a dropped notification
means the renderer goes on drawing from a slot that now belongs to a different section.

Every scripted test passed with that bug in place. A randomised one — 400 frames of mixed uploads,
touches and releases against a deliberately tight budget, checking after every frame that each
resident mesh is still byte-for-byte what was put in it — failed on the first run. On hardware it
would have surfaced as one chunk occasionally wearing another's geometry, which is about the worst
kind of bug to chase on a 240-line screen.

The rule that follows: **drain `evicted()` after every `upload`, whether or not it succeeded.**

## 4. Visibility: flood-fill graph culling, front-to-back

Tommaso Checchi's algorithm, as craftus implements it in `WorldRenderer.c`:

1. **At mesh time**, flood-fill the air volume of each 16³ section and record a 6×6 "can see from
   face A through to face B" relation — 15 unique pairs, stored as a `uint16_t`.
2. **At render time**, breadth-first search outward from the player's section. Step into a neighbour
   only if the entry/exit face pair is in the mask, the section passes the frustum test, and it has
   not been visited.

This is what makes caves cheap: underground you draw a handful of sections instead of hundreds.

Traverse **front-to-back** so the PICA's early-depth test (`C3D_EarlyDepthTest`) discards occluded
fragments. This game is fill-rate bound, so early-Z is worth more here than on a desktop GPU.

Translucent geometry is collected during the same walk and drawn afterwards in reverse order, with
depth writes off.

**Built.** It needs no second walk and no sort: the draw list is already breadth-first out of the
camera's own section, so walking it backwards *is* far-to-near, at section granularity — which is
what the original sorts at too. The mesher keeps translucent quads in their own range of the same
per-section allocation (`mesh::MeshRanges`), so the third pass costs one more walk of the draw list
and the state changes at its ends, not another allocation or another copy. Measured over the real
660-chunk world: **55,618 of the 87,244 detail quads are translucent**, all of it water, and total
mesh bytes are unchanged to the byte — the split is a reordering, not extra geometry.

Which blocks go in it is a1.1.2's own `getRenderBlockPass`, not a judgement: water and ice, and
**not** lava, which shares a class with water and is separated only by its material.

### Measured: the search has to drive meshing, not just drawing (M2)

Meshing all 660 columns of a real a1.1.2 world — every section of every loaded column, which is the
obvious thing to build first — gives **91.1 KB of vertex data per column**. That is 3.6x the ~25 KB
the *block* data costs, and it does not fit:

(That was measured when the mesher emitted cubes and crossed squares only. Fluid took it to
**99.4 KB**, so every figure in the table below is now 9 % low. The numbers are left as they were
measured, because the argument they were part of was reversed on other grounds — see the second
half of this section — and a superseded argument is more useful with its original numbers attached.)

| Render distance | Columns | Mesh everything loaded | Only what the search reaches |
|---|---|---|---|
| 6 | 169 | 15.0 MB | 4.0 MB |
| 8 | 289 | **25.7 MB** | 6.8 MB |
| 10 | 441 | 39.2 MB | 10.4 MB |
| 12 | 625 | 55.6 MB | 14.8 MB |

The o3DS VBO pool is ~12 MB (§Memory budget). Meshing everything loaded misses it at distance 8 by
more than double, and misses it at distance **6**. So this is not an optimisation to add later — a
renderer that meshes what happens to be loaded cannot ship at any useful render distance.

The reason is in the height distribution of that same world:

```
y   0- 15   31.1 %   ###############################
y  16- 31   15.9 %   ###############
y  32- 47   11.9 %   ###########
y  48- 63   14.4 %   ##############
y  64- 79   19.2 %   ###################
y  80- 95    7.1 %   #######
y  96-111    0.3 %
y 112-127    0.0 %
```

**73 % of all geometry is below y=64** — cave walls, ravine faces and the underside of the terrain,
almost none of which is ever on screen.

### Measured again: reachability is not the fix, and the first conclusion here was wrong

The obvious inference from the histogram — let the visibility BFS decide what gets a VBO, and skip
what it never reaches — was written into this document before it was measured. Measuring it killed
it. Walking outward from the player's actual position in that world, with the frustum left open
because a player can spin on the spot:

| Render distance | Mesh everything in range | Only what the walk reaches | Saved |
|---|---|---|---|
| 6 | 11.51 MB | 7.18 MB | 38 % |
| 8 | 20.79 MB | 17.67 MB | **15 %** |
| 10 | 31.61 MB | 25.77 MB | 18 % |
| 12 | 47.95 MB | 39.44 MB | 18 % |

Reachability barely helps, and the reason is obvious in hindsight: standing on the surface under
open sky, nearly everything *is* reachable. The air above the terrain is one connected volume that
touches every column, and alpha's cave systems connect to the surface in enough places that most of
the underground joins it. A test that only excludes sealed pockets excludes almost nothing.

### What actually bounds it: a budgeted pool, with the walk supplying priority

The number that matters is the **draw** set, not the reachable set — what one real frustum admits,
sampled every 45° around the compass so the figure is not one lucky facing:

| Render distance | Sections drawn | Geometry resident |
|---|---|---|
| 6 | 205–330 (mean 251) | 1.02–4.20 MB (mean 2.13) |
| 8 | 412–598 (mean 480) | 2.20–7.72 MB (mean 4.64) |

That fits. A ~12 MB VBO pool holds the worst facing at distance 8 with room to spare, and enough
slack to keep the sections just outside the current view so that turning does not stall.

So the architecture is a **bounded VBO pool with eviction**, not a rule about what to mesh:

- The visibility walk supplies the **priority order** — `VisibleSet::toMesh` comes out nearest
  first, so a frame that can afford three meshes builds the three that matter.
- Sections drop out of the pool when they stop being drawn, least-recently-visible first.
- `mesh_distance` stays a hard outer bound, because a pool with no ceiling still fragments.

The visibility graph keeps its real job: cutting the **per-frame** draw list, which is where it pays
(underground it collapses to a handful of sections), and giving the BFS a sane order for early-Z.

The lesson worth keeping is the one about method. The histogram was real, the inference from it was
plausible, and it was still wrong — the missing step was that "never seen" and "not reachable" are
different sets, and only one of them is cheap to compute.

Two supporting numbers from the same run:

- **Worst section: 3,281 quads** against the 12,288 the shared index buffer allows. Real terrain
  uses a quarter of the worst case, so the buffer stays sized for the checkerboard bound rather
  than for observed terrain — a pathological world must not truncate.
- **36 % of sections are uniform air** and are rejected before the mesher runs; another 517 of the
  3,370 that were meshed produced nothing at all (solid rock inside solid rock). The cheap
  `sectionIsEmpty` test earns its place.
- Meshing costs **28.5 us per section** on the development host at `-O3` (10.0 filling the scratch,
  19.1 emitting faces). The ARM11 is far slower and this does not extrapolate cleanly, but it
  establishes that the work is small and that the binding constraint is memory, not mesher
  throughput. Fluid cost 2.7 µs of that emit — measured against the same world with the fluid
  emitter switched off, not estimated.

### The walk's constant factor: ARMv6k cannot divide (measured, fixed)

The algorithm above was right and its implementation was paying for a fact about the CPU. **ARMv6k
has no integer division instruction**, so every `%` in `core/render/visible_set.cpp` compiled to an
`__aeabi_idivmod` call out to libgcc. Counted out of the shipped object rather than reasoned about:

```console
$ arm-none-eabi-objdump -d build/a1.1.2/CMakeFiles/3dalpha_core.dir/src/core/render/visible_set.o \
    | awk '/^[0-9a-f]+ </{fn=$2} /aeabi_idivmod/{c[fn]++} END{for(f in c) print c[f], f}' | sort -rn
10 <mc::render::buildVisibleSet(...)      4 <SectionField::find(...)
 2 <SectionField::visibility(...)          2 <SectionField::sectionDirty(...)
 2 <SectionField::meshSlot(...)            2 <SectionField::isLoaded(...)
 ...                                       # 34 call sites in the file
```

`find()` did not inline, and the walk asked four separate accessors about every section it
visited — `isLoaded`, `meshSlot`, `sectionDirty`, `visibility` — each of which resolved the column
from scratch, then did two more divisions per face inside `visitedIndex`. **About twenty software
divisions per visited section**, plus four passes over a `Cell` grid on a console whose old model
has no L2 cache at all.

None of it was necessary. `cellIndex` is a wrap into a `(2 * radius + 1)` grid, every caller checks
`inRange` first, so the offset from the centre is bounded by the radius and the wrap is one
conditional add against a base cached when the centre moves:

```cpp
int cx = baseX_ + int(chunkX - centreX_);   // baseX_ = floorMod(centreX_, edge_)
if (cx < 0)           cx += edge_;          // |offset| <= radius_, so one
else if (cx >= edge_) cx -= edge_;          // correction always suffices
```

| | before | after |
|---|---|---|
| `__aeabi_idivmod` call sites in `visible_set.o` | **34** | **6** |
| divisions per visited section | ~20 | **0** |
| divisions per frame | ~20 × sections visited | 4 (`setCentre` ×2, camera seed ×2) |
| column lookups per visited section | 4 | 1 (`SectionField::ColumnRef`) |
| `visited` array cleared per frame | `edge² × 8` bytes | once per 65,535 frames (stamped) |

The six that remain are all once-per-frame or once-per-render-distance: `setCentre`, `reset`, and
the camera's own seed index, which keeps a `floorMod` on purpose because the camera is not
guaranteed to be inside the field — the centre follows it a frame late, since `WorldStreamer::update`
runs after the walk.

**This buys core 0, not the GPU.** The cube pass is a flat 0.208 µs/quad and this removes no quads,
so it cannot move the M2 gate; what it frees is the core the walk shares with meshing, the tick, the
relighter and streaming. Verified behaviour-preserving rather than argued: `--fly` over the real
660-chunk world at distance 8 for 400 frames is identical frame for frame, and `--mesh`'s reachable
and drawn tables are unchanged to the section (distance 8: 1,525 of 2,312 reached; 309–554 drawn).
Host suite 684/684. **The hardware `walkMs` before/after has not been taken yet.**

The overlay gained the graph's half of the culling, which
[§Debug overlay](#debug-overlay) had asked for and nothing reported: sections in range minus
sections visited is what the masks stopped, since the walk either reaches a section or never
arrives. It needs no new counter.

## 5. Hardware fog does the distance fade for free — **reversed, it cannot do this one**

The idea was the obvious one, and it is what shipped first:

```c
FogLut_FromArray(&fogLut, densityTable);            // 128 entries
C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
C3D_FogColor(skyColour);
C3D_FogLutBind(&fogLut);
```

Zero fragment cost, and the LUT is filled from a1.1.2's own linear fog: clear until
`renderDistance * 0.25`, solid at `renderDistance`.

**It does not work at Minecraft's distances.** The LUT's 128 entries are spread uniformly over
*window depth*, and window depth under a perspective projection is 1/d. `FogLut_CalcZ` — citro3d's
own helper, which is what says so — inverts to

```
d(i) = far*near / ((i/128)*(far - near) + near)
```

so with the near plane at 0.2 blocks (what it was then; it is 0.05 now, which crowds the ramp
further still) and the far plane at 176, the **whole fog ramp from 40 to 160 blocks lands between
LUT index 0.015 and index 0.495.** Entry 0 and entry 1 are the only two the
fog ever reads, and the hardware interpolates linearly between them. What comes out:

| distance | a1.1.2 | what the LUT gave |
|---|---|---|
| 30 | 0 % | 29 % |
| 40 | 0 % | **50 %** |
| 60 | 17 % | 72 % |
| 80 | 33 % | **83 %** |
| 120 | 67 % | 93 % |
| 160 | 100 % | 99 % |

And it gets worse the further you can see, because raising the render distance pushes the ramp
further into 1/d's flat tail: at half the render distance the error is 33 → 69 % on an old 3DS at
distance 6, and 33 → 83 % on a New one at distance 10. Reported from hardware as "the fog is too
thick for this render distance, it should scale" — which is the symptom exactly.

Raising the near plane does not rescue it (the ramp is still inside the first two entries at any
near plane you can stand next to a wall with), and neither does a bigger LUT, because there is not
one.

**What replaced it: one line in the vertex shader and one combiner stage.**

`out_pos.w` is already the number GL_LINEAR fog is defined against — every citro3d perspective
matrix writes `M[3][2] = -1`, so `w` is `-z_view` — so the fog amount is
`clamp(w * a + b, 0, 1)` for a line through (`fogStart`, 0) and (`fogEnd`, 1), computed per vertex
and interpolated perspective-correctly, which is exact across a quad one block wide.

Getting it to the fragment costs a channel, and there is exactly one: **the vertex colour's alpha**,
which was a constant 1.0. Primary colour is the only per-vertex value a texenv stage can read, and
the three colour bytes already carry face shade. So:

```
stage 0   rgb = atlas * primary.rgb        alpha = atlas.a          (REPLACE, not MODULATE)
stage 1   rgb = previous * lightmap        alpha = previous
stage 2   rgb = lerp(previous, sky, primary.a)   alpha = previous    (INTERPOLATE)
```

The alpha routing is load-bearing: leaving stage 0 modulating alpha would multiply water's 150 by
how foggy it is, so lakes would go opaque as they receded. Fog is applied before blending, which is
what GL does too.

Cost: three vertex-shader instructions, one more combiner stage, **no extra texture fetch** and no
extra bandwidth. The two shaders must agree, or a flower fades at a different rate from the block
it stands on.

Tint the fog colour by time of day, and swap it when the camera is inside water or lava (alpha does
both) — that is still a `C3D_TexEnvColor` write, and now it is a single one per frame.

## 6. Fill-rate knobs

- **RGB565 pipeline** — render target `GPU_RB_RGB565`, `gfxSetScreenFormat(GSP_RGB565_OES)`, and a
  matching `GX_TRANSFER_FMT_RGB565`. Halves colour bandwidth for slight banding. Ship as a
  "16-bit colour" option.
- **Resolution scale** — render the world into an offscreen texture at ½ or ⅔ height and blit it
  scaled up. Fragment count scales with the square, and in stereo the world is drawn twice, so this
  is the strongest single lever on an Old 3DS. Keep the GUI at native resolution so text stays sharp.
- ~~**Depth 16-bit always** (`GPU_RB_DEPTH16`). 24-bit depth buys nothing at these draw distances
  and costs bandwidth.~~ **Wrong, and it is these draw distances that make it wrong.** Depth
  resolution under a perspective projection is

  ```
  dd = d^2 * (far - near) / (near * far * 2^bits)
  ```

  and at near 0.2 / far 176 that is 0.19 blocks at 50 away, 0.76 at 100 and **1.95 at 160** — so
  past roughly 110 blocks two surfaces a whole block apart share a depth value and which one wins
  is decided by rounding. It changes as the camera moves and it differs between the two eyes, so it
  reads as shimmer rather than as a static artefact, and it is worst exactly where a long render
  distance is supposed to be paying off. `GPU_RB_DEPTH24_STENCIL8` multiplies all of those by 256;
  a block of separation at 160 blocks becomes 178 depth units. It costs 375 KB of VRAM across both
  eyes, against the ~5 MB M0 measured free, and depth-buffer bandwidth the GPU has (0.8 ms of draw
  time per frame at distance 10).
- **Never use 800×240 wide mode.** It doubles fill and is mutually exclusive with stereo 3D.

## 7. Storage: palette sections and taming the Alpha file layout

A raw Alpha column is 80 KB; at render distance 8 that is ~23 MB of block data on a 64 MB device.
Palette-compressed 16³ sections bring a column of a **real** a1.1.2 world to a mean of **18,013
bytes — 4.55×** (660 chunks measured; median 17,718, range 9,472–32,342). At render distance 8 that
is ~5.1 MB of block data against the ~8 MB budgeted.

87 % of nibble planes and 37 % of sections turn out to be uniform, and exactly **one** section in
the whole world needs more than 4-bit palette indices — the nibble planes are where the memory is,
not the block palette. Full breakdown in [world-format.md](world-format.md).

### Storage I/O: what the devoptab actually costs

The first draft of this playbook said to use raw `FSFILE` handles because newlib's devoptab "adds
real per-call overhead". Disassembling the installed libctru (`archive_dev.o`) shows that is wrong,
so the recommendation has changed:

| Operation | What libctru's devoptab does |
|---|---|
| `read()` | calls `FSFILE_Read` directly — one IPC call, no copy |
| `write()` | calls `FSFILE_Write` (plus `FSFILE_GetSize` when appending) |
| `fsync()` | calls `FSFILE_Flush` |
| `readdir()` | `FSDIR_Read` with **entryCount = 32**, remaining entries served from a cache |

The batched `readdir` is the surprise, and it removes the strongest argument for going raw: a
directory walk over the Alpha world tree costs one IPC round trip per 32 entries either way. The
cost that is real sits *above* the devoptab, in newlib's `FILE` layer — its own buffering on top of
the FS service's, `_reent` lookup, per-call locking. **Use the file-descriptor API (`open`, `read`,
`write`, `fsync`), not `fopen`/`fread`**, and that layer is simply absent.

What raw `FSFILE` still buys, none of it yet worth a second implementation:

- Positional reads with no seek state (`FSFILE_Read` takes a `u64` offset), so no `lseek` call.
  Measured since: the `lseek` it would save is free anyway — see the packed-worlds note below.
- Explicit `FS_WRITE_FLUSH` per write rather than a separate `fsync`.
- Holding one archive handle and pre-built `FS_Path` values, skipping the UTF-8 → UTF-16 conversion
  and cwd resolution that `open()` does per call. With one file per chunk, **opens dominate** — this
  is the one worth measuring before dismissing.

Consequence of the 32-entry batch: an open `DIR` caches 32 × 552 bytes ≈ **17.7 KB**. The world
index scan should hold one directory open at a time rather than recursing with several.

The point stands regardless of which API wins: the dominant cost is the IPC round trip to the FS
sysmodule plus SD latency, which both APIs pay identically *per operation*. The lever is the number
of operations, which is what the cached chunk index, the coalesced write-back queue and packed mode
attack. Picking the API is worth a few percent; removing an operation is worth all of it.

**A faster SD card is not a lever at all**, and it is worth saying plainly because it is the first
thing anyone reaches for. A chunk file is 2,917 bytes at the median; even at a pessimistic 5 MB/s
the transfer is under a millisecond, against four to six IPC round trips plus FAT metadata. Neither
is internal storage a second tier — a title's save data and extdata live *on the SD card*, CTRNAND
is not writable from a 3DSX, and both go through the same sysmodule. See
[save-data.md](save-data.md).

### What was built: core/world/chunk_cache.hpp

Three pieces of SD work used to run on the render thread, and the symptom was a hitch exactly when
chunks loaded and unloaded:

| Where | What it cost | Now |
|---|---|---|
| `WorldStreamer::classifyCell` | one `stat` per newly exposed cell, unbudgeted — ~25–49 per chunk-boundary crossing at distance 8 | a lookup in the leaf-directory index, listed ahead of the player on the I/O thread |
| `WorldStreamer::loadColumn` | `open`+`fstat`+`read`+`close` plus a gzip inflate, inside the frame, 1–2 a frame | a clone out of the cache, or a request posted and the cell retried next frame |
| both of the above | queued behind the generation worker's `storageLock_` while it deflated and wrote a chunk file | the render thread does not take a storage lock at all |
| `WorldStreamer::dropCell` | the column was freed, and re-read if the player turned round | given back to the cache and served from RAM |

The cache is byte-capped (8 MB on a New 3DS, 2 MB on an Old one, against the heap split in §2b) and
holds both the retention ring and a read-ahead band two chunks wider than the load radius. At
distance 8 that band is 264 columns ≈ 4.6 MB at the measured 18,013-byte mean.

The precedent is the original's own: `ft` holds `new ga[1024]`, a 32×32 direct-mapped chunk table
that saves the previous occupant of a slot when a new chunk lands on it. 1024 columns is 18.4 MB,
which is more than an Old 3DS heap has — hence the byte cap and LRU.

**`Stats::mainThreadMicros` on the Storage debug page is the regression test**: it is main-thread
time inside a storage call and is expected to read 0.0. The one thing that can raise it is the
`stat` fallback when a directory group is asked about before its listing arrives, which is what
sprinting into unwalked ground does.

Plus: the chunk index instead of per-chunk `stat`, and all I/O on the I/O thread — both built.

### Packed worlds: the operation count, measured

The lever is the number of operations, and the packed format is the change that removes most of
them. Measured with `./build-host/3dalpha --world-info` on both shapes of the same real world, 1,119
chunks:

| | file opens to read every chunk | directory listings | on disk at a 16 KB cluster |
|---|---|---|---|
| Folder | 1,122 | 1,157 | 37,339,136 B |
| Packed | **4** | **1** | **4,145,152 B** |

At four to six IPC round trips per file operation, that is roughly 9,100–13,700 round trips down to
about 25 — and 9× less card. Both numbers come off the file and directory counts, so they hold
whatever the card's speed is.

**The wall-clock figure is still owed and can only be taken on hardware.** On a Linux host with a
warm page cache, meshing the whole world takes 0.559 s from the folder and 0.517 s from the packed
copy — an 8% difference, which measures the host's cheap `open`, not the console's expensive one.
Record the real number here after the next launch: create a world (it is packed), fly until it
generates, then convert it to Folder from the world options screen and fly the same path again.

The sector size was re-derived rather than borrowed. McRegion's 4,096-byte sector wastes 53% on
a1.1.2's chunk-size distribution (min 1,194 / median 2,917 / mean 2,945 / max 5,872) because the
distribution straddles it; 1,024 bytes wastes 17%. See [packed-worlds.md](packed-worlds.md).

Positional I/O got the seam it needed: `io::RandomAccessFile` with `readAt`/`writeAt`. On the host
those are `pread`/`pwrite`; **devkitARM's newlib has neither**, so the 3DS build seeks first. That
costs nothing here — libctru's devoptab `lseek` for `SEEK_SET` is arithmetic on a struct in the
application's own memory, no IPC, and `read` then hands the stored offset to `FSFILE_Read`. Only
`SEEK_END` would cost a round trip, and nothing seeks that way.

### Batch reads: 576 operations down to 98, measured

Packed mode removed the opens; what was left was still **one `readAt` per chunk**, and the World
screen's diorama needs 576 of them for one world. At the 4 ms an operation this codebase models
(`MC_IO_LATENCY_US`) that is 2.3 s of card against ~90 ms of CPU for the same chunks — the picture
was waiting on round trips, not on work.

`RegionFile::readMany` sorts a batch by sector offset, merges runs within `kBatchGapSectors` while
they fit `kBatchReadBytes`, and issues one `readAt` per merged run. `WorldPeek::loadChunks` groups a
request by region and feeds each group through it; a folder world falls back to one read each,
because a chunk there is its own file and no ordering removes an open.

**How many chunks are asked for at once is the whole lever**, and it is a scheduling decision, not a
tuning constant. Measured over a real packed world's 576-chunk diorama window (1.52 MB of payload,
four regions, 64 KB scratch, 16-sector gap):

| how the reads are grouped | card reads | transferred | modelled at 4 ms/op |
|---|---|---|---|
| one chunk at a time | 588 | 1.58 MB | 2.35 s |
| one batch per tile | 176 | 3.41 MB | 0.70 s |
| **the centre tile, then the other eight** | **98** | **3.02 MB** | **0.39 s** |
| the whole table as one batch | 75 | 2.87 MB | 0.30 s |

(Counts include the 12 reads that open four regions — two header copies and a directory each.)

A tile is 8 chunks out of a 32-wide region row, so a tile's chunks are eight short runs with the
rest of the row between them; batching *across* tiles is what puts those runs next to each other.
`menu_preview` therefore reads the centre tile alone — so something lands on the table as fast as it
ever did, 36 reads — and then all eight remaining tiles together.

**A batch that reaches the screen only when it finishes is a stall, and on hardware it showed.** The
first console run of the two-pass schedule put the centre tile up immediately, sat for several
seconds, then filled the other eight back to back — because nothing was posted until all 512 chunks
were read and folded. So `WorldPeek::loadChunks` reports *every* chunk asked for, absent ones first
and with no payload, which lets `readDioramaTiles` count each tile down and hand it over the moment
its last chunk lands. The reads are still one batch; the meshing is still per tile. Measured on the
host, the nine tiles now come back at 9, 24, 43, 45, 58, 62, 78, 80 and 81 ms into a read that takes
81 ms, against all nine at the end of it — for the same 98 card reads, since an absent chunk costs
no I/O.

Splitting the *read* instead was measured and does not work: a second pass over any four outer tiles
already spans the region's whole sector range, so three passes cost 143 reads and five cost 153 —
worse than the 86 two passes cost, with no more progress to show for it.

Widening the gap and the cap was measured and rejected. Per tile, a 64-sector gap and a 256 KB
scratch reaches 62 reads but transfers 6.08 MB — four times the payload, for a buffer an Old 3DS
should not spend. Batching more chunks at once is strictly better than merging more aggressively
across a bigger gap: 98 reads for 3.02 MB beats 62 for 6.08 MB on both axes that matter.

**The wall-clock figure is owed on hardware**, like the rest of this section. What is measured here
is the operation count, which holds whatever the card does.

## 8. Faster inflate

Every chunk file and every `0x33` Map Chunk payload is a deflate stream. On a 268 MHz ARM11 this is
visible whenever chunks stream in.

Benchmark **libdeflate** against **miniz**/zlib on hardware. libdeflate is typically ~2× faster on
ARM for whole-buffer inflate, which is exactly our access pattern — we always hold the complete
stream, so streaming inflate buys us nothing.

Decompress on the worker thread. Never on core0.

## 9. Texture memory

`terrain.png` at 256×256 RGBA8 is 256 KB. Convert offline (and cache the conversion for user packs):

| Format | Size | Use when |
|---|---|---|
| ETC1A4 | 64 KB | default; 1 byte/texel with a 4-bit alpha channel |
| RGBA5551 | 128 KB | 1-bit alpha is enough (glass, leaves) and ETC1 artefacts show |
| RGBA4444 | 128 KB | pack has gradients ETC1 handles badly |

Let the GPU do the Morton swizzle with `C3D_SyncDisplayTransfer`; craftus's `TextureMap.c` shows
both that path and a CPU `tileImage32` fallback for textures under 64×64.

Nearest filtering is free and correct. Mipmaps are optional — they *help* a fill-bound scene by
improving texture-cache locality at distance, so make it a toggle and measure rather than assuming.

## 10. Stereo 3D done cheaply

- Poll `osGet3DSliderState()`. **When it reads 0, skip the second eye's entire draw pass** and call
  `gfxSet3D(false)`. craftus always renders both eyes; that is roughly half the frame given away.
- Compute the visible-section set **once** for both eyes using a widened frustum. Only the
  projection matrix (`Mtx_PerspStereoTilt`) and the draw pass differ per eye.
- **Derive the widening; do not pick it.** A section the outer eye can see and the centre one
  cannot is a section that appears in one eye and not the other, at the left or right edge of the
  screen, which is the most uncomfortable thing a stereo renderer can do. Reading
  `Mtx_PerspStereoTilt` out of `libcitro3d.a`, the row that becomes the 400-pixel axis is

  ```
  clip.y = -x/(t*A) + z*iod/(2*S*t*A) + iod/2,   w = -z,   t = tan(fov/2), A = 400/240
  ```

  so solving `clip.y/w = 1` puts that eye's edge at `x = -t*A*d - d*|iod|/(2*S) + |iod|*t*A/2`
  against the centre eye's `-t*A*d`. The term that grows with distance is `d*|iod|/(2*S)`, so
  widening the half-extent by `|iod|/(2*S)` covers it at every distance.

  This started as a flat `* 1.08` on the field of view. The honest number at 10 px of disparity is
  1.025, so 1.08 was generous — but `|iod|` is on a d-pad and 1.08 stops being enough somewhere
  around 25 px, so the artefact got worse the harder the player turned the 3D up. A constant is the
  wrong shape for this.
- Expose a "3D depth" scale so users can trade depth for a smaller IOD, which keeps more geometry
  inside both frusta.

## 11. Two screens means zero HUD overdraw

Put the hotbar, inventory, chat, crafting and debug readout on the **bottom screen**. The top screen
then renders nothing but the world — no HUD overdraw at all, which is real fill-rate on a fill-bound
device.

The bottom screen only needs redrawing when it changes: track a dirty flag and skip both its clear
and its draw on unchanged frames.

Touch also solves the button shortage for inventory and crafting without modal key combinations.

### The hearts are on the top screen, and that is a deviation from this section

**Decided with the user at M3 step 4.** Everything else this section asks for is where it says:
the hotbar, the inventory, the four container screens, the chat and the debug pages are all on the
bottom screen and the world has the top screen to itself. **The health, armour and air row is the
exception** — `core/render/hud_mesh.hpp` builds a1.1.2's own `lu` layout as screen-space
`DetailVertex` quads and `Renderer::drawHud` draws it over the world, once per eye.

The argument for it is not a performance one and is not pretending to be. Health is the one number
a player reads *while looking at the thing that is hurting them*; a heart bar on the other screen
is a heart bar nobody looks at until it is too late. Everything that can be checked at leisure
stays where this section put it.

What it costs, stated so it can be measured rather than argued about:

- **At most 150 quads**, and usually far fewer: ten hearts, up to ten armour icons and up to ten
  bubbles, each a container tile plus an overlay. Nothing is drawn at full health with no armour
  and a dry head except the ten containers and their hearts.
- **Two draw calls, one per eye**, at the screen plane with zero stereo offset — so the row does
  not swim in depth, and the second eye costs the same fill as the first.
- **The fill is 16.875 x 16.875 pixels per icon on a 400 x 240 screen**, alpha-tested rather than
  blended, which is the cheapest way a textured quad can reach the framebuffer on a PICA200. That
  is `kHudTexelUnits` (30 sixteenths of a pixel, so 1.875) times the sheet's own 9 x 9 cell: at 1:1
  the row was legible in a screenshot and not on the panel. **It was a flat 2.0 and came down by
  6.25 % on the user's ask**, which no integer scale can do — the trade is that a heart's nine texel
  rows are no longer all the same height on screen, one pixel in eight. Worst case — ten hearts
  flashing with their outlines, ten armour icons and ten bubbles — is a shade under 18,000 texels,
  a little under 2% of one eye.
- **The rows sit in the top two corners rather than across the bottom centre**, which is where
  a1.1.2 puts them: its layout is measured outwards from the hotbar it sits on top of, and the
  hotbar here is on the other screen. Hearts top left, armour top right, bubbles under the hearts.
  See `docs/physics-a1.1.2.md`, *The HUD*.

**Owed on hardware: a fill-rate number for this pass**, measured the way §2's quad cost was — a
frame with the row up against one with it suppressed, at the same position and render distance.
Until that exists this entry is a decision with an estimate attached, not a measurement.
`docs/status.md` carries it as open.

### And the bottom screen's map redraw, which is now two sizes

The map window on the bottom screen is 212 x 162 in a gamemode with a hotbar and 212 x 202 in
Spectator, which has no hotbar band to reserve — see `platform/ctr/map_screen.hpp`. The only
measurement there has ever been is **36,864 pixels in about 700 microseconds on a New 3DS**, so
scaling off it gives roughly 650 for the banded window (34,344 pixels) and roughly 815 for the bare
one (42,824). Both are estimates from one point, and the larger one is the one worth taking to
hardware: it is the first time this screen's redraw has been asked to cost more than the number
that was measured. The Info page already reports what a map redraw cost, so the figure is one
session in Spectator away.

**What is not an estimate** is the chunk store behind it. At zoom -1 the bare window covers
424 x 404 blocks, which touches 28 x 27 = 756 chunk patches against the banded window's 588, so the
store capacities went to 1,024 on an old 3DS (1.5 MB) and 2,048 on a New one (3 MB) — 4% of the
newlib heap each console gets, and the same headroom over the widest window they had before. A
store that cannot hold the window does not degrade; it thrashes, because the ring scan touches the
centre first and so the least-recently-used entry is the ground under the player.

## 12. New 3DS

RSF (`makerom`):

```
SystemModeExt : 124MB
CpuSpeed      : 804MHz
EnableL2Cache : true
CanAccessCore2: true
```

Also call `osSetSpeedupEnable(true)` at runtime — recent Luma3DS no longer forces the clock from RSF
alone. Detect the model with `APT_CheckNew3DS()` and pull raised defaults (render distance, mesh
distance, resolution scale, worker count) from a **separate defaults profile**, not from
`if (isNew3DS)` scattered through the code.

C-stick (`KEY_CSTICK_*`) and ZL/ZR exist only on New 3DS. The input layer maps *actions* to a
rebindable binding table with distinct Old/New default sets.

## 13. Build flags

```
-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
-O3 -ffunction-sections -fdata-sections -Wl,--gc-sections -fomit-frame-pointer
-fno-exceptions -fno-rtti -fno-threadsafe-statics
```

Add LTO once the build is stable. `-fno-exceptions` matters for more than speed: unwind tables are
significant size on a device where the whole binary competes with world data.

---

## Options contract

Every option must remove real work, not hide a feature. This table is the contract — each row states
what stops executing.

| Option | Off / lower ⇒ what stops running |
|---|---|
| `render_distance` (2–12) | fewer sections stored, meshed, culled, drawn; fog LUT rescaled |
| `mesh_distance` (≤ render) | sections beyond it keep block data but hold no VBO |
| `stereo_3d` | second eye's entire draw pass skipped; `gfxSet3D(false)` |
| `resolution_scale` (1.0 / 0.75 / 0.5) | fragment count scales with the square |
| `color_depth` (16 / 24) | RGB565 render target halves colour bandwidth |
| `smooth_lighting` | AO flood-fill skipped at mesh time, **and** enables face merging ⇒ fewer quads |
| `greedy_meshing` | merges coplanar same-texture, same-light faces, runs up to 3×3; 2.02× fewer cube quads on the real world, 34 → 22 ms GPU on hardware. **Built**, on the debug page (status.md §22) |
| `clouds` | whole clouds pass skipped, VBO freed |
| `particles`, `entity_shadows`, `view_bobbing`, `hand_render` | those passes and their per-frame updates skipped |
| `mipmaps` | mip chain never generated, ~⅓ less texture memory |
| `fancy_leaves` | leaves become opaque cubes ⇒ interior faces culled ⇒ far fewer quads |
| `fancy_water` | translucent pass skipped; water becomes alpha-tested |
| `fps_cap` (30 / 60) | frame pacing halves GPU and CPU work |
| `audio` | ndsp never initialised, decode thread never spawned, no linear memory taken. Off is the whole subsystem absent, not a volume of zero |
| `music_volume` | 0 stops the playing track outright and frees its voice, as a1.1.2's `of.a()` does — it does not play silently |
| `texture_quality` | atlas downscaled ⇒ less VRAM, better texture-cache hit rate |
| `worker_threads`, `n3ds_clock` | scheduling and clock policy |

## Debug overlay

You cannot tune what you cannot see, and on-device profiling is otherwise painful.

The bottom screen has **three pages**, cycled with **SELECT + Y** forward and **SELECT + X** back:
the player's screen (the hotbar's future home), the readout below, and a settings page carrying the
knobs that change what the renderer *does* rather than what it reports — render distance and
wireframe. SELECT is a modifier rather than a page key of its own because X is sprint and Y is the
stereo tuner, and a page cycle that could be hit mid-flight would be worse than no page cycle.

**The frame split is the number to read first, and it has to be a split.** A 17.5 ms frame against
a 0.8 ms GPU says nothing on its own: `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` blocks until VBlank, so
16.7 ms of every healthy frame is *supposed* to be waiting. The page reports `CPU busy` (walk +
stream + submit) against `vsync` separately, so "at the refresh rate with headroom" and "missing
frames" cannot be mistaken for each other.

The readout reports every frame:

- frame time split CPU / GPU, and the frame budget headroom
- `C3D_GetCmdBufUsage()`
- quads drawn, sections drawn, sections culled by frustum vs by visibility graph
- BFS steps taken
- VBO pool bytes used / free, split VRAM vs linear
- free linear heap, free VRAM
- pending mesh jobs, pending I/O jobs, inflate ms/chunk

## Performance gates

Enforced per milestone; a milestone that regresses a gate is fixed before the next one starts. Perf
debt on this hardware does not get paid back later.

| Device | Gate |
|---|---|
| Old 3DS | ≥ 30 fps at render distance 6, 3D off, default options |
| New 3DS | ≥ 30 fps at render distance **8**, 3D on, default options |

**The New 3DS gate was 10 and is now 8, deliberately and provisionally.** The sixth launch measured
0.208 µs per quad, which puts distance 10 with 3D on at 9.4 fps — 3.2× over. Distance 8 is the
render distance the rest of the engine is already sized around (the streamer, the o3DS pool ceiling,
the play limit for an old 3DS), so it is the honest floor to hold the renderer to while the
geometry-shader path is unmeasured. **This is a floor, not a target**: distance 10 stays the number
worth wanting, and if the seventh launch says the cost was vertex fetch, raising it back is the
first thing to reconsider. Refine once there is a measurement instead of a judgement.

## Measurements

Fill in as milestones land. Estimates are worthless once hardware disagrees.

### Hardware readings (M0 probe, New 3DS, 3DSX via Homebrew Launcher)

| Reading | Value | Note |
|---|---|---|
| App region | 124 MB | fully committed at startup; "free" always reads 0 |
| Heap / linear (libctru default) | 91 MB / 32 MB | linear at its cap — see §2b |
| Heap / linear (3DAlpha policy) | 40 MB / 82 MB | 83,860 KB linear; boots and renders |
| VRAM free after 2 stereo top targets | 5,019 of 6,144 KB | targets cost exactly 1,125 KB |
| Winding | `GPU_CULL_BACK_CCW` correct | faces listed CCW seen from outside |
| Morton swizzle | correct | texture tiles render unscrambled |
| Fragment fill rate | ~210 M/s | two textures bound; 4.77 ns each |
| Fixed cost per frame | ~562 us | clear + display transfer, scene-independent |
| Second texture unit | free | see section 1b |
| SD cluster size | **16 KB** | not the assumed 32 KB; halves world-on-disk cost |
| SD free | ~24.3 GB | |
| 1024² block world, unpacked | 64 MB | 4,096 chunks × 16 KB |

### Fragment pipeline (M0b overdraw pass, New 3DS, two textures bound)

A screen-filling quad drawn N times with the depth test off, so the fragment count is exactly
`N x 400 x 240` and nothing else varies.

| Layers | Fragments | GPU draw |
|---|---|---|
| 1 | 96,000 | 1020 us |
| 8 | 768,000 | 4230 us |
| 64 | 6,144,000 | 29,850 us |

Marginal cost is **4.777 ns/fragment** between 1 and 8 layers and **4.766 ns** between 8 and 64 —
agreement across an 8x range, so this is a real slope and not noise:

- **Fill rate ~210 M fragments/sec** with the atlas and the lightmap both bound.
- **Fixed overhead ~562 us/frame**, independent of scene complexity. This is the clear plus the
  display transfer, and at ~3.4 % of a 16.7 ms frame it is the measured argument for §11's
  dirty-flag rule: never clear or redraw the bottom screen on an unchanged frame.

The model predicts the M0 cube at ~638 us against 730 us measured, which is close enough to trust it
for budgeting.

**Do not trust a per-fragment figure above ~4295 us of draw time from a build before this was
fixed.** `unsigned long` is 32 bits on ARM and `drawUs * 1000000` overflowed it, so the 64-layer pass
reported 664 ps/fragment instead of 4858 — a wrong number that looked plausible. The arithmetic is
64-bit now.

| Technique | Estimated | Measured | Notes |
|---|---|---|---|
| 12-byte vertex + shared index buffer | 2× vertex bandwidth | mechanics validated M0 | 48 B/quad vs craftus 96; 8 B blocked on UV range |
| Lightmap vs baked vertex light | free day/night, +1 texture fetch/fragment | **free: -25 ps/frag over 93 A/B rounds** | adopted; §1b |
| Geoshader quad expansion | 12× vertex bandwidth, −25 % VS throughput | — | net effect unknown |
| VBOs in VRAM | ? | **not possible** | CPU cannot write VRAM; permission fault. Reversed in §3 |
| RGB565 pipeline | ~2× colour bandwidth | — | |
| Resolution scale 0.5 | ~4× fragment count | — | |
| libdeflate vs miniz | ~2× inflate | — | |
| Skip second eye at slider 0 | ~2× frame | — | |
