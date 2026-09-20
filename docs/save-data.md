# Save data: where worlds live and what it costs

Short version: worlds are **ordinary files on the SD card**, not 3DS title save data. There is no
per-world size limit and no practical world-count limit — but FAT32 cluster granularity makes an
Alpha-format world cost far more on disk than its contents suggest, and that is the number to plan
around.

## Why not 3DS title save data

The 3DS gives a title an encrypted save archive whose size is fixed at build time in the RSF
(`SaveDataSize`), stored under `Nintendo 3DS/<id0>/<id1>/title/…` and encrypted with console-unique
keys. Every property of it is wrong for us:

| Property | Consequence |
|---|---|
| Fixed size declared at build time | Worlds grow without bound; you cannot pick a number |
| Console-bound encryption | A world could not be copied to a PC or another console |
| Opaque image | Breaks the project's core requirement — real a1.1.2 worlds, readable both ways |
| Typically 128 KB – 1 MB | Off by three orders of magnitude |

So `SaveDataSize` stays minimal and unused. Worlds go on the SD card as plain files, in the real
Alpha level format:

```
sdmc:/alpha/saves/<world>/level.dat
sdmc:/alpha/saves/<world>/session.lock
sdmc:/alpha/saves/<world>/<b36>/<b36>/c.<b36>.<b36>.dat
```

Copy that folder into a PC `.minecraft/saves/` and it just works — which is the whole point. CIA
builds need `FileSystemAccess: DirectSdmcWrite` in the RSF to write there.

## The real cost: FAT32 cluster slack

The 3DS is **FAT32 only** — the hardware cannot read exFAT, so SDXC cards must be reformatted.
Common allocation unit sizes are 32 KB for cards under 128 GB and 64 KB above, but this varies with
how the card was formatted: **a real card measured by the M0 probe reported 16 KB.** Do not assume a
value — the spread across plausible cards is 4×, which is the difference between a comfortable world
and a full SD card.

The Alpha format stores **one file per chunk column**. A file smaller than a cluster still consumes
a whole cluster, and alpha chunks are far smaller than a cluster. Measured over a real a1.1.2 world
of 660 explored chunks:

| Gzipped chunk file | Bytes |
|---|---|
| smallest | 1,194 |
| median | 2,917 |
| mean | 2,945 |
| largest | 5,872 |

An earlier version of this document estimated 8–20 KB. That was a guess and it was wrong by 3–4×,
which makes the slack **worse** than it claimed, not better: at a 32 KB cluster the same world
occupies **20.7 MB on disk for 1.85 MB of data — 11.1×**. Every chunk fits in one cluster with room
to spare, so:

> **On-disk cost ≈ (number of chunks ever visited) × (cluster size)** — almost entirely independent
> of how well the data compresses.

| Explored area | Chunks | @ 16 KB | @ 32 KB | @ 64 KB |
|---|---|---|---|---|
| 512 × 512 blocks | 1,024 | 16 MB | 32 MB | 64 MB |
| 1024 × 1024 | 4,096 | 64 MB | 128 MB | 256 MB |
| 2048 × 2048 | 16,384 | 256 MB | 512 MB | 1 GB |
| 4096 × 4096 | 65,536 | 1 GB | 2 GB | 4 GB |

A well-explored world reaching a gigabyte is entirely normal, and roughly **11× the size of its
actual content** at a 32 KB cluster — around 5× at 16 KB, 22× at 64 KB. This is not a bug in our
implementation; it is exactly why Mojang replaced the format with McRegion in Beta 1.3, and it is
inherited along with the compatibility we asked for.

Read the cluster size at runtime. `FSUSER_GetSdmcArchiveResource()` returns it, along with free
space:

```c
FS_ArchiveResource res;   // { sectorSize, clusterSize, totalClusters, freeClusters }
FSUSER_GetSdmcArchiveResource(&res);
```

Use the real `clusterSize` to report a world's true on-disk footprint in the world list, and
`freeClusters` to show remaining space and refuse to open a world when the card is nearly full.

### What is *not* a limit

- **FAT32's 4 GiB file cap** — irrelevant; chunk files are kilobytes.
- **Files per directory.** FAT32 allows 65,536 directory entries, and `c.-d.18.dat` needs about two
  (it isn't 8.3-compatible, so it takes a long-filename entry plus the short one) — roughly 32,000
  files per directory. The Alpha layout spreads chunks across 64 × 64 = 4,096 leaf directories by
  the low six bits of the coordinates, so a leaf directory only collects chunks spaced 64 apart. A
  world spanning ±1,024 chunks puts about 1,024 files in each leaf — two orders of magnitude clear.
- **Files per volume** — FAT32 tops out in the hundreds of millions.

## How many worlds

`saves/` holds one directory per world, each costing a few directory entries, so the filesystem
allows thousands. The binding limits are SD free space and the console's own sluggishness with very
full cards.

Practically: dozens to low hundreds of worlds is comfortable. The world-select screen should page
rather than build one giant list, show each world's true on-disk size, and show SD free space.

Worlds live under one shared `saves/` root across all game-version builds, but each build only lists
worlds in a format it owns — see [build-versions.md](build-versions.md).

## Checkpoint and System Transfer

**Checkpoint will not back up worlds.** It manages title save data and extdata only, writing to
`sdmc:/3ds/Checkpoint/saves/<unique id> <title>` and `…/extdata/…`. Our worlds are ordinary SD files
in no save archive, so Checkpoint has nothing to find.

Its `additional_save_folders` config option looks like an escape hatch but isn't: reading
`3ds/source/configuration.cpp` and `title.cpp`, those entries extend the list of directories
Checkpoint searches for **existing backups** (so it can restore ones made by other tools), and the
lookup is gated on the title having an accessible save archive. It does not let you nominate an
arbitrary SD directory as save data.

**System Transfer moves the console's `Nintendo 3DS` folder, not our files.** Everything on the SD
card outside `Nintendo 3DS/` and `DCIM/` — including `sdmc:/alpha/` — is not part of the transfer;
the standard procedure is to copy those folders to a PC and back onto the target card by hand.
System Transfer also wipes the source console.

None of this is a problem, and it is the direct benefit of not using save data:

| Task | How |
|---|---|
| Back up a world | Copy the folder. SD reader, or `ftpd` over Wi-Fi. |
| Move to another console | Move the SD card, or copy `alpha/` across. Nothing is console-bound. |
| Move to/from a PC | Drop the folder into `.minecraft/saves/`. It is a real alpha world. |
| Survive a System Transfer | Copy `alpha/` manually, like all other homebrew data. |

Had we used title save data, every one of those rows would have required Checkpoint and
console-bound decryption.

## Autosave: what the original does, and what we do

**a1.1.2 has no timed autosave.** Taken from the client jar rather than assumed:

- `ft.a(boolean, nu)` — `ChunkProviderLoadOrGenerate.saveChunks(saveAll, progress)` — writes at
  most **two dirty chunks per call** when `saveAll` is false. The relevant bytecode is
  `iinc 3,1; iload_3; iconst_2; if_icmpne; iload_1; ifne; iconst_0; ireturn` at offset 145, which is
  `if (++saved == 2 && !saveAll) return false`.
- Its only periodic caller is the **"Saving level.." screen**, which calls `cn.a(int)`
  (`World.quickSaveWorld`) once per *rendered frame* until it returns true. That screen is reached
  from "Save and quit to title" and from nowhere else.
- Otherwise a chunk is written **when it is evicted** from the provider's chunk table — `ft` holds
  `new ga[1024]`, a 32×32 direct-mapped cache indexed by `(x & 31) + (z & 31) * 32`, and putting a
  chunk into an occupied slot saves and drops the previous occupant. Synchronously, on the main
  thread. That is the original's own version of the stutter this project has been chasing.

So the autosave interval in `3ds.ini` is **ours, not a port**, and the default is a judgement rather
than a recovered constant: 45 seconds. What it covers is everything: dirty columns, `level.dat` —
which now carries the player's position, rotation and the world clock rather than only `LastPlayed`
— and the `session.lock` refresh. Opening the pause menu does the same thing at a moment of the
player's choosing, and leaving the world does it blocking.

**Nothing is written outside those points.** That is the original's shape, and it was arrived at by
reversing the opposite choice. Writing a generated column eagerly is *safer* than a1.1.2: population
passes spill across chunk borders, so a column lost to a crash and later regenerated against
neighbours already marked `terrainPopulated` comes back missing whatever their passes had put into
it. But the original is more exposed to exactly that, not less — its table only evicts when
something 32 chunks away collides with a slot — so eager writing was safety a1.1.2 does not have,
bought with a deflate and six file operations per finished column. The interval bounds the exposure
instead. The sole exception is memory: a dirty column cannot be evicted, since it is the only copy
of that part of the world, so past the dirty budget whoever dirtied it writes one itself.

The 1024-slot table is also the precedent for our own chunk cache: same idea, byte-capped and LRU
instead of a fixed direct-mapped grid, and it sits below the generator rather than being it. See
`src/core/world/chunk_cache.hpp`.

## Internal storage is not a second tier

Asked and answered, because it looks like an obvious win and is not:

- **Title save data and extdata for an SD-installed title live on the SD card**, under
  `Nintendo 3DS/<id0>/<id1>/`. Same medium, same FS sysmodule, worse API. The format objections
  above are a second, independent reason.
- **CTRNAND is not writable from a 3DSX.** It needs an exheader granting NAND read/write; Luma's
  synthesised 3DSX exheader grants `DirectSdmcWrite` and not that. Free space on a normal system is
  tens of megabytes, and filling or corrupting it is how a console bricks.
- **There is no throughput to win.** Both media go through the same IPC path, and NAND traffic is
  additionally AES-encrypted per block by the hardware engine. The bottleneck is per-operation
  latency, not bandwidth — a chunk file is 2,917 bytes at the median, so even at a pessimistic
  5 MB/s the transfer is under a millisecond against four to six IPC round trips.

The useful version of the idea — a buffer zone wider than what is drawn, filled ahead of the player
— is real and is built, in RAM. See the chunk cache.

## Implementation requirements

- **Atomic writes.** FAT32 has no journal and a 3DS can be switched off mid-write. Write
  `c.x.z.dat.tmp`, close it, then rename over the target. Same for `level.dat` — never rewrite it in
  place, or a bad moment costs the player their spawn, inventory and seed.
- **Only save dirty chunks.** Note that the claim this used to make about the original — that it
  "saved everything loaded on a timer" — is wrong; see the autosave section above. The requirement
  stands on its own: rewriting unchanged data is needless flash wear either way.
- **All of it on the I/O thread, behind a write-back cache.** `core/world/chunk_cache.hpp` is the
  only thing in the process that touches the storage slot, and the render thread's half of its API
  never blocks on it.
- **Few open handles.** Open, read, close per chunk. Do not hold thousands of files open.
- **Chunk index cache.** Enumerating 4,096 subdirectories per world is slow; build the index once
  and cache it (see [world-format.md](world-format.md)). It must always be rebuildable, because it
  is a cache and the card may be edited on a PC. **Not needed for a packed world** — the region
  directory *is* the index, and one 16 KB read settles all 1,024 of its chunks.
- **`session.lock`** on open, refreshed periodically, as the original does. The autosave timer is
  what runs the refresh; before it existed, `refreshLock()` was written and never called.
- **Free-space guard.** Check `freeClusters` before a save flush; warn early rather than failing
  halfway through writing a world.

## Getting the slack back: packed worlds

Storage mode is a **per-world setting**, on the world options screen behind `X` in the world list,
and both modes are fully playable:

- **Folder** — a real Alpha world. Copy it in from a PC, play it, copy it back out. Pays the
  cluster slack above, **twice**: once per chunk file and once per leaf directory, since below a
  64×64-chunk span every chunk sits alone in its own leaf.
- **Packed** (what new worlds are created in) — chunks live in sector-allocated region containers
  instead of one file each, in 1024-byte sectors. Measured on a real 1,119-chunk world at a 16 KB
  cluster: **37.3 MB → 4.1 MB, 9× smaller**, and reading the whole world drops from 1,122 file
  opens and 1,157 directory listings to 4 and 1.

Converting either way is lossless and verified; unpacking restores the original folder
byte-for-byte, so nothing about interchange is given up. The mode is read off the disk rather than
stored as a preference, so a folder dropped in from a PC is recognised with no setup.

Full design, container format and crash-safety protocol: [packed-worlds.md](packed-worlds.md).

## One more feature worth adding

**Export over Wi-Fi** — once the socket stack exists for multiplayer (M5), serving a world over HTTP
or FTP costs very little and removes the need to take the SD card out at all.
