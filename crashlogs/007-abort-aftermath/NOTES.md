# 007 — the dump is the aftermath of `abort()`, not the fault that caused it

Reported 2026-09-01 against the build of the same day (the ELF here, archived before the run).
Far Lands, geometry-shader format, large render distance — and reported as *both* shapes at
different times: an exception screen with red text, and a clean drop to the HOME menu.

**Those two are the same event.** That is what this dump establishes, and it is why `005` could not
be resolved and `006` concluded that the failure leaves no artifact at all.

## What the dump says

```
ARM11 core 0, data abort
       sp = 0x0800F8B8
       lr = 0x001917F0    gspEventThreadMain   (gspgpu.c:395)
       pc = 0x00189864    syncArbitrateAddress (synchronization.c:21)
      far = 0x0800F8B4
     dfsr = 0x00000805    -> section translation fault, on write
stack: EMPTY -- the kernel could not read sp, so sp is unmapped.
```

`pc` is the *first instruction* of `syncArbitrateAddress`, which is `str lr, [sp, #-4]!` — the
prologue push. `far` is `sp - 4`. So the thread faulted pushing one word onto its own stack, and
`dfsr` says **section** translation fault: the entire 1 MB at `0x08000000` has no level-1 page table
entry.

`0x08000000` is `OS_HEAP_AREA_BEGIN` — the newlib heap, which `src/platform/ctr/heap.cpp` maps
there. libctru's `threadCreate` takes thread stacks from that heap with `memalign`, and the GSP
event thread is created early, which is why its stack sits ~63 KB in.

**So the newlib heap was unmapped while the GSP event thread was still running on it.**

## The mechanism, disassembled out of this ELF

```
abort:          mov r0,#6 ; bl raise ; mov r0,#1 ; bl _exit
_exit:          b __syscall_exit -> __ctru_exit
__ctru_exit:    bl __appExit ; ldr sp,[__stack_top] ; b __libctru_exit
__appExit:      archiveUnmountAll ; fsExit ; hidExit ; aptExit ; srvExit
__libctru_exit: svcControlMemory(__ctru_heap,        __ctru_heap_size,        MEMOP_FREE)
                svcControlMemory(__ctru_linear_heap, __ctru_linear_heap_size, MEMOP_FREE)
                envDestroyHandles ; __sync_fini ; svcExitProcess
```

**`__appExit` does not call `gspExit`.** `gfxInit`/`gfxExit` belong to the application, not to
libctru's app init, so an exit that does not go through the app's own teardown leaves the GSP event
thread alive. `__libctru_exit` then frees the heap out from under it, and the window between that
`svcControlMemory` and `svcExitProcess` is wide enough for one GSP interrupt to land.

If the thread wakes in that window, it faults and Luma writes this dump. If it does not, the process
reaches `svcExitProcess` and the console drops cleanly to the HOME menu.

**That is a race, and it is the whole explanation for a single failure being reported as two.**

## What this means for the earlier entries

- `006`'s "a clean drop to HOME leaves no artifact" is half right. It leaves no artifact *sometimes*.
  When it does leave one, the dump names `gspEventThreadMain` or `aptEventHandler` — a libctru
  service thread, in a freed heap — and points nowhere near the code that exited.
- `005`'s dumps `08` and `09` are `aptEventHandler` with `sp` unmapped in the same region. Same
  family. Its "consistent with memory exhaustion or corruption and nothing narrower" was as far as
  the evidence went, and this is the narrower thing.
- The dump handed over with the first report of this session was `005`'s `crash_dump_00000009.dmp`
  byte for byte, picked back up off the card. `cmp` said so; it carried no new information and the
  duplicate copy at the top of `crashlogs/` has been removed.

## What is still not known, and it is the whole question

**This dump identifies the *class* of failure and says nothing about its cause.** The registers
belong to the GSP event thread; the thread that called `abort()` is not in the dump at all, and a
244-byte Luma dump holds one thread.

With `-fno-exceptions` the overwhelmingly likely route to `abort` is a failed `operator new` →
`std::terminate` → `abort`, which is exactly `006`'s hypothesis and exactly what
`installOutOfMemoryReporter` was written to catch. **So the next thing to establish is whether
`sdmc:/3dalpha-oom.txt` exists**, because:

- **it does** — the reporter worked, the numbers in it name the consumer, and this stops being a
  hypothesis;
- **it does not** — something reached `abort` by a route that is not `operator new`, and the
  reporter is looking in the wrong place. `abort` calls `raise(SIGABRT)` before `_exit`
  (disassembled above), so a `SIGABRT` handler would catch *every* route rather than one.

Either way the dump itself is now a recognisable fingerprint rather than a mystery: **a fault in a
libctru service thread with `sp` unmapped in the `0x08000000` region means the process called
`abort()`, and the cause is elsewhere.**
