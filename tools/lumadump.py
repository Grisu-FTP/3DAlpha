#!/usr/bin/env python3
"""Decode a Luma3DS exception dump and name the code it crashed in.

    tools/lumadump.py <dump.dmp> ...  [--elf build/a1.1.2/3DAlpha-a1.1.2.elf]

Luma writes these to `luma/dumps/<process>/crash_dump_XXXXXXXX.dmp` on the SD
card whenever a process takes an exception. They are small, undocumented and
entirely mechanical: a 40-byte header, then registers, then the instructions
ending at PC, then as much of the stack as the kernel could read.

Pass `--elf` and every address that lands inside our binary is resolved to a
file and line. That works with no extra tooling because a 3DSX is loaded at
0x00100000 and the linker already put .text there, so the addresses in the dump
and the addresses in the ELF are the same numbers -- no rebasing, no map file.

**It must be the ELF that produced the crash.** Nothing checks this and nothing
can: any build resolves any address to *some* plausible function, so a rebuilt
binary answers confidently and wrongly. Keep the .elf beside the .dmp, or
resolve nothing.

Two fields carry most of the diagnosis and neither is obvious:

  - **An empty stack dump is a finding, not a gap.** Luma dumps the stack by
    reading from `sp`. Zero bytes means `sp` itself is unmapped, which is a
    stack overflow or a corrupted `sp` and nothing else.
  - **DFSR says what kind of bad access it was.** Its low bits are the fault
    type and bit 11 is read-vs-write, so "translation fault, write" plus a FAR
    a few words from `sp` is an overflowing prologue rather than a wild pointer.
  - **An undefined instruction is usually not undefined.** `0xE7F000F0` is
    `udf #0`, which GCC emits for `__builtin_trap()`, and every Luma sysmodule
    aborts through a `panic()` that is exactly that one instruction and nothing
    else. So a dump from `loader` is not a system module having a bad day: it is
    Luma refusing to load something and having no channel to say so, with the
    Result still sitting in `r0`.

The 3DS toolchain ships no reader for this format, and the register order below
is the order Luma's handler pushes them in.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys

REGNAMES = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
            "r11", "r12", "sp", "lr", "pc", "cpsr", "dfsr", "ifsr", "far",
            "fpexc", "fpinst", "fpinst2"]

EXCEPTIONS = ("FIQ", "undefined instruction", "prefetch abort", "data abort")

# DFSR/IFSR status, ARMv6 encoding: bits [3:0] with bit 10 as the high bit.
FAULTS = {
    0b00001: "alignment fault",
    0b00010: "debug event",
    0b00011: "access flag fault (section)",
    0b00100: "instruction cache maintenance fault",
    0b00101: "translation fault (section)",
    0b00110: "access flag fault (page)",
    0b00111: "translation fault (page)",
    0b01000: "precise external abort",
    0b01001: "domain fault (section)",
    0b01011: "domain fault (page)",
    0b01100: "external abort on translation (first level)",
    0b01101: "permission fault (section)",
    0b01110: "external abort on translation (second level)",
    0b01111: "permission fault (page)",
    0b10110: "imprecise external abort",
}

# A 3DSX is loaded here, and the ELF is linked here, so dump addresses index
# straight into the ELF.
CODE_BASE = 0x00100000
CODE_LIMIT = 0x08000000

# Luma links its *own* sysmodules at 0x14000000 (`-Wl,--section-start,.text=`
# in sysmodules/*/Makefile), so a pc up there is Luma's code and no --elf will
# ever resolve it. Recognising the address is most of not chasing it.
SYSMODULE_BASE = 0x14000000

# `panic()` in sysmodules/loader/source/util.h is one instruction --
# `__builtin_trap()`, which GCC emits as this word -- and it is `noinline`, so
# it receives its argument in r0 and never disturbs it. Everything loader
# decides not to survive ends here, which is why a homebrew image that loader
# *rejected* and one that *crashed* look identical from the outside: black
# screen, red text, a dump belonging to `loader`.
ARM_TRAP = 0xE7F000F0

# hb:ldr launches every 3DSX under this title id, so finding it in the
# registers says the panic came from the homebrew path rather than from
# loading some ordinary title.
HBLDR_TID = (0x0D921E00, 0x00040000)

# Luma reimplements these, and every one of them aborts through the same
# `panic`. Only `loader` is linked at 0x14000000; the rest sit at 0x00100000
# like any sysmodule, which is exactly where our own 3DSX sits -- so for those
# the process name is the only thing keeping `--elf` honest.
LUMA_SYSMODULES = ("loader", "pm", "sm", "pxi", "rosalina", "err:f")

# The Results that reach `panic` from `LoadProcess`'s
# `assertSuccess(hbldrLoadProcess(...))`, and what each one means. Both are
# about the *file*, not about anything our code did -- at this point our code
# has not run.
LOADER_PANIC = {
    0xFFFFFFFF:
        "Ldr_Get3dsxSize returned false: loader opened the .3dsx and could not\n"
        "     read its 32-byte header, or the magic was not '3DSX'. The file on\n"
        "     the card is empty, truncated or not a 3DSX at all.",
    0xD96043FA:
        "Ldr_CodesetFrom3dsx returned 0: the header read but a segment or a\n"
        "     relocation table did not. The same cause one step later -- the file\n"
        "     is long enough to describe itself and too short to be itself.",
}


class Resolver:
    """Addresses to file:line, via addr2line, or a no-op without an ELF."""

    def __init__(self, elf):
        self.elf = elf
        self.tool = None
        if elf:
            for candidate in ("arm-none-eabi-addr2line",
                              "/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line"):
                if shutil.which(candidate) or os.path.isfile(candidate):
                    self.tool = candidate
                    break
            if self.tool is None:
                print("warning: arm-none-eabi-addr2line not found, "
                      "addresses left raw", file=sys.stderr)

    def __call__(self, addr):
        if self.tool is None or not (CODE_BASE <= addr < CODE_LIMIT):
            return None
        out = subprocess.run([self.tool, "-f", "-C", "-i", "-e", self.elf, hex(addr)],
                             capture_output=True, text=True).stdout.strip().splitlines()
        if not out or out[0] == "??":
            return None
        # addr2line alternates function, location, innermost frame first.
        return "  ".join(f"{out[i]} at {out[i + 1]}" for i in range(0, len(out) - 1, 2))


def parse(path, resolve):
    data = open(path, "rb").read()
    if len(data) < 40 or struct.unpack_from("<2I", data) != (0xDEADC0DE, 0xDEADCAFE):
        sys.exit(f"{path}: not a Luma3DS exception dump")

    major, minor = struct.unpack_from("<2H", data, 8)
    processor, exc_type, total, reg_size, code_size, stack_size, extra_size = \
        struct.unpack_from("<7I", data, 12)

    off = 40 + reg_size + code_size + stack_size
    procname = data[off:off + extra_size].split(b"\0")[0].decode("ascii", "replace")

    print(f"=== {path} ===")
    # **The process name comes first, because it is the first question.** Luma
    # dumps every process that faults, not only the one you were testing, and a
    # dump from some system module resolves against our ELF to plausible
    # nonsense -- addresses in another process's code are still addresses. This
    # was printed last once, and an afternoon went into a crash that turned out
    # to belong to `loader`.
    if procname:
        print(f"process: {procname}")
    print(f"dump format v{major}.{minor}, {total} bytes")
    print(f"ARM{processor & 0xFFFF} core {processor >> 16}, "
          f"{EXCEPTIONS[exc_type] if exc_type < len(EXCEPTIONS) else exc_type}")

    off = 40
    regs = list(struct.unpack_from(f"<{reg_size // 4}I", data, off))
    off += reg_size

    for i, value in enumerate(regs):
        regname = REGNAMES[i] if i < len(REGNAMES) else f"reg{i}"
        where = resolve(value) if regname in ("pc", "lr") else None
        print(f"  {regname:>7} = 0x{value:08X}" + (f"    {where}" if where else ""))

    # **Only a data abort updates DFSR and FAR, and only a prefetch abort
    # updates IFSR.** They are not cleared otherwise, so on any other exception
    # they hold whatever the last fault left there -- often nothing meaningful,
    # sometimes a perfectly convincing address from minutes earlier. Decoding
    # them unconditionally is how an undefined-instruction trap got read as a
    # permission fault on a write to a wild pointer, which is a different bug
    # with a different cause and sent the reading off in the wrong direction.
    if len(regs) > 19:
        if exc_type == 3:  # data abort
            dfsr = regs[17]
            status = ((dfsr >> 10) & 1) << 4 | (dfsr & 0xF)
            print(f"  -> {FAULTS.get(status, f'unknown fault 0b{status:05b}')}, "
                  f"on {'write' if (dfsr >> 11) & 1 else 'read'} to 0x{regs[19]:08X}")
        elif exc_type == 2:  # prefetch abort
            ifsr = regs[18]
            status = ((ifsr >> 10) & 1) << 4 | (ifsr & 0xF)
            print(f"  -> {FAULTS.get(status, f'unknown fault 0b{status:05b}')} "
                  f"fetching 0x{regs[15]:08X}")
        else:
            print("  -> dfsr/ifsr/far are stale: this exception does not set them")

    code, off = data[off:off + code_size], off + code_size
    stack, off = data[off:off + stack_size], off + stack_size
    extra = data[off:off + extra_size]

    pc_word = None
    if code_size:
        pc, thumb = regs[15], bool(regs[16] & 0x20)
        width = 2 if thumb else 4
        count = code_size // width
        # The dump ends at PC; it is the instructions leading up to the fault.
        start = pc - (count - 1) * width
        print(f"code ({'THUMB' if thumb else 'ARM'}), "
              f"0x{start:08X}..0x{pc:08X}:")
        for i in range(count):
            addr = start + i * width
            word = int.from_bytes(code[i * width:(i + 1) * width], "little")
            if addr == pc:
                pc_word = word
            print(f"    0x{addr:08X}: {word:0{width * 2}X}"
                  + ("   <-- pc" if addr == pc else ""))

    if regs[15] < CODE_BASE or regs[15] >= CODE_LIMIT:
        print(f"  -> pc is outside this ELF's 0x{CODE_BASE:08X}..0x{CODE_LIMIT:08X}; "
              "nothing here resolves against it")
        if regs[15] >= SYSMODULE_BASE:
            print(f"     0x{SYSMODULE_BASE:08X} and up is where Luma links its own "
                  f"sysmodules, so this is\n     {procname or 'a system module'}'s "
                  "code and no ELF of ours will resolve it.")

    # **A trap is a decision, not an accident.** Reading this word as "undefined
    # instruction, cause unknown" is how the same dump got filed as a random
    # system module having a bad day; it was loader refusing our image and
    # having no other way to say so.
    if pc_word == ARM_TRAP:
        print("  -> pc is `udf #0` -- GCC's __builtin_trap(). Whatever ran here chose to "
              "stop;\n     this is a deliberate abort, not a wild jump.")
        if procname in LUMA_SYSMODULES:
            print(f"     {procname} is one of Luma's, and its panic() is exactly this one "
                  "instruction,\n     `noinline`, so r0 still holds the Result it was "
                  "handed.")
        else:
            print("     In our own code that is __builtin_trap, std::abort, or a UBSan "
                  "check\n     compiled to trap rather than to a diagnostic.")
        if procname == "loader":
            print(f"     r0 is the Result panic() was handed: 0x{regs[0]:08X}")
            note = LOADER_PANIC.get(regs[0])
            print("     " + note if note else
                  "     Not a Result this reader knows; grep loader's sources for it.")
            if (regs[6], regs[7]) == HBLDR_TID:
                print(f"     r6/r7 hold 0x{regs[7]:08X}{regs[6]:08X}, the hb:ldr 3DSX "
                      "title id, so this is\n     the homebrew path: our .3dsx is what "
                      "loader was refusing.")
            print("     Our code never ran. Check the file *on the card*, not the one "
                  "in\n     build/, with tools/check3dsx.py.")

    if stack_size:
        print(f"stack, {stack_size} bytes from 0x{regs[13]:08X}:")
        for i in range(0, stack_size, 16):
            words = struct.unpack_from(f"<{min(4, (stack_size - i) // 4)}I", stack, i)
            print(f"    0x{regs[13] + i:08X}: " + " ".join(f"{w:08X}" for w in words))
    else:
        print("stack: EMPTY -- the kernel could not read sp, so sp is unmapped."
              "\n       That is a stack overflow or a corrupted sp, not a missing field.")

    if extra_size:
        print(f"process: {procname}")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--elf", help="ELF to resolve pc/lr against")
    args = ap.parse_args()

    resolve = Resolver(args.elf)
    for path in args.dumps:
        parse(path, resolve)


if __name__ == "__main__":
    main()
