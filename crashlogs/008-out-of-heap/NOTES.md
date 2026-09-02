# 008 — the out-of-memory reporter fired, and the obvious reading of it is wrong

`sdmc:/3dalpha-oom.txt`, reported 2026-09-01. Far Lands, geometry-shader format, large render
distance. **This is the first measurement of a real failure this project has** — everything before
it was a dump of the aftermath (`007`) or an absence (`006`).

```
OUT OF HEAP  free 4405k of 40960k  blocks 10688k  owed 1582k  pool 13425k
             cols 382  sect 1314  chunk -63 -2000007  geoshader
```

## What it settles

`006`'s hypothesis, in outcome: **the newlib heap is what ran out.** Not linear, not the command
buffer, not the GPU. The handler exists precisely so that sentence can be written down instead of
guessed at, and it is the whole return on `006`.

## What it contradicts

**Resident columns are 29 % of the problem, not the problem.** `blocks 10688k` of `40960k - 4405k =
35.7 MB` in use. And `cols 382` against the 729 that render distance 12 asks for — the grid had not
even finished loading when the heap ran out.

So the Far Lands story that `006` told and that `docs/3ds-performance.md` §2 was written around is
real but secondary. What is left, roughly:

| | |
|---|---|
| resident columns (`blocks`) | 10.4 MB — **measured** |
| chunk cache, clean side | up to 8 MB — capped by `cleanCapBytes`, not reported |
| generator's own cache | ~8 MB — `16 * loadRadius + 96` columns, not reported |
| owed to the card (`owed`) | 1.5 MB — measured |
| sound, map patch cache, stacks, allocator overhead | the remainder — not reported |

**The reporter named the smallest of the three big terms.** `clean` and `gen` are on that line now
so the next one does not have the same hole. `pool 13425k` is linear memory and cannot exhaust this
heap at all; it is on the line to rule itself out, and it does.

## What was done, and what it is honestly worth

- **The heap/linear split was rebalanced**, and *this* is the fix for this crash. `vboBudget` caps
  the VBO pool at 32 MB while the old policy handed linear ~83 MB, so ~38 MB sat idle beside a heap
  capped at 40 that was 35.7 MB used. The split now derives from what linear actually needs: New 3DS
  heap **40 → 75 MB**, Old 3DS **21 → 36 MB**, pool keeping its full budget in both. Checked
  numerically across twelve sizes first, because `__system_allocateHeaps` panics rather than
  reporting.
- **A byte budget on resident columns**, `WorldStreamer::setMemoryBudget`, with the admission radius
  shrinking a ring at a time over budget and growing back under seven eighths of it. **This would
  not have fired here** — 10.4 MB against a budget of five eighths of the heap. It is a real bound
  and the right shape, and it is a backstop for the case §2 predicts (50.9 MB of columns at render
  distance 24 at the Far Lands), not a fix for this line. Saying otherwise would be reading the
  fix one wanted into the evidence one has.

## Two things worth chasing next

- **`28.0 KB` per column here, against `21.7 KB` measured through the generator.** The probe
  measures freshly generated columns; these have been lit and ticked, and `NibbleArray::set`
  materialises a plane that `assign` had collapsed and nothing ever collapses it again. **Nothing in
  the game calls `compact()`.** That is 6.3 KB a column if it is the explanation, and the 0.0 %
  figure in §2 does not cover it, because that probe never ran the relighter.
- **`chunk -63 -2000007` is 32 million blocks out** — two and a half times the Far Lands, and well
  past where float precision in the render path was reasoned about. Nothing here says what a column
  costs that far out beyond the 28.0 KB this one line implies, and nothing says what else is
  different out there.
