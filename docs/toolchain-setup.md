# Toolchain setup

## devkitPro — installed

Verified on this machine (devkitPro repos added to the system pacman, `3ds-dev` installed):

| Tool | Version |
|---|---|
| `arm-none-eabi-gcc` | devkitARM 16.1.0 |
| `picasso` | 2.7.2 |
| libctru / citro3d / citro2d | present in `/opt/devkitpro/libctru` |
| `smdhtool`, `3dsxtool`, `3dslink`, `tex3ds`, `bin2s`, `mkromfs3ds` | `/opt/devkitpro/tools/bin` |

On Arch-family systems the packages come from the devkitPro repo per the
[official instructions](https://devkitpro.org/wiki/devkitPro_pacman).

Then, in your shell profile:

```sh
set -gx DEVKITPRO /opt/devkitpro
set -gx DEVKITARM /opt/devkitpro/devkitARM
set -gx PATH $DEVKITPRO/tools/bin $DEVKITARM/bin $PATH
```

(This machine uses fish; adapt for other shells.)

Verify:

```sh
arm-none-eabi-gcc --version
picasso --version
ls $DEVKITPRO/libctru/lib/libcitro3d.a
```

## makerom and bannertool — NOT in devkitPro

`.3dsx` builds work with what's installed. **CIA packaging does not**: devkitPro does not distribute
`makerom` or `bannertool`, so neither is on this machine. Both come from
[3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR) (or a maintained community fork) and go
somewhere on `PATH`.

This matters as soon as multiple versions are in play: `.3dsx` files coexist by filename, but
installing several versions onto the HOME menu at once requires CIAs with distinct title IDs — see
[build-versions.md](build-versions.md). The build must therefore treat CIA output as optional and
skip it with a clear message rather than failing, so a fresh clone still builds something runnable.

## Portlibs we rely on

```sh
sudo dkp-pacman -S 3ds-zlib 3ds-libpng
```

`miniz` and `lodepng` are vendored in-tree instead (single-file, no build-system friction, and we
need miniz's zip reader for texture packs anyway). **libdeflate** is vendored too, for the inflate
benchmark described in [3ds-performance.md §8](3ds-performance.md).

## Host build

The core also builds for Linux, which is where most development and all unit testing happens. It
needs a C++17 compiler, CMake, SDL2 and an OpenGL 3.3 loader:

```sh
sudo pacman -S cmake sdl2 mesa
```

## Building

The 3DS build is **per game version** — see [build-versions.md](build-versions.md).

```sh
# 3DS: produces build/a1.1.2/3DAlpha-a1.1.2.{3dsx,cia}
make VERSION=a1.1.2
make all-versions

# Host: unit tests and the SDL2 desktop build
cmake -B build-host -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANITIZE=ON
cmake --build build-host
ctest --test-dir build-host
```

The 3DS target uses a Makefile based on devkitPro's `3ds_rules` (citro3d's and craftus's Makefiles
are good templates). The host target uses CMake. They share the same source lists via a generated
file so neither can silently drift.

## Packaging: the RSF file

An RSF template drives `makerom`; `tools/configure.py` substitutes the per-version `Title`,
`ProductCode` and `UniqueId` into it. The shared settings that matter for performance:

```
SystemModeExt  : 124MB    # New 3DS extended memory
CpuSpeed       : 804MHz   # New 3DS clock
EnableL2Cache  : true     # New 3DS L2
CanAccessCore2 : true     # exclusive extra core for the chunk worker
MemoryType     : Application
StackSize      : 0x40000

FileSystemAccess:
  - DirectSdmcWrite       # required to read/write sdmc:/3dalpha from a CIA
```

Recent Luma3DS no longer forces the New 3DS clock from RSF alone, so also call
`osSetSpeedupEnable(true)` at runtime. Both, not either.

Old 3DS application memory stays at the 64 MB default unless measurements force otherwise; raising
it via the exheader reduces compatibility and should be a deliberate, measured decision.

## Running

| Target | How |
|---|---|
| Emulator | **Azahar** — the maintained fork of Citra, which is discontinued. Fast iteration, GPU debugging, but its timing is not the hardware's. |
| Hardware (3dsx) | Copy `3DAlpha.3dsx` to `sd:/3ds/` and launch from the Homebrew Launcher. |
| Hardware (cia) | Install `3DAlpha.cia` with FBI. **Required** for the New 3DS clock/memory settings — the Homebrew Launcher path does not apply the exheader. |
| Net loading | `3dslink` over Wi-Fi to hbmenu, the fastest hardware iteration loop. |

Performance numbers only count from **hardware**, and from **both** models — the memory and clock
envelopes differ enough that testing one will mislead you about the other.

## Debugging

- `Crash`-style handler that dumps registers and a stack trace to `sdmc:/3dalpha/crash.txt`
  (craftus's `misc/Crash.c` is a good model).
- Logging to SD behind a compile-time switch; SD writes are slow enough to change what you are
  measuring.
- The in-game debug overlay ([3ds-performance.md](3ds-performance.md)) is the primary profiling tool
  on hardware.
- For logic bugs, reproduce on the host build under ASan/UBSan first. That is the entire reason the
  host target exists.
