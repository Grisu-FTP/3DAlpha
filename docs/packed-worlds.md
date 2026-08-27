# Packed worlds

A per-world storage mode, on the world options screen behind `X` in the world list. Both modes are
fully playable; conversion either way is lossless.

| Mode | On disk | Good for |
|---|---|---|
| **Folder** | A real Alpha world — `level.dat`, base36 chunk tree | Dropping in from a PC, copying back out, interchange |
| **Packed** (default for new worlds) | Sector-allocated containers, one per 32×32 chunk region | Playing on the console: measured **9× smaller** on a 16 KB-cluster card, and far fewer file operations |

Drop a folder into `sdmc:/3dalpha/saves/` and play it as-is. Pack it when you want the space back.
Unpack it and the original folder returns byte-for-byte.

**New worlds are created packed.** That reverses what this document originally said, and the reason
is the size table below: a world made on the console has no PC client waiting for it, and the folder
format costs 9× the card for it. A player who wants interchange converts, which is lossless and
takes seconds.

## What "compression" actually buys here

Chunk files are already gzip streams, so re-compressing them gains close to nothing. The waste is
**cluster slack**: the 3DS is FAT32-only, a real card measured **16 KB clusters**, and a median
2,917-byte chunk file consumes a whole one. See [save-data.md](save-data.md).

The folder format pays that slack twice. A leaf directory `<x&63>/<z&63>` also costs a cluster, and
below a 64×64-chunk span **every chunk has its own leaf** — the measured 1,119-chunk world has 1,157
directories to 1,122 files. So the folder cost is roughly `(chunks + leaves) × cluster`, not
`chunks × cluster`, and that is why the saving below is far larger than this document first guessed.

Packing removes both by putting many chunks inside one file, allocated in **1024-byte sectors**.

### Sectors are 1024 bytes, not 4096

Re-derived from the chunk-size distribution rather than borrowed from McRegion. Over the measured
660-chunk world: min 1,194, median 2,917, mean 2,945, max 5,872 bytes. Against a 4,096-byte sector
the median chunk takes two sectors and wastes 5,275 bytes — **53% overhead**, because the
distribution straddles the sector size rather than sitting well inside it. Against 1,024 bytes it
takes three sectors and wastes 155 — **17%**. McRegion's 4 KB is right for chunks an order of
magnitude bigger than a1.1.2's; it is wrong here.

### Measured, on a real 1,119-chunk world

`./build-host/3dalpha --world-info` on both shapes of the same world, with the cluster arithmetic
applied at each plausible card geometry:

| Cluster | Folder | Packed | |
|---|---|---|---|
| 4 KB | 9,609,216 B | 4,079,616 B | 2.36× |
| **16 KB (measured card)** | **37,339,136 B** | **4,145,152 B** | **9.01×** |
| 32 KB | 74,678,272 B | 4,259,840 B | 17.53× |

Content bytes go *up*, 3,378,238 → 4,060,184, which is the 17% sector padding plus the manifest.
The card still gives up a ninth as much, because slack dwarfs padding at these cluster sizes.

So the payloads are stored **verbatim**, exactly the bytes the folder held. That costs no CPU on a
268 MHz ARM11, and it is what makes byte-exact restoration provable rather than hopeful.

An optional second level — recompressing each chunk at a higher gzip level — would gain maybe
10–15% more and give up byte-exact restore. Not worth it by default; noted so the decision is on the
record rather than forgotten.

### The secondary win is speed

Opening a file on the 3DS FS is expensive — 4–6 IPC round trips to the FS sysmodule — and the Alpha
layout means one open per chunk plus a walk through its directory tree. A packed region is **one
open handle, then positional reads**.

For the measured world, reading every chunk costs:

| | file opens | directory listings |
|---|---|---|
| Folder | 1,122 | 1,157 |
| Packed | 4 | 1 |

Those counts are hardware-independent — they are the file and directory counts `--world-info`
reports — and they are the number that matters, because the cost is per operation rather than per
byte. **The wall-clock figure is still owed and can only be taken on hardware**: on a Linux host
with a warm page cache the same walk differs by 8% (0.559 s → 0.517 s meshing the whole world),
which measures the host's cheap `open`, not the console's expensive one. See
[3ds-performance.md](3ds-performance.md).

It also makes the chunk-index cache redundant: for a packed world the region header **is** the
index, so `cache/<world>.idx` isn't built at all.

## The state is the disk, not a setting

There is no stored preference to keep in sync. The mode is observable from the folder's shape:

- `world.3dm` present ⇒ **Packed** (checked first)
- `level.dat` present ⇒ **Folder**
- neither ⇒ not a world

That means a folder copied in from a PC is immediately recognised with no registration step, and the
mode can never desync from reality.

**Packed is checked first** so that a folder holding both — which is not a shape this code ever
writes, and can therefore only be a half-finished conversion — is read as packed. That is the
reading that does not hand a caller a `level.dat` whose chunks have already moved into a manifest.

The world options screen shows the format and the true on-disk size. **Size is computed when that
screen opens, for one world**, never per row of the world list: measuring a folder world is a stat
per chunk file across up to 4,096 leaf directories, and doing it per row would make a card with many
worlds unusable.

## Layout

### Folder mode

Untouched, canonical Alpha level format — see [world-format.md](world-format.md).

### Packed mode

```
sdmc:/3dalpha/saves/<world>/
  world.3dm            manifest + verbatim store for level.dat and any non-chunk files
  r.0.0.3dr            region container, 32×32 chunks
  r.-1.0.3dr
  README.txt           plain text: this is a packed 3DAlpha world, and how to unpack it
```

**A packed world is deliberately not a valid Minecraft world.** `level.dat` moves inside the
manifest. If it stayed on disk, copying a packed world to a PC and opening it in Minecraft would
find a world with no chunks and cheerfully generate new terrain over it. Making the folder
unopenable by Minecraft is the safer failure: the `README.txt` explains what it is.

The manifest carries a small metadata block — world name, last played, seed, chunk count, packed and
unpacked sizes — so the world list renders without opening any region.

## Region container format

Modelled on McRegion's sector scheme, for the same reason Mojang chose it.

```
r.<rx>.<rz>.3dr        one region = 32 × 32 chunks, 1024-byte sectors

sector 0   header A  ─┐ two copies, 44 bytes each: magic "3DR1", version u16,
sector 1   header B  ─┘   sectorBytes u16, regionChunks u16, headerBytes u16,
                          regionX/Z i32, generation u64, dirSlot u32,
                          sectorCount u32, reserved, crc32 at offset 40
sector 2   directory slot A   1024 entries × 16 bytes = 16 sectors
sector 18  directory slot B   (double-buffered)
sector 34+ data sectors

directory entry:
    u32 sectorOffset      0 = chunk absent
    u16 sectorCount
    u16 flags             payload encoding (verbatim gzip)
    u32 byteLength
    u32 crc32             of the payload
```

The header lives in sector `generation & 1`, so the two copies alternate and neither is ever
overwritten while it is the live one.

**Commit protocol** — this is what makes it crash-safe:

1. Write payload sectors (into free space, never over the live copy).
2. Write the updated directory into the **inactive** slot.
3. Write the header with `generation + 1` pointing at that slot, then `flush()`.

On open, take the header copy with the highest `generation` that passes its CRC. A torn header
leaves the previous generation intact, so the worst case of a power cut mid-save is losing the last
save, never the region.

**`commit()` is separate from `saveChunk`**, and the split matters: a commit rewrites 16 KB of
directory plus a header, so doing one per column would cost more write amplification than the format
saves. Chunks accumulate and a commit happens every 64, on eviction from the open-region LRU, on
close, and at each autosave — `ChunkCache`'s housekeeping job calls it right after `saveLevel()`.

### What this does *not* do, deliberately

- **Chunk payloads are not CRC-checked on the gameplay read path.** A payload is a gzip stream and
  inflate already verifies gzip's own CRC-32; adding a second checksum over ~3 KB per chunk load
  would tax normal play for a threat model that was explicitly waived. The `crc32` field is written
  and *is* verified during conversion, where "loses no data" is a promise rather than a hope.
- **The free-sector bitmap is not persisted.** It is derived from the directory at open, which is
  the same information in a form that cannot disagree with itself. A persisted map is one more
  structure a torn write can leave inconsistent with the directory that authorises it.
- **No Compact action.** Sectors freed by a chunk that grew or shrank return to the in-memory free
  map immediately, or are held until the committed generation stops naming them. Fragmentation is
  bounded by that reuse; a region that never leaks does not need compacting.
- **`mtime` is not recorded.** FAT timestamps are 2-second granular and the console's clock is the
  player's; restoring them would be restoring noise. The round-trip test compares *contents*.

### Shared with McRegion later

When the b1.7.3 build needs real McRegion, the sector allocator and the double-buffered commit are
the same machinery. Build it as a reusable utility under the `storage` slot rather than inline in
one implementation — see [build-versions.md](build-versions.md).

## Lossless round-trip

The promise is that unpacking restores the *entire* original folder, so the manifest records every
file the folder had:

```
per stashed file:  path, byteLength, crc32, blob offset
```

**Chunks are deliberately not listed in the manifest.** The regions already know which chunks they
hold, and a second list of the same thing is a second thing that can be wrong. The manifest carries
only what the regions cannot: `level.dat`, and every file the packer did not recognise.

A file counts as a chunk only if its name parses *and* it sits at the canonical path for those
coordinates *and* no other file has claimed that slot. A `c.-d.18.dat` in the wrong leaf directory
is a stray file, not a chunk — tools do produce these, and relocating one silently would change the
world.

`3dalpha.ini` is stashed like anything else **and** put back as a plain readable file on the packed
side, which is what lets the world options screen read a gamemode without opening a container. It is
carried as **raw bytes**, not as a parsed `WorldSettings`: saving rewrites the file from the keys the
running build knows, so a round trip through the struct would quietly drop a key a later build wrote
— exactly the data loss a conversion is not allowed to cause. See
[`core/settings/world_settings.hpp`](../src/core/settings/world_settings.hpp).

Stray files matter. Worlds pick up data from third-party tools and older servers; a "packer" that
silently drops what it doesn't recognise is a data-loss bug waiting to happen. Anything not
identified as a chunk goes into the manifest blob verbatim and comes back at the same path.

Unpack recreates the tree from the manifest and **verifies every CRC before it writes anything**. A
mismatch aborts with the original still intact.

## Conversion, safely

Both directions follow the same rule: **never destroy the source until the destination is complete
and verified.**

1. Check free space with the real cluster size from `FSUSER_GetSdmcArchiveResource()`, reached
   through the `io::VolumeInfo` seam so core never includes `<3ds.h>`. Refuse up front, with the
   actual numbers, if it won't fit. **An unknown cluster size degrades to byte totals and skips the
   refusal** — it must not become a reason a conversion cannot run.
2. Build the new representation into `<world>.converting/`.
3. Verify every CRC, from freshly opened handles rather than from what is still in memory.
4. Write the commit marker, rename into place, then delete the old representation.

**Unpacking does not stage**, and that asymmetry is deliberate. Staging an unpack would mean two
full folder copies on the card at once — the shape whose whole problem is that it is 9× larger than
it needs to be. Instead it writes into the world in place behind a `.3dalpha-unpacking` marker, and
verifies *before* the first write rather than after the last. An interrupted unpack still has a
whole `world.3dm`, so `recoverConversions` rolls it back to packed rather than forward.

Consequences to surface in the UI:

- **Packing needs the packed size free** — roughly a third of the original.
- **Unpacking needs the unpacked size free, including slack**, which can be 3× what the packed world
  occupies. A 200 MB packed world may need ~600 MB. Say so *before* starting, not at 80%.
- Converting 16,000 chunks is not instant. Show progress and allow cancel; cancelling just deletes
  `<world>.converting/`.
- A power cut mid-conversion leaves an unfinished `<world>.converting/` with no commit marker, which
  is discarded on next scan. The original is untouched either way. `listWorlds` never offers a
  directory whose name ends in `.converting`, so a half-written world cannot be played.

**Conversion runs synchronously on the menu thread**, not on the I/O thread as this document first
said. That sentence assumed converting a world from inside a running session; conversion is instead
started from the world list with the world **closed**, so there is no I/O thread to hand it to and
nothing else for the console to be doing. The frame loop lives inside the operation: the observer
callback draws a frame and polls B to cancel, the same shape `Menu::extractJar` already uses.

## Why this doesn't compromise faithfulness

The Alpha level format stays the canonical interchange form; what changed is which one a *new*
world starts in. Packed mode is a reversible, verified choice about how bytes sit on one SD card —
it changes nothing about what the world *is*. Every packed world can be returned to a pristine
folder that a real a1.1.2 client will open, and the round-trip test below is what makes that a fact
rather than a claim.

**One thing it does change, and it is worth stating.** A region listing settles all 1,024 of its
chunks at once, where a folder listing settles a scattered mod-64 set. That changes the *order* in
which the streamer reaches the frontier, and population order is part of an Alpha world: see the
note at the top of
[`impl/worldgen/alpha_nobiome/chunk_generator.hpp`](../src/impl/worldgen/alpha_nobiome/chunk_generator.hpp)
— *"a seed does not determine an Alpha world, and that is the original's property, not ours"*.
Flying the same seed to the same place in both formats gave 891 identical chunks and 7 that differ
by a handful of blocks, all of them where the two runs had populated different neighbours. Each
format is deterministic run to run, reopening a packed world regenerates nothing, and converting an
existing world is byte-exact; it is only *newly generated* frontier that can differ, exactly as it
does between two a1.1.2 clients that walked different routes.

## Test requirements

All of these exist. `tests/region_file_test.cpp`, `tests/packed_storage_test.cpp`,
`tests/convert_test.cpp`.

- **Round-trip on the host**: real alpha world → pack → unpack → compare the whole tree
  byte-for-byte, including files the packer doesn't understand and a world with negative
  coordinates on both axes. Run on the real 1,119-chunk world through
  `./build-host/3dalpha --convert <copy> pack|unpack`; `diff -r` against the original is empty.
- **Torn-write simulation**: a fault-injecting `FileSystem` fails at each stage of the commit
  protocol; the previous generation still loads.
- **Free-space refusal**: conversion refuses rather than half-fills a card, and writes nothing at
  all when it does.
- **Cancellation and power-loss**: `<world>.converting/` is discarded and the original opens
  normally; an interrupted unpack rolls back to the packed world.
- **The group key is 64 bits wide.** `tests/chunk_cache_test.cpp` streams two regions whose keys
  collide in their low 32 bits. A narrowed queue element makes them one group, and the cache then
  reports chunks that exist as absent — which regenerates terrain over a world that was there.
