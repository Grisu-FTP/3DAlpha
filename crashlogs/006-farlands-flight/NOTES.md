# 006 — flying at the Far Lands, geoshader format, dropped to the HOME menu

Reported 2025-08-31, twice in one session. The first report was a **hang** — black top screen,
HOME dead, no exception — and that one is understood and fixed: the frame path's blocking
`C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` was gated on `cubeFormat_`, which describes the frame about to
be recorded, while the wait is for the frame already in the queue. See `docs/3ds-performance.md` §2,
*The third hardware failure*.

The second report is this one, and it is a **different failure**: flying at the Far Lands with the
geometry-shader cube format live, the console **dropped cleanly to the HOME menu**. No exception
screen, no Luma dump, nothing on the card.

## There is no dump here, and that is the finding

`3DAlpha-a1.1.2.elf` is the build that produced it, archived per the rule at the top of
`../README.md` — kept even though nothing resolves against it, because the *next* occurrence may be
a real fault and this is the pair that would resolve it.

**A clean drop to HOME is `abort()`, and with `-fno-exceptions` that is what running out of newlib
heap turns into.** Disassembled out of this very ELF:

```
_Znwj:  bl malloc ; subs r3, r0, #0 ; popne           @ the good path
        bl _ZSt15get_new_handlerv ; cmp r0, #0
        blxne r0 ; b <retry the malloc>
        bl __cxa_allocate_exception ; bl __cxa_throw   @ no landing pad anywhere
```

libstdc++ is built with exceptions; every frame in this binary is not. The throw finds no handler,
reaches `std::terminate`, which calls `abort`, which is `_exit` — and a 3DSX that exits goes to the
HOME menu exactly as though the player had quit. **It is the only failure this project can have
that leaves no artifact at all**, which is why the previous report of the same shape,
`005-lava-flicker-session`, ended at "consistent with memory exhaustion and nothing narrower".

## Why the Far Lands is the case

The heap policy gives the newlib heap `min(available / 3, 40 MB)` — see `src/platform/ctr/heap.cpp`
— and the columns live in it. Column storage is **paletted per section**, so ordinary terrain is
cheap: a section that is all air or all stone is one palette entry. **A Far Lands section is
neither.** Every one of them is mixed stone, dirt, gravel and air the whole way up, so the palette
buys nothing and every resident column costs close to its unpaletted size. The same render distance
that is comfortable over ordinary ground is several times the block bytes out there, against a heap
that does not grow to match.

This is a mechanism with the right shape, **not a measurement.** What it predicts is that
`blocks N.N MB in the heap` on the Info page is far higher at the Far Lands than anywhere else and
that `free KB heap` is near zero just before the exit. Both of those are now on that page.

> **Measured afterwards, and half of the paragraph above is wrong.** 169 columns generated at
> chunk (784426,0) against 169 at (0,0), through the real generator: a column costs **21.7 KB at the
> Far Lands against 14.0 KB over ordinary terrain — 1.55x, not "several times".** And the palette is
> not buying nothing; it is working normally. What the Far Lands lacks is *uniform* sections, which
> cost literally zero — 763 of 1352 sections are non-uniform over ordinary ground, against 1078 of
> 1352 out there.
>
> The conclusion survives the correction and the reasoning does not. 1.55x is still fatal, because
> the grid is `(2d + 1)^2` columns whatever the terrain: at the debug page's ceiling of 24 that is
> 2401 columns, **50.9 MB at the Far Lands against a heap that was capped at 40**. The trigger was
> never the Far Lands alone — it is the Far Lands *and* a large render distance, which is exactly
> how it was reported.
>
> See `docs/3ds-performance.md` §2, *The heap split*, for the per-plane table, for the two savings
> that were measured and found dead (`compact()` recovers 0.0%, a narrower palette tier 2%), and for
> what was done instead.

## What was changed off the back of it

- **`heapFreeBytes()` is on the Info page.** It existed and nothing displayed it, which is why
  `005`'s notes could ask for it and the next report still could not carry it. The row is now
  `free KB heap N lin N vram N`.
- **An out-of-memory reporter**, `mc::ctr::installOutOfMemoryReporter()` in `heap.cpp`. A
  `std::set_new_handler` that prints what ran out to the bottom screen — single-buffered, so it
  needs no GPU — writes the same line to `sdmc:/3dalpha-oom.txt`, waits two seconds so it can be
  read or photographed, and then ends the process itself. It cannot save the run: `operator new`
  retries the malloc when a handler returns, so a handler that has freed nothing must not return.
  What it buys is that the next one of these arrives with a cause attached instead of as
  "it rebooted".
- The figures it prints come from `mc::ctr::setMemorySnapshot`, written once a frame by the game
  loop as plain scalars. **Not fetched at the moment of failure**: the allocation that fails may
  fail on the generation worker or the I/O thread, and reaching back into the streamer from a new
  handler would ask for the locks whichever of them is already holding.

## What the next run should produce

Either `sdmc:/3dalpha-oom.txt` — in which case the numbers in it name the consumer and this stops
being a hypothesis — or nothing, in which case the exit was not an allocation failure and the whole
paragraph above is wrong and should be deleted rather than defended.

**It produced the file.** Reported 2026-09-01: the reporter fired, so the exit *was* an allocation
failure and the newlib heap *was* what ran out. The mechanism above is confirmed in outcome and
corrected in magnitude by the measurement inset earlier in this file. `crashlogs/007` explains why
the same event was also reported as an exception screen — `abort()` frees the heap out from under
libctru's GSP event thread, and whether that thread wakes before `svcExitProcess` decides which of
the two the player sees.

What was done about it is in `docs/3ds-performance.md` §2, *The heap split*: the heap/linear split
was rebalanced (40 → 75 MB on a New 3DS, out of linear memory the VBO pool was never going to use),
and `WorldStreamer::setMemoryBudget` put a real bound under the render distance so that the next
view too big for the heap costs rings rather than the process.
