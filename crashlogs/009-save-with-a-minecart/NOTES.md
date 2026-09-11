# 009 — the save screen drew a world it had already freed

**Verdict: a use-after-free of `tick::TickWorld`, exposed by the minecart draw
inside `WorldStreamer::close`'s progress callback.** Not a storage fault, and
not the command-buffer overrun the previous session filed it as.

## What the dumps say

Both are `3dsx_app`, ARM11 core 0, **prefetch abort — permission fault (page)**:
the CPU tried to *execute* a page that is not executable. The two faults are the
same event in two different builds, one 0x1000 apart from the other:

| | 11 | 12 |
|---|---|---|
| `pc` = `r0` = `r3` | `0x00227830` | `0x00228830` |
| `r1`, `r2` | `-10`, `-4` | `-10`, `-4` |
| `r5`, `r6`, `r7`, `r9` | `-152`, `-56`, `70`, `71` | same |
| `sp` | `0x080062A0` | `0x080062A0` |

## Why the target address settles it without the matching ELF

**Neither dump has the ELF that produced it** — see the warning in
`../README.md`; the build in `build/` post-dates both, so `addr2line` on it
answers confidently and wrongly, and the `lr` it resolves lands on an
instruction that is not a call. Nothing here rests on that resolution.

What does rest on evidence is the *contents* of the faulting page, which Luma
dumped out of the crashing process. From `0x2287D0` up it is a run of word pairs
in which the word at `A` and the word at `A + 4` both hold `A - 8`:

    0x002287D8: 002287D0        0x002287DC: 002287D0
    0x002287E0: 002287D8        0x002287E4: 002287D8

That is **newlib's `__malloc_av_`**, verbatim. `bin_at(i)` is
`&av_[2i] - 2*sizeof(size_t)`, and an *empty* bin stores `fd = bk = bin_at(i)`,
which is exactly "two adjacent words that both point eight bytes back". The
current build puts `__malloc_av_` at `0x228590..0x228998`, and `0x228830` is
inside it — a coincidence of layout, but the byte pattern is the proof and it
does not need the layout.

So the jump target was a **malloc bin pointer**, which is what the first two
words of a *freed* chunk hold.

## Which freed object

`TickWorld`'s first member is `TickAccess`, and its first two fields are
`void* ctx` then `world::ChunkColumn* (*column)(void*, i32, i32)` —
a context and a function pointer, at offsets 0 and 4. `columnFor` loads both
and calls through the second:

    ldr r4, [r0, #4]     ; access_.column
    ldr r0, [r0]         ; access_.ctx
    blx r4

Read out of a freed chunk, both of those come back as the same bin pointer —
which is precisely `r0 == r3 == pc` in both dumps. The arguments agree too:
`r1 = -10`, `r2 = -4` are chunk coordinates, and `r5 = -152`, `r6 = -56`,
`r7 = 70` are the block coordinates inside them (`-152 >> 4 == -10`,
`-56 >> 4 == -4`). Something asked a destroyed `TickWorld` for the block at
(-152, 70, -56).

## How it happened

`WorldStreamer::close` used to destroy `tick_` immediately after
`flushTickDirty()`, and *then* run the drain loop that reports progress. That
loop calls back into the caller once per pumped write, and on the 3DS the
callback is `drawSaveProgress`, which draws **a whole frame of the world**
behind the progress bar.

One pass in that frame borrows the tick world: `Renderer::drawMinecarts`, which
holds a `const TickWorld*` set once a frame from `world.worldTick()`. A cart
leans along the *track* rather than along its own motion, so the minecart pass
is the only entity draw that needs the world — and `buildMinecarts` asks it what
block each cart is sitting on. Every frame of the save screen was that read
against freed memory.

Which is why the crash needs **a minecart in the world** to happen at all, and
why it looked like a storage bug: it only ever fires while saving.

`tests/streamer_progress_test.cpp`'s
`the_save_progress_callback_can_still_read_the_world` reproduces it on the host.
Against the old ordering it reports **49 progress callbacks, all 49 with no
world** — 49 frames the console drew against a destroyed object.

## The fix

Two halves, and the first is the one that matters:

* `WorldStreamer::close` now releases `tick_` and `light_` **after** the drain
  loop, immediately before `cache_.close`. Nothing above needed them gone:
  `flushTickDirty` has already handed the tick's changes to the cache, and
  neither the generator flush nor the drain asks the tick anything. The save
  screen's frames now draw a live world, carts included.
* `runGame` hands the borrowed pointer back — `renderer.setMinecarts(nullptr,
  nullptr)` — after `close()` returns, so the borrow cannot outlive the world
  even if something later draws between the close and `renderer.shutdown()`.

## What the previous session got wrong, and why it is worth writing down

It read these same two dumps as "PCs point into the renderer command-buffer data
in `.bss`", concluded citro3d was overrunning its command buffer, and spent the
fix on a 64K-word command-space reserve, a separate linear buffer for the
crosshair, and a white outline shader. The `.bss` reading was the error: the
address is in `.data`, inside newlib's malloc bins, and the self-referential
word pairs around it say so outright.

The reserve is harmless and the separate crosshair buffer is tidier than sharing
one, so neither was reverted. The white outline **was** reverted: it made
a1.1.2's `glColor4f(0, 0, 0, 0.4)` selection box white to make the crosshair
visible, and both now come off a `tint` uniform instead.

**The lesson is the one the README already states and this pair proves twice
over**: archive the ELF *before* rebuilding. Had either build's ELF survived,
the call chain would have been a minute's work instead of a reconstruction from
a memory pattern — and the first reconstruction was wrong.
