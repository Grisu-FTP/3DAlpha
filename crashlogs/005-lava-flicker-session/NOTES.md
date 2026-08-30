# 005 — three dumps from the session that reported flicker and a lava refresh loop

Filed 2025-08-30. The ELF beside them is the build that produced them
(`build/a1.1.2/3DAlpha-a1.1.2.elf`, 13:03; dumps 13:21, no source changed between), so the
addresses below resolve honestly rather than plausibly.

    tools/lumadump.py crashlogs/005-lava-flicker-session/*.dmp \
        --elf crashlogs/005-lava-flicker-session/3DAlpha-a1.1.2.elf

## What they say

| Dump | pc | Resolves to |
|---|---|---|
| 07 | `0x0015F228` | `JavaRandom::setSeed` inlined into **`ChunkProvider::ChunkProvider`** — i.e. `make_unique<ChunkGenerator>` in `WorldStreamer::open`. Data abort **writing** `this+4` at `0x09CA8898`. |
| 08 | `0x00190D88` | `svcWaitSynchronizationN` from libctru's **`aptEventHandler`**. Data abort writing `0x080091DC`, which is `sp + 0x14`. |
| 09 | — | **Byte-identical to 08.** Same registers, same pc; only `ifsr` differs. One event, dumped twice. |

## What is *not* established

**All three report `stack: EMPTY -- the kernel could not read sp`.** Every one of them faulted with
its own stack pointer in unmapped memory, which is why there is no call chain under any of them.
That is the signature of the process's memory map being gone, not of a particular line: dump 08/09
is libctru's own thread failing to write its own stack frame, and libctru is not where a bug of ours
lives.

So these are consistent with **memory exhaustion or corruption**, and they do not name a cause. Two
of the three are the same event, so there are really two data points, one of which is in a
system-library thread.

Dump 07 is the interesting one and the only one in our code: it is at **world open**, constructing
`ChunkGenerator` — a ~900 KB object (`ChunkProvider` ~300 KB plus `LightEngine` 576 KB, per
`chunk_generator.hpp`). A write to a freshly allocated object landing in an unmapped section is what
a heap that has run out, or been corrupted, looks like from inside.

## What was found and fixed off the back of them

Not by reading the dumps -- by auditing what had changed. Recorded here so the next reader does not
re-derive it:

- **`ChunkCache::save`'s `SavePressure::Defer` had no ceiling.** Taking the main thread off the
  write path removed the only back-pressure on the dirty set, and a dirty column cannot be evicted
  because it is the only copy of that world. Relighting made this much worse by marking a column
  dirty for every section whose light moved. The set could grow without bound -- the same leak the
  cache had before (11.28 MB against a 4 MB cap), reintroduced from a different direction. It now
  pays for one column itself past a ceiling. See `deferCeilingLocked`.

That is a real unbounded-growth path on the console, and it is the best available candidate for a
heap that ran out. **It is not proof.** If it crashes again, what would settle it is the Info page's
storage figures at the moment before -- `owed to the card` against its cap -- and the free-linear and
free-heap numbers beside them.

## The lesson this set repeats

Two of three dumps were the same event. Copy the whole `luma/dumps/<process>/` directory rather than
picking files, and note what the console was doing -- "flying over a lava pool", "walking into a new
world" -- because a dump with an unmapped stack carries no other context.
