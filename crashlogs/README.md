# Crash logs from hardware

Luma writes these to `luma/dumps/<process>/` on the SD card. They live here rather than under
`build/`, which a clean rebuild deletes.

**Check whose dump it is before anything else.** Luma dumps *every* process that faults, and the
directory name on the SD card says which -- as does the first line the reader now prints. **The
name depends on how the build was launched**: a 3DSX under the Homebrew Launcher is `3dsx_app`,
while an installed CIA is its own title and appears under its own name, **`3DAlpha`** (`010` is
the first of those). Resolving anyone else's addresses against our ELF produces plausible,
confident nonsense.

**And the launch route is itself a variable, not just a label.** A 3DSX runs inside the Homebrew
Launcher's host title and inherits its permissions; a CIA has only what `packaging/3dalpha.rsf.in`
grants it. So "works from hbmenu, dies as a CIA" is a whole class of bug that no amount of reading
our own code will explain -- see `010`.

**But "not ours" is not the same as "not about us".** A dump belonging to `loader` is the one
shape that is still entirely our problem: `LoadProcess` wraps the homebrew loader in
`assertSuccess`, and `assertSuccess` calls `panic`, which is a bare `__builtin_trap()`. Luma has
no way to tell the Homebrew Launcher "I will not load that file", so it traps instead -- and a
**rejected** 3DSX and a **crashing** one look identical from the couch: black screen, red text.
`003-loader-rejected-the-3dsx/` is that, and it was filed as somebody else's bad day for a while.

`tools/lumadump.py` now names it: `udf #0` at a pc above `0x14000000` is Luma's `panic`, and `r0`
is the Result it was handed. `0xFFFFFFFF` means `Ldr_Get3dsxSize` said no -- loader opened the
file and could not read 32 bytes of header out of it. `tools/check3dsx.py` runs the same checks
here, so the question "is the image bad or is the copy on the card bad?" has an answer without a
console.

Read one with:

    tools/lumadump.py crashlogs/<dir>/*.dmp --elf crashlogs/<dir>/*.elf

**Keep the `.elf` that produced the crash beside the `.dmp`.** A 3DSX loads at `0x00100000` and the
ELF is linked there -- and a CIA's ExeFS `.code` loads at the same address, so the same ELF resolves
either one -- so `addr2line` resolves dump addresses directly -- but *any* build resolves
*any* address to some plausible function, so a later build answers confidently and wrongly. Nothing
detects this; the archived pair is the only defence.

| Dump | Verdict |
|---|---|
| `001-rungame-stack-overflow/` | Stack overflow in `runGame`'s prologue: a 38,220-byte frame on a 32 KB stack, from holding `WorldStreamer` by value. Crashed before the function's first line. |
| `002-vbo-upload-vram/` | `VboPool::upload` `memcpy`ing a section mesh into VRAM. The CPU cannot write VRAM; the VBO pool's VRAM tier is off. |
| `004-loading-a-world-from-the-menu/` | **`C3D_BindProgram` reading the shader program `C2D_Fini` had just freed.** citro3d keeps the last program bound and dereferences it on the *next* bind, so the menu's teardown killed the game's first frame. Symmetric: `Renderer::shutdown` freeing its pipelines would have killed the menu on the way back. Fixed with `ctr::parkShaderProgram()`. **The first dump in this project with its ELF archived beforehand**, which is why the call chain took a minute instead of a reconstruction. |
| `005-lava-flicker-session/` | **Three dumps, two events, no cause established.** All three faulted with their own `sp` unmapped, so none has a call chain: 07 is `ChunkGenerator` construction at world open, 08 and 09 are byte-identical copies of one fault in libctru's `aptEventHandler`. Consistent with memory exhaustion or corruption and nothing narrower. See its `NOTES.md` for what was audited and fixed afterwards -- an unbounded dirty set in `ChunkCache` -- and for why that is a candidate rather than a verdict. |
| `006-farlands-flight/` | **No dump, and that is the verdict.** Flying at the Far Lands in the geoshader format dropped the console cleanly to the HOME menu. That shape is `abort()`, and with `-fno-exceptions` a failed `operator new` becomes exactly that — so it leaves no exception screen and nothing on the card. Holds the ELF of the build that did it and the reasoning; see its `NOTES.md`. An out-of-memory reporter now writes `sdmc:/3dalpha-oom.txt` so the next one is not silent. |
| `007-abort-aftermath/` | **The dump is the aftermath of `abort()`, not the fault that caused it.** `gspEventThreadMain` faulting in its own prologue with `sp` unmapped in the `0x08000000` region -- the newlib heap, freed by `__libctru_exit` while the GSP event thread was still running on it, because `__appExit` does not call `gspExit`. **This is what settles `005` and corrects `006`: the exception screen and the clean drop to HOME are the same event**, decided by whether one GSP interrupt lands between the heap free and `svcExitProcess`. Says nothing about *what* called `abort`. |
| `008-out-of-heap/` | **The out-of-memory reporter fired, and the obvious reading of it is wrong.** `free 4405k of 40960k  blocks 10688k  cols 382` — so the newlib heap is confirmed as what runs out (`006`'s hypothesis, settled), but **resident columns are only 29 % of it** and the grid had not finished loading. The larger half is the chunk cache, the generator's cache and overhead, and the reporter named none of them. The fix for this one is the heap/linear split, not the column budget. |
| `009-save-with-a-minecart/` | **A use-after-free of `TickWorld`, not a storage fault.** Two dumps, one event in two builds: a prefetch abort with `pc == r0 == r3` pointing into **newlib's `__malloc_av_`** — the freed-chunk `fd`/`bk` of a `TickAccess`, whose first two fields are a context and a function pointer. `WorldStreamer::close` destroyed `tick_` *before* the drain loop whose progress callback draws a whole frame, and the minecart pass borrows the tick world. Neither dump has its ELF; the byte pattern of the malloc bins is what settles it. **Corrects the command-buffer verdict a previous session filed these under.** |
| `010-cia-dsp-memory-unmapped/` | **Not a bug in the binary -- a permission the CIA did not ask for.** `ndspInitialize` storing through a pointer into DSP DRAM at `0x1FF57FFE`, which is mapped only for a process whose exheader says so. A 3DSX inherits the mapping from the Homebrew Launcher's host title, so the identical ELF boots from hbmenu and dies as a CIA. `DSP_ConvertProcessAddressFromDspDram` is arithmetic and returns success regardless, so `audio.cpp`'s graceful no-firmware path never runs -- **the crash only reaches players whose audio would have worked**. Fixed by `IORegisterMapping: 1ff00000-1ff7ffff` in the RSF; verified by diffing the kernel capability descriptors of two CIAs built from one ELF. First dump in the set about packaging rather than code. |
| `011-cia-import-world/` | **Another permission the CIA did not ask for: VRAM, read-only.** The Import World keyboard died in libctru's `aptConvertScreenForCapture`, reading `0x1F4C7800` while copying the bottom screen for the applet. GSP reported that screen's `framebuf0` in VRAM, although our framebuffers are linear. The Homebrew Launcher's host title maps VRAM, so the 3DSX reads it harmlessly. Fixed by `MemoryMapping: 1f000000-1f5fffff:r`, the line that sits next to `010`'s in the standard template. The ELF is a rebuild of `5da0f66`, checked byte-identical against the installed CIA's code. |
| `003-loader-rejected-the-3dsx/` | **`loader` refusing to load our 3DSX**, not a crash in it. `panic()` on core 1 with `r0 = 0xFFFFFFFF`, which is `Ldr_Get3dsxSize` failing to read the file's 32-byte header, and the hb:ldr title id in `r6`/`r7`. Our code never ran, so nothing about the build's *contents* is implicated -- the file loader opened was empty, truncated or not a 3DSX. First filed as "not ours"; that verdict was wrong. |

## Neither set has its ELF, and that is the lesson

`001` is a reconstruction: those dumps sat in `build/` and a clean rebuild deleted them. They were
rebuilt byte-for-byte from a hexdump taken beforehand; the ELF was not. `crash_dump_00000000` in
that set is unrelated to 3DAlpha -- process `pm`, an older Luma dump-format revision, already on
the card when the app first ran.

`002` lost its ELF the same way, to a rebuild that happened *after* the rule above was written
down. What survives is `analysis.txt` beside the dump, holding the resolved call chain and the
register reading. That is the habit worth keeping either way:

    cp build/<ver>/*.elf crashlogs/<dir>/     # first, before touching anything
    tools/lumadump.py crashlogs/<dir>/*.dmp --elf crashlogs/<dir>/*.elf \
        > crashlogs/<dir>/analysis.txt

The resolution is the artifact. The ELF is only how you get it, and it is the part that a `make`
quietly destroys.

**A crash that leaves no dump is still a crash with a signature.** Luma writes a dump for a *fault*
— a data abort, an undefined instruction. It writes nothing for a process that exits, and this
binary has one way to exit that is not the player's: `abort()`, which is where a failed
`operator new` lands with `-fno-exceptions`. So the first question about any hardware death is not
"where is the dump" but **which of the three shapes it was**, because two of them never produce one:

| what the console did | what it is | where to look |
|---|---|---|
| exception screen, red text | a fault — data abort, bad instruction | `luma/dumps/3dsx_app/`, or `luma/dumps/3DAlpha/` for a CIA, resolved against the archived ELF |
| frozen or black, HOME dead | a hang — the main thread is blocked, usually on the GPU | no dump exists; the bottom screen is the channel, see `geoTrace` |
| **clean drop to the HOME menu** | `abort()` — out of heap, or a deliberate one | `sdmc:/3dalpha-oom.txt`, see `006` |

**The first and third rows overlap, and `007` is why.** `abort()` reaches `__libctru_exit`, which
frees the newlib heap at `0x08000000` — and `__appExit` never calls `gspExit`, so the GSP event
thread is still alive on a stack inside it. If one GSP interrupt lands before `svcExitProcess`, that
thread faults and you get an exception screen; if none does, the console drops cleanly to HOME. Same
event, two appearances, decided by a race.

So there is a fourth thing to recognise, and it is a **dump that is not about the code that failed**:

| the dump says | what it actually means |
|---|---|
| a fault in `gspEventThreadMain`, `aptEventHandler` or any libctru service thread, **with `sp` unmapped in the `0x08000000` region** | the process called `abort()`. The thread in the dump is collateral; the one that failed is not in it. Go to `sdmc:/3dalpha-oom.txt` and to `006`/`007`, not to the registers |

`current-build.elf` is a copy of the ELF matching the `.3dsx` currently in `build/`. If the next
run crashes, that is the one to resolve against -- **move it into the new dump's directory before
rebuilding**, which is also what commits it: it is in `.gitignore` and the archived pairs are not.
Untracked is the point. A file that is replaced on every build is a new 27 MB blob in the history
every time, and it is worth nothing to anyone who clones -- their `build/` holds something else.
The pair inside a dump directory is the artifact; this is the spare you have not needed yet.
