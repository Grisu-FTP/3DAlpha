# 011 -- CIA: the Import World keyboard reads VRAM it has no mapping for

"Import world causes an exception with fault status translation - section for the access type
read in the .cia version." Same build from hbmenu: fine.

## The ELF is a rebuild, and it was checked

The installed CIA was built at 11:27 from `5da0f66`, and a later `make` overwrote its ELF before
anything had crashed. That commit was rebuilt in a clean worktree and packaged again. The two CIAs
are the same size and differ only in 600 bytes inside the first 15 KB, which are makerom's
signatures and hashes. The compressed `.code` is byte-identical, so the ELF here resolves the dump.

## The dump

    tools/lumadump.py crash_dump_00000002.dmp --elf 3DAlpha-a1.1.2.elf   (full output: analysis.txt)

- Data abort, translation fault (section), read of `0x1F4C7800`. That is the VRAM window.
- `pc` is in libctru 2.7.0's `aptConvertScreenForCapture` (apt.c:729), on `ldrb r10, [r3], #1`,
  the read from `src`. `r3 == far`.
- The stack holds the UTF-16 hint "Name for the world you are importing", so this is
  `Menu::askImportName`'s `swkbdInputText`.
- The `GSPGPU_CaptureInfo` that `aptScreenTransfer` imported is also on the stack, at
  `0x080061C8`:

| screen | framebuf0 | framebuf1 | format | line bytes |
|---|---|---|---|---|
| top | `0x1F273000` | `0x1F2B9800` | `0x141` | 720 |
| bottom | `0x1F4C7800` | `0x14119400` | `0x102` (RGB565) | 480 |

  The call that faulted is the bottom screen's (height `0x140` on the stack). It reads
  `framebuf0` unconditionally.

## Why a CIA and not a 3DSX

Launching a library applet makes libctru copy both screens into the capture block itself, reading
whatever `framebuf0` GSP reports for each. Three of those four addresses are in VRAM, but
`gfxInitDefault` allocates this program's framebuffers in linear memory, and nothing in `src/`
calls `vramAlloc` for a screen. The VRAM addresses are therefore probably not our buffers:
probably stale entries left by whatever last used that slot. **That part is inferred, not shown.**

What is certain is the mapping. The Homebrew Launcher's host title maps VRAM read-only, so a 3DSX
survives the same read. The CIA's RSF carried the DSP mapping from `010` but not the VRAM one that
sits next to it in the standard homebrew template (`Steveice10/buildtools`, `3ds/template.rsf`,
`1f000000-1f5fffff:r # VRAM`). Why only this keyboard and not Create World's: not established.
The capture reads whatever GSP reports at that moment, and this keyboard opens directly from the
world list, while the table preview owns the bottom screen.

## Fix

`packaging/3dalpha.rsf.in`: `MemoryMapping: - 1f000000-1f5fffff:r`. Read-only on purpose: the CPU
still must not write VRAM (`002`, `gpu_memory.cpp`).

Verified the way `010` was, by scanning the CIA for ARM11 "map address range" descriptors:

| image | descriptors |
|---|---|
| before | `0xFF81FF00`, `0xFF81FF80` (DSP) |
| after | those, plus `0xFF91F000`, `0xFF91F600` -- in both the exheader (`0x3E88`) and the access descriptor (`0x4288`) |

Page `0x1F000`, bit 20 set: read-only. Exclusive end `0x1F600`, bit 20 set: static. **Not yet
launched on a console.**

## Dump 3: the old CIA again, not the fix failing

`crash_dump_00000003.dmp`, reported as "same crash" after the fix was built. It *is* the same
crash, from the same binary. Its pc (`0x002A1B1C`) and the `aptScreenTransfer` return address on
its stack (`0x002A2BC0`) fit `5da0f66`'s layout. In the fixed build, `aptConvertScreenForCapture`
starts at `0x2A1A90`, not `0x2A1A50`, and that return address would be `0x2A2C00`. So the
console was still running the old CIA. The fixed CIA (md5 `e2df25ab20b5a3201bf73f10a68d2c05`)
still carries the VRAM descriptors. **The fix has still not been launched.** Resolved against
this folder's ELF in `analysis-dump3.txt`.
