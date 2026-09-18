# 010 — the CIA had no DSP memory, and the 3DSX never needed to ask

**Verdict: settled, and fixed in `packaging/3dalpha.rsf.in`.** This is the first dump in the set
that is about *packaging* rather than about our code. The same ELF that runs from hbmenu dies on
launch as a CIA, and nothing in the binary differs between the two.

## What the dump says

    ARM11 core 0, data abort
    pc = 0x00287008   ndspSetCounter  (inlined) -- ndsp.c:60, from ndspInitialize at ndsp.c:273
    lr = 0x0028A4B4   DSP_SetSemaphore -- dsp.c:160
    r2 = 0x1FF57FFE   far = 0x1FF57FFE
    -> translation fault (page), on write to 0x1FF57FFE

`process: 3DAlpha`, so it is ours, and the ELF beside this file is the one that produced it — its
bytes at `0x00286FAC..0x00287008` are the dump's `code` block verbatim, checked before anything was
concluded from the addresses.

The faulting instruction and the four before it:

    286ff4: e59f2104   ldr   r2, [pc, #260]   @ -> &frameCount
    286ff8: e59f4104   ldr   r4, [pc, #260]   @ -> &ndspFrameId
    286ffc: e5821000   str   r1, [r2]         @ frameCount = 0
    287000: e5982000   ldr   r2, [r8]         @ r2 = ndspVars[0][0]
    287004: e1c430b0   strh  r3, [r4]         @ ndspFrameId = 4     -- fine, ordinary .bss
    287008: e1c230b0   strh  r3, [r2]         @ *ndspVars[0][0] = 4 -- faults

`r8 = 0x003127A8` is `ndspVars` in our own `.bss`; `nm` names it. The *pointer it holds* is what is
unmapped. `ndspVars` was filled a few instructions earlier, in the loop at `286FB4`, by
`DSP_ConvertProcessAddressFromDspDram`.

## Why nothing failed before the fault

`DSP_ConvertProcessAddressFromDspDram` is arithmetic on the service side —
`arm_addr = (dsp_addr << 1) + 0x1FF40000`. Check it against the faulting address:
`(0x1FF57FFE - 0x1FF40000) >> 1 = 0xBFFF`, a plausible DSP DRAM offset. **It succeeds whether or
not the caller can reach the address it returns**, so `ndspInit` gets a clean `Result`, stores the
pointer, and writes through it.

That is the trap in the shape of this bug. `platform/ctr/audio.cpp:37` handles `ndspInit` failing —
that is the missing-`dspfirm.cdc` path, and it is correct — but this is not a failure. It is a
store to an address the kernel never mapped, so the graceful path never runs. **A player with no
`dspfirm.cdc` would have booted fine**, because `ndspInit` would have failed long before this line.
The CIA only dies for players whose audio was going to work.

## The cause

3dbrew's memory layout, on the 0x1FF00000 region:

> 0x1FF00000 | 0x00080000 | DSP memory
> "DSP memory, access to this is specified by the exheader."

Our RSF granted the `dsp::DSP` *service* and never mapped the *memory*. A 3DSX does not need it:
the Homebrew Launcher runs us inside a host title whose exheader already carries the mapping, so
every hardware test of the audio path to date has been run under someone else's permissions. A CIA
is its own process with its own exheader, and ours had no memory mappings of any kind.

The fix is two lines in `packaging/3dalpha.rsf.in`:

    IORegisterMapping:
      - 1ff00000-1ff7ffff

`IORegisterMapping`, not `MemoryMapping`: the DSP writes this region concurrently with the ARM11,
so it must be mapped uncached. This is also what the standard homebrew RSF template does with it
(`Steveice10/buildtools`, `3ds/template.rsf`, commented `# DSP memory`).

## Verified, without a console

`makerom` is not installed on the developer machine, so it was built from the tag CI pins
(`makerom-v0.19.0`) and two CIAs were made from the *same* ELF, one per RSF. Scanning both for
ARM11 kernel capability descriptors with the `0b11111111100x` "map address range" prefix:

| image | descriptors found |
|---|---|
| RSF before the fix | none |
| RSF after the fix | `0xFF81FF00`, `0xFF81FF80` (twice — exheader and access descriptor) |

Decoded: page index `0x1FF00` → `0x1FF00000`, bit 20 clear → read-write; exclusive end `0x1FF80`
→ `0x1FF80000`, bit 20 clear on the second descriptor → IO rather than static, i.e. uncached.
Exactly the range and the caching the fault demanded.

**What is not verified is the console.** The reasoning above closes the loop from the faulting
store to the exheader bit that permits it, but nobody has yet launched the fixed CIA. The thing to
watch for is a *second* mapping of the same kind rather than a repeat of this one.

## VRAM was considered and deliberately left out

The canonical template also carries `MemoryMapping: 1f000000-1f5fffff:r` for VRAM, and the same
argument would apply to it — a CIA does not get it for free either. It is not added, because
nothing here CPU-touches VRAM: `gpu_memory.cpp`'s VRAM tier is switched off (see `002`), and
`textures.cpp` checks every upload destination against `[0x1F000000, +0x600000)` and routes VRAM
through `C3D_SyncTextureCopy` rather than `memcpy`. `vramAlloc` itself keeps its bookkeeping on the
ordinary heap, and the GPU reaches VRAM by physical address without the MMU. Adding the mapping
would have been a guess dressed as a fix; if a VRAM read ever appears, this is the line to add.
