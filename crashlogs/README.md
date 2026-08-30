# Crash logs from hardware

Luma writes these to `luma/dumps/<process>/` on the SD card. They live here rather than under
`build/`, which a clean rebuild deletes.

**Check whose dump it is before anything else.** Luma dumps *every* process that faults, and the
directory name on the SD card says which -- as does the first line the reader now prints. 3DAlpha
appears as **`3dsx_app`**; resolving anyone else's addresses against our ELF produces plausible,
confident nonsense.

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
ELF is linked there, so `addr2line` resolves dump addresses directly -- but *any* build resolves
*any* address to some plausible function, so a later build answers confidently and wrongly. Nothing
detects this; the archived pair is the only defence.

| Dump | Verdict |
|---|---|
| `001-rungame-stack-overflow/` | Stack overflow in `runGame`'s prologue: a 38,220-byte frame on a 32 KB stack, from holding `WorldStreamer` by value. Crashed before the function's first line. |
| `002-vbo-upload-vram/` | `VboPool::upload` `memcpy`ing a section mesh into VRAM. The CPU cannot write VRAM; the VBO pool's VRAM tier is off. |
| `004-loading-a-world-from-the-menu/` | **`C3D_BindProgram` reading the shader program `C2D_Fini` had just freed.** citro3d keeps the last program bound and dereferences it on the *next* bind, so the menu's teardown killed the game's first frame. Symmetric: `Renderer::shutdown` freeing its pipelines would have killed the menu on the way back. Fixed with `ctr::parkShaderProgram()`. **The first dump in this project with its ELF archived beforehand**, which is why the call chain took a minute instead of a reconstruction. |
| `005-lava-flicker-session/` | **Three dumps, two events, no cause established.** All three faulted with their own `sp` unmapped, so none has a call chain: 07 is `ChunkGenerator` construction at world open, 08 and 09 are byte-identical copies of one fault in libctru's `aptEventHandler`. Consistent with memory exhaustion or corruption and nothing narrower. See its `NOTES.md` for what was audited and fixed afterwards -- an unbounded dirty set in `ChunkCache` -- and for why that is a candidate rather than a verdict. |
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

`current-build.elf` is a copy of the ELF matching the `.3dsx` currently in `build/`. If the next
run crashes, that is the one to resolve against -- move it into the new dump's directory before
rebuilding.
